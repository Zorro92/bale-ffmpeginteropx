# Fork backlog

What is open, waiting or parked for this fork, and what was done recently. Keep it current: add an
item when work is proposed, move it when its state changes, and drop "Done" entries once they are
older than a few releases (the commits and `README.md` keep the details). Every design item is sized
against the console limits in `AGENTS.md` ("Design against the console's limits").

Last updated: 2026-11-23 (wrapper 2.1.0-bale.76).

## Open: fork work

| Item | State | Why / sized against |
|---|---|---|
| rav1g route: avoid the per-frame copy of direct output into the player pool | Idea | One full-frame GPU copy per frame (GPU copies are ~2.4x slower in the idle state). The copy exists because handing decoder images to Media Foundation deadlocked the fixed image pool (bale.45), so any change needs a new design for that. |
| FFmpeg 9.x | Deferred | 9.0/9.1 bring nothing the fork needs (HEVC hardening, a driver-compat SRV flag). Easier once Bale drops dav1d zero-copy, so native FFmpeg needs no dav1d patch (`p1`). |

## Waiting on others

| Item | Waiting for |
|---|---|
| dav1d D3D11 GPU grain (`bale_film_grain_mode=3`, bale.60) | Owner: visual check on the TV. Console: works for NV12 and P010, 1.11 vs 1.51 cores (1080p 10-bit, Series X). |
| rav1g untested paths | Console runs: HDR passthrough (needs an HDR display), 8-bit NV12 direct output (shared UAV NV12 not yet confirmed on the console), film-grain films. |
| rav1g as the production AV1 default | Owner's decision. Bale's debug builds already use it; release builds keep dav1d. |

## Parked or on hold (owner decisions)

- **HEVC/H.264 passthrough media-type attributes (sequence header, level, interlace, NALU length):** not needed. The first-10-bit-after-boot slowdown no longer reproduces (likely an OS update), and the 8 vs 21 attributes made no difference (Bale `player-plan.md`, 2026-11-23).
- **Upstream pull requests:** not before Bale is stable, and only with the owner's approval.
- **D3D12 / SM6 tone map:** parked by the owner (2026-11-18).
- **rav1g pipeline cache (ABI 2, `rav1g_device_create_with_cache`):** on hold; creating the device with the cache removed the GPU device on the console.
- **Caller-supplied duration for forward-only inputs (engine player):** idea, never requested.
- **dav1d zero-copy seek out-of-memory (4K AV1 normal seek exhausts the upload pool):** known, unfixed by choice; Bale uses fast seek for AV1 and the dav1d route no longer uses zero-copy.

## Owner to-dos

- Check whether ~115 MB of FFmpeg PDBs ship in Bale's MSIX.

## Done recently

| Release | What |
|---|---|
| Bale (2026-11-22) | Stage fix in bale_gpu: 4K60 AV1 on rav1g 47.1 → 57.6 fps (missed 21.8% → 4.5%), same as without the stage. Owner approved `bale_d3d11va_pool_from_stream` as the HEVC default (debug builds now, release after a long HEVC play). |
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
