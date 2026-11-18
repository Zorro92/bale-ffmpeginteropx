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
  With it set, `bale_zero_copy`, `bale_d3d11_upload` and `bale_film_grain_mode` are ignored for that
  stream (rav1g applies film grain itself); they still apply if it falls back to dav1d.

## 2. rav1g lifecycle

| Object | Created | Destroyed |
|---|---|---|
| D3D12 device + queue | first rav1g stream of the process, on the adapter of MF's D3D11 device (`IDXGIDevice::GetAdapter`) | process exit (shared) |
| `Rav1gDevice` | with the D3D12 device (`rav1g_device_create`) | process exit |
| `Rav1gDecoder` | first frame of a stream (MF device known); recreated only when rav1g reports `NEEDS_RECONFIGURE` (~0.11 s create + 0.04 s destroy at 4K) | stream end |

Settings: `rav1g_settings_default`, then only `output_kind = RAV1G_OUTPUT_SEMI_PLANAR`,
`shared_fence = true`, the output callbacks and `memory_available` (= `AppMemoryUsageLimit −
AppMemoryUsage`, read in-process from `Windows.System.MemoryManager`; 3328 MiB limit on Series X
with `hevcPlayback`; no app callback, since FFmpegOptions can't carry one). `low_memory` stays at rav1g's default (39: 4K device buffers ≈ 501.5 MiB). Width, height and bit
depth come from `rav1g_peek_stream_info` on the first temporal unit (max frame size). The pool is
created after the decoder, so `memory_available` also holds back the pool's bytes.
After create: `rav1g_decoder_output_fence` → `CreateSharedHandle` → `ID3D11Device5::OpenSharedFence`
(each decoder has its own fence: re-open it after a recreate). `rav1g_set_*` are process-global:
the provider never calls them. One `Rav1gDevice` serves every stream of the process; a removed
device means recreating device, decoder and pool (the stream fails, §6).

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

`get_image` (decode thread or a rav1g thread): take a free pair from the pool and return its D3D12
side. If none is free it **blocks** until MF returns one: a non-zero return fails the temporal unit
(references break until the next key frame), so it returns non-zero only to cancel on stop or seek.
Pairs match the request (size and NV12/P010 by bit depth); streams that change size get pairs of
the new size, old ones are released when they come back.

## 4. Output path

| Source | Display | Output to MF |
|---|---|---|
| 8-bit SDR | ≥ video size | rav1g's NV12 texture directly (source depth), rav1g mark |
| 10-bit SDR | ≥ video size | rav1g's P010 texture directly, rav1g mark |
| SDR | smaller than video | stage, downscale only (`BALE_TONEMAP_SDR709`), P010 at display size, marked |
| HDR (PQ/HLG) | SDR display | stage, tone map (+ downscale when larger), P010, marked |
| HDR | HDR display | rav1g's P010 directly, with HDR attributes, rav1g mark |

Marks: direct rav1g output carries a new private media-type attribute (GUID chosen by Bale, value
1, like the tone-map mark); stage output keeps the tone-map mark. Bale's guard accepts direct
output on this route only with the rav1g mark, a subtype matching the source depth (8 → NV12,
10 → P010) and the source colour attributes.

The stage reads rav1g's texture directly (it has `SHADER_RESOURCE`), so no slice copy. Bit-depth
rule holds: 8-bit stays NV12 unless the stage downscales; this also ends the accepted 8-bit
AV1 → P010 exception of the zero-copy path.

## 5. Texture pool, seeking, memory

- **Pool:** fixed size N = rav1g's images in flight (slots + 2, i.e. 5 at 4K) + MF's queue (~4) +
  1 in our queue, so about 10 at 4K. Pairs of a D3D11
  texture (`RENDER_TARGET | SHADER_RESOURCE | UNORDERED_ACCESS`, `MISC_SHARED_NTHANDLE |
  MISC_SHARED` — the flags MF already accepts on the zero-copy path, plus UAV for rav1g's direct
  write) opened in D3D12. Returned to the pool on the sample's `Processed` (stored texture, as in
  565ee8c). **Never grows**: an exhausted pool makes rav1g wait instead of allocating, which avoids
  the zero-copy seek out-of-memory (`known issue`, full seeks of 4K software AV1).
- **Seek:** no flush or recreate. Bump a generation counter (part of `user_data`), cancel any blocked
  `get_image`, `rav1g_decoder_drain` (the decoder stays usable), return every frame of an older
  generation to the pool unsent, then feed from the key frame FFmpeg's seek landed on. Bale keeps
  FastSeek for AV1 (keyframe landing); with exact seeks, frames ending before the target are also
  returned unsent.
- **Memory at 4K:** rav1g ≈ 502 MiB + pool 10 × 4K P010 ≈ 250 MiB + stage output (display-sized)
  ≈ 30 MiB + Bale/FFmpeg ≈ 0.6 GiB → about 1.4–1.8 GiB of 3328 MiB. `memory_available` lets rav1g
  pick fewer slots when tight.

## 6. Failure and fallback

- `rav1g.dll` missing, ABI mismatch, device or decoder creation refused at open → no rav1g
  provider; the stream falls back to today's dav1d path (logged once). Bale shows it in stats.
- Error during a stream (`rav1g_decoder_decode`/`drain` status, device removed, fence timeout) →
  fatal latch → `NotifyError`. Today Bale's single replan withdraws all software claims (server
  transcode); for testing Bale adds a one-time same-plan remount on dav1d instead.
- Logs: `Bale rav1g: active WxH bits=N slots=S`, `Bale rav1g: fallback to dav1d (reason, status)`,
  `Bale rav1g: stream failed (status/hr)`, pool counters at destroy.

## 7. Who does what

- **Fork (this repo, Codex):** `GpuDecoderSampleProvider`, `BaleRav1g.h`, option, pool, stage reuse,
  logs; one release behind `bale_av1_decoder`.
- **Bale:** ship `rav1g.dll` pinned by release tag + SHA256 (like ffmpeg-native), with a CI
  export/version check and a THIRD_PARTY_NOTICES entry; debug switch setting `bale_av1_decoder=1`
  (skipping its zero-copy registration requirement); the rav1g mark GUID and guard rule (§4); the
  same-plan dav1d remount (§6); console tests; later the policy (dav1d capped at 1080p as fallback).
- **rav1g:** a release asset for `rav1g.dll` that Bale can pin.
- **rav1g:** API stability; answers to the questions below.

## 8. Open questions

1. ~~rav1g: seeks~~ — answered: drain + generation, no recreate (§5; rav1g docs/integration.md, 52d43ea).
2. ~~rav1g: get_image with no free image~~ — answered: block; non-zero fails the temporal unit (§3).
3. ~~rav1g: device reuse~~ — answered: nothing to reset; re-open each decoder's fence (§2).
4. ~~Bale: guard rule for direct rav1g output~~ — answered: a rav1g mark (§4).
5. Owner: pool size N (now ≈ 10 at 4K from rav1g's numbers; Bale suggested 8), and whether HDR → HDR display passes rav1g's
   P010 straight through with HDR attributes (Bale recommends yes; untestable on the SDR monitor).

## 9. Test plan (console, debug switch on)

1. md5/visual parity with dav1d on the AV1 clips Bale uses (8-bit, 10-bit, film grain, 1080p, 4K).
2. Heavy 4K in real time, CPU and GPU cost, memory peak.
3. HDR AV1 tone-mapped on SDR; 4K SDR AV1 downscaled (40 opens, the platform-leak check).
4. Seeks (forward/back, 20×), stop/start cycles (20×), pool counters steady.
5. Fallback paths: missing DLL, forced create failure.
