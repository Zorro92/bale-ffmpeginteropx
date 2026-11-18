# Design: rav1g AV1 provider (draft for review)

Status: proposal, 2026-11-18. Nothing is implemented. Owner, Bale and rav1g review this before
any code. Goal: real-world testing of [rav1g](https://github.com/Zorro92/rav1g) (GPU AV1 decoding in
D3D12 compute) inside Bale, **behind a debug switch**. Production keeps dav1d zero-copy until rav1g
has proven itself; then dav1d becomes a software fallback limited to 1080p.

## 1. Where it sits

- New generic provider `GpuDecoderSampleProvider` (no rav1g names): takes demuxed packets from
  `FFmpegReader`, hands Media Foundation one D3D11 texture per frame plus `VideoFrameColorInfo`.
  It reuses `VideoFrameProcessor` (the tone-map/downscale stage) and the `TexturePool` pattern of
  `D3D11VideoSampleProvider`.
- Bale/rav1g glue in one seam, `Source/BaleRav1g.h`: loads `rav1g.dll` from the app package
  (`LoadPackagedLibrary`, once per process), checks the ABI version, mirrors `rav1g.h` verbatim
  (copied from the rav1g repo, like `bale_tonemap.h`), and implements the provider's decoder
  interface.
- Selected in `FFmpegMediaSource::CreateVideoSampleProvider` for AV1 when
  `bale_av1_decoder=1` (new opt-in key; 0 = today's dav1d path). Production never sets it.

## 2. rav1g lifecycle

| Object | Created | Destroyed |
|---|---|---|
| D3D12 device + queue | first rav1g stream of the process, on the adapter of MF's D3D11 device (`IDXGIDevice::GetAdapter`) | process exit (shared) |
| `Rav1gDevice` | with the D3D12 device (`rav1g_device_create`) | process exit |
| `Rav1gDecoder` | first frame of a stream (MF device known), and after each seek (see §5) | stream end / seek |

Settings: `rav1g_settings_default`, then only `output_kind = RAV1G_OUTPUT_SEMI_PLANAR`,
`shared_fence = true`, the output callbacks and `memory_available` (= `AppMemoryUsageLimit −
AppMemoryUsage`, from a callback the app registers; 3328 MiB limit on Series X with
`hevcPlayback`). `low_memory` stays at rav1g's default (16: 4K device buffers ≈ 538 MiB).
After create: `rav1g_decoder_output_fence` → `CreateSharedHandle` → `ID3D11Device5::OpenSharedFence`.

## 3. Packet → frame flow

Pull model on the MSS sample-request thread, like the other providers:

1. If a decoded frame is queued, take it (step 4).
2. Otherwise read the next AV1 packet and call `rav1g_decoder_decode(data, size, user_data)`,
   where `user_data` = an index into a small table of {pts, duration, packet flags}. Repeat until a
   frame is queued or the packet buffer needs refilling. At end of stream `rav1g_decoder_drain`.
3. `frame` callback (rav1g completion thread): push {texture pair, fence value, colour info,
   user_data} into a bounded queue, signal a condition variable. No D3D11 calls on that thread.
4. On the sample thread: `ID3D11DeviceContext4::Wait(fence11, fence_value)` (GPU-side), then either
   hand the texture to MF as the sample, or run the stage (§4) and hand its output. Timestamps from
   the `user_data` table.

`get_image` (decode thread or a rav1g thread): take a free pair from the pool, return its D3D12
side; if none is free, return failure so rav1g waits (rav1g retries / reports; to confirm with rav1g).

## 4. Output path

| Source | Display | Output to MF |
|---|---|---|
| 8-bit SDR | ≥ video size | rav1g's NV12 texture directly (source depth) |
| 10-bit SDR | ≥ video size | rav1g's P010 texture directly |
| SDR | smaller than video | stage, downscale only (`BALE_TONEMAP_SDR709`), P010 at display size, marked |
| HDR (PQ/HLG) | SDR display | stage, tone map (+ downscale when larger), P010, marked |
| HDR | HDR display | rav1g's P010 directly, with HDR attributes |

The stage reads rav1g's texture directly (it has `SHADER_RESOURCE`), so no slice copy. Bit-depth
rule holds: 8-bit stays NV12 unless the stage downscales; this also ends the accepted 8-bit
AV1 → P010 exception of the zero-copy path.

## 5. Texture pool, seeking, memory

- **Pool:** fixed size N (proposal: frames in flight in rav1g + 4 for MF, e.g. 8). Pairs of a D3D11
  texture (`RENDER_TARGET | SHADER_RESOURCE | UNORDERED_ACCESS`, `MISC_SHARED_NTHANDLE |
  MISC_SHARED` — the flags MF already accepts on the zero-copy path, plus UAV for rav1g's direct
  write) opened in D3D12. Returned to the pool on the sample's `Processed` (stored texture, as in
  565ee8c). **Never grows**: an exhausted pool makes rav1g wait instead of allocating, which avoids
  the zero-copy seek out-of-memory (`known issue`, full seeks of 4K software AV1).
- **Seek:** rav1g's public API has no flush. Proposal: drain and destroy the decoder, recreate on
  the next frame (device stays). Bale keeps FastSeek for AV1 (keyframe landing), so no frames
  before the target need dropping; if exact seeks are used, frames whose pts ends before the target
  are returned to the pool without being sent. **Question for rav1g:** a flush/reset call, and the
  cost of recreate.
- **Memory at 4K:** rav1g ≈ 538 MiB + pool 8 × 4K P010 ≈ 200 MiB + stage output (display-sized)
  ≈ 30 MiB + Bale/FFmpeg ≈ 0.6 GiB → about 1.4–1.8 GiB of 3328 MiB. `memory_available` lets rav1g
  pick fewer slots when tight.

## 6. Failure and fallback

- `rav1g.dll` missing, ABI mismatch, device or decoder creation refused at open → no rav1g
  provider; the stream falls back to today's dav1d path (logged once).
- Error during a stream (`rav1g_decoder_decode`/`drain` status, device removed, fence timeout) →
  fatal latch → `NotifyError` → Bale replans (dav1d ≤1080p or server).
- Logs: `Bale rav1g: active WxH bits=N slots=S`, `Bale rav1g: fallback to dav1d (reason, status)`,
  `Bale rav1g: stream failed (status/hr)`, pool counters at destroy.

## 7. Who does what

- **Fork (this repo, Codex):** `GpuDecoderSampleProvider`, `BaleRav1g.h`, option, pool, stage reuse,
  logs; one release behind `bale_av1_decoder`.
- **Bale:** ship `rav1g.dll` in the package; debug switch setting `bale_av1_decoder=1`; register
  the memory callback; output guard rules for the rav1g route (direct NV12/P010 SDR is not
  tone-mapped, so it carries no tone-map mark — the guard needs a rule for it); console tests;
  later the policy (dav1d capped at 1080p as fallback).
- **rav1g:** API stability; answers to the questions below.

## 8. Open questions

1. rav1g: flush/reset for seeks, or is drain + destroy + recreate the intended path (cost)?
2. rav1g: `get_image` returning "no image" — does the decoder wait and retry, or fail?
3. rav1g: one `Rav1gDevice` per process for successive streams — any state to reset between them?
4. Bale: guard rule for direct (unmarked) rav1g NV12/P010 SDR output.
5. Owner: pool size N, and whether HDR → HDR display should pass rav1g's P010 straight through.

## 9. Test plan (console, debug switch on)

1. md5/visual parity with dav1d on the AV1 clips Bale uses (8-bit, 10-bit, film grain, 1080p, 4K).
2. Heavy 4K in real time, CPU and GPU cost, memory peak.
3. HDR AV1 tone-mapped on SDR; 4K SDR AV1 downscaled (40 opens, the platform-leak check).
4. Seeks (forward/back, 20×), stop/start cycles (20×), pool counters steady.
5. Fallback paths: missing DLL, forced create failure.
