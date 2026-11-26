# Fork backlog

What is open, waiting or parked for this fork, and what was done recently. Keep it current: add an
item when work is proposed, move it when its state changes, and drop "Done" entries once they are
older than a few releases (the commits and `README.md` keep the details). Every design item is sized
against the console limits in `AGENTS.md` ("Design against the console's limits").

Last updated: 2026-11-26 (wrapper 2.1.0-bale.84).

## Open: fork work

| Item | State | Why / sized against |
|---|---|---|
| rav1g route: avoid the per-frame copy of direct output into the player pool | Idea | One full-frame GPU copy per frame (GPU copies are ~2.4x slower in the idle state). The copy exists because handing decoder images to Media Foundation deadlocked the fixed image pool (bale.45), so any change needs a new design for that. |
| Upstream PR #487: the source holds strong references to itself while playing, and handles the stream source's Closed event (issue #486, teardown crashes) | Wait for upstream to merge | Changes when the source is freed. Bale closes it explicitly, so it matters less here; if the source outlived Bale's close, a 4K decoder pool (~190–480 MiB) would stay allocated against the 3328 MiB app limit. Take it with a console open/close loop. |

## Waiting on others

| Item | Waiting for |
|---|---|
| Audio passthrough plumbing (debug): `bale_audio_out=<id>`, `bale_audio_out_stream=<index>`; audio not exposed to MF; the fork decodes the chosen track on its own feeder thread and writes it with pts to bale_gpu's exclusive WASAPI renderer; flush on seek and stream switch | ABI landed (Bale 9e216fe, `bale_audio_out.h` ABI 1): id 1–65535; write(id, data, bytes, pts_100ns) of whole frames returns 0 = full (retry) or 0xFFFFFFFF = closed (stop and fall back); `bale_audio_out_format(id)` gives the format (TV path: PCM 8 ch 48 kHz, s32 container with 24 valid bits, mask 0x63F, FL FR FC LFE BL BR SL SR, the same order as FFmpeg's 7.1). Build only when the owner asks. |
| rav1g untested paths | Console runs: HDR passthrough (needs an HDR display), 8-bit NV12 direct output (shared UAV NV12 not yet confirmed on the console), film-grain films. |
| rav1g as the production AV1 default | Owner's decision. Bale's debug builds already use it; release builds keep dav1d. |

## Parked or on hold (owner decisions)

- **HEVC/H.264 passthrough media-type attributes (sequence header, level, interlace, NALU length):** not needed. The first-10-bit-after-boot slowdown no longer reproduces (likely an OS update), and the 8 vs 21 attributes made no difference (Bale `player-plan.md`, 2026-11-23).
- **Upstream pull requests:** not before Bale is stable, and only with the owner's approval.
  Candidates to offer then: the stream-sized d3d11va pool (4K HEVC 478 → 191 MiB), threaded swscale for formats beyond 4:2:0 8-bit (pairs with upstream PR #485), dithered 8-bit output, dav1d GPU film grain, and a generic processed GPU-frame path (upstream issues #465, #467). Upstream issue #460 (2x speed with the system decoder): check against Bale's passthrough route.
- **D3D12 / SM6 tone map:** parked by the owner (2026-11-18).
- **D3D12 video decoding (FFmpeg d3d12va; upstream PR #484):** parked by the owner for a stable release first (2026-11-25). The console supports it (bale.83 probe: HEVC Main10 4K, H.264, VP9 1080p/4K; feature level 11_0 max, tier 2). Branch `test/d3d12-decode` (484ef35, builds in CI, never run on the console) has `bale_d3d12_decode`: an 11_0 device on Media Foundation's adapter, shared-heap frames opened by D3D11, GPU-only fence sync, then the existing d3d11va route. Expected gains are small: the stage already reads d3d11va frames in place, so no copy is saved there; the A/B would compare decode time, CPU and decoder memory. Next step when resumed: rebase, publish a test package with the owner's OK, console A/B with the key off and on. Pure D3D12 would need Bale's own player instead of `MediaPlayer` (Media Foundation takes D3D11 surfaces only).
- **rav1g pipeline cache (ABI 2, `rav1g_device_create_with_cache`):** on hold; creating the device with the cache removed the GPU device on the console.
- **Caller-supplied duration for forward-only inputs (engine player):** idea, never requested.
- **AC-3 audio encoding for optical / older HDMI ARC soundbars (owner: one of the project's last steps, 2026-11-25):** they take only compressed surround (AC-3/DTS), not multichannel PCM. Needs only FFmpeg's AC-3 encoder (`--enable-encoder=ac3`, LGPL; native builds keep `--disable-encoders` today), so a change to Bale's native build flags with the owner's approval, plus an encode step in the audio passthrough feeder. Small binary, little CPU (sized against the 4 shared cores).
- **Interlaced video through the stage (HDR, or larger than the display):** untested. Without the stage, 1080i H.264 and MPEG-2 are deinterlaced by Media Foundation on all three routes (Bale console check, 2026-11-23). The stage reads the woven frame, so combing is possible there. Rare; look at it if it shows up.
- **dav1d zero-copy seek out-of-memory (4K AV1 normal seek exhausts the upload pool):** known, unfixed by choice; Bale uses fast seek for AV1 and the dav1d route no longer uses zero-copy.

## Owner to-dos

- Check whether ~115 MB of FFmpeg PDBs ship in Bale's MSIX.

## Done recently

| Release | What |
|---|---|
| bale.83 | D3D12 probe creates its device at feature level 11_0 and hands it to FFmpeg (bale.82's probe failed at FFmpeg's 12_0 device creation) |
| bale.84 | In-band caption packet tap `bale_cc_sink` (H.264/HEVC/MPEG-2 packets to bale_gpu `bale_cc_push`, flush on every stream flush). Console (Bale 0.1.790/791, as Bale reported): worked on the six cea608 clips (H.264 MP4/MKV/TS), `tap on (codec 1 nal_length_size 4)`, packet and flush counts as expected |
| (bale.60 check) | dav1d D3D11 GPU grain (`bale_film_grain_mode=3`) closed: on the console with Bale 0.1.791 and bale.84, 1080p60 10-bit AV1 grain clips: `mode=gpu-d3d11`, 0 missed frames, Device Portal CPU ≈12–14% steady vs ≈30–53% bursty with dav1d grain; GPU vs CPU grain on the same paused frame RMSE 0.73% (each vs grain off 1.24–1.32%), visually identical |
| bale.82 | Native FFmpeg 9.1 (dav1d 1.5.5, no patches); upstream crash fixes #488/#489; opt-in `bale_d3d12va_probe`. Console regression passed (Bale 0.1.778: rav1g, HEVC 4K HDR/SDR, H.264, VP9, MPEG-2, 5 software routes, seeks, HLS; 0 missed frames). FFmpeg's pools unchanged (HEVC/H.264 20, VP9 12), so the +3 assumption holds |
| Bale (2026-11-22) | Stage fix in bale_gpu: 4K60 AV1 on rav1g 47.1 → 57.6 fps (missed 21.8% → 4.5%), same as without the stage. Owner approved `bale_d3d11va_pool_from_stream` as the HEVC default (debug builds now, release after a long HEVC play). |
| bale.81 | Threaded swscale (sws_scale_frame, up to 4 threads) for software frames beyond 4:2:0 8-bit |
| bale.80 | Software frames through the stage (tone map, downscale, depth, white) on the staged D3D11 upload; console-verified with bale.81 (4K H.264 → 1080p, 25 opens without the leak, 4:2:2 HDR10 tone map) |
| bale.79 | Decoder pool from in-band parameter sets (MKV with a 23-byte hvcC): 4K HEVC 20 → 9 surfaces; console-verified, plus a 30-min HEVC HDR10 soak. Now Bale's release default (8ed6c50) |
| bale.78 | rav1g interface version 3 (v0.8.0-alpha.1): container HDR peaks to the display pass, peak log, fixed-peak/bilinear-chroma A/B keys; console-verified (452-nit film) |
| bale.77 | `bale_tonemap_white_nits` (SDR white, 80–400) for the stage and rav1g's display pass; console-verified on both routes |
| bale.76 | Retired the direct decoder-frame read experiment (`bale_d3d11va_shader_resource` accepted, ignored) |
| bale.75 | Display pass: keep the stage when the device can't create shader-writable display images (never seen on the console) |
| bale.74 | rav1g interface version 2; opt-in rav1g display pass (`bale_rav1g_display`) in place of the stage on the rav1g route. Console: 4K60 AV1 60 Mbps 0% missed (SDR and HDR10; stage 3.4–4.9% / 12–13%), pass 1.0/1.18 ms, images 47.5 vs 237.3 MiB |
| bale.73 | Per-frame `Bale rav1g frame u= pts= ...` line from av1_perf_frame (rav1g v0.6.0-alpha.3, 632 bytes) |
| bale.72 | `bale_rav1g_perf=2`: per-frame stage line (take, submit, release, gpudone; QPC ms; u and pts) |
| bale.71 | Decoder pool: compare with FFmpeg's real pool (parameters + 3); console-verified for VP9 (12 → 11) and H.264 (1080p 20 → 8); every branch logged with a `Bale ` prefix |
| bale.70 | `bale_gpu_output_images` (4–16) and who holds the images at a full-pool wait; console: the waits are pending D3D11 work, Bale keeps 10 |
| bale.69 | Decoder pool from the stream also for H.264 (SPS/VUI or level) and VP9 (8 + 1 + 2) |
| bale.68 | d3d11va decoder pool sized from the HEVC SPS (opt-in); console: 4K HEVC 20 → 8 surfaces (478 → 191 MiB), no misses or errors |
| bale.67 | rav1g image pool takes an image whose D3D11 readers are done (or creates one) instead of the first free one; logs `image release wait` |
| bale.66 | rav1g delivery line: decode-thread waits (output, slot, fb, image), entropy wall, packet read and decode-call gap, for the 4K60 stalls |
| bale.65 | rav1g headers from v0.6.0-alpha.1 (perf record 624 bytes) |
| bale.64 | Shader-readable decoder pools only when bale_gpu reads them in place (bit 64) |
| bale.63 | Probe for shader-readable decoder pools; passed on the console |
| bale.62 | GPU memory ledger (bale_mem_account, categories 16–22) |
| bale.61 | Dithered 8-bit stage output (`bale_tonemap_output_depth=8`) |
| bale.60 | D3D11 GPU film grain for dav1d without zero-copy (`bale_film_grain_mode=3`) |
| bale.59 | Height limit for the software AV1 fallback (`bale_av1_fallback_max_height`) |
| bale.55–58 | rav1g memory logging, player texture cap, 10 output images, header updates |
