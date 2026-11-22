# Fork backlog

What is open, waiting or parked for this fork, and what was done recently. Keep it current: add an
item when work is proposed, move it when its state changes, and drop "Done" entries once they are
older than a few releases (the commits and `README.md` keep the details). Every design item is sized
against the console limits in `AGENTS.md` ("Design against the console's limits").

Last updated: 2026-11-22 (wrapper 2.1.0-bale.65).

## Open: fork work

| Item | State | Why / sized against |
|---|---|---|
| Smaller d3d11va decoder pool: an opt-in key (e.g. `bale_d3d11va_surfaces=auto`) that sizes FFmpeg's decoder texture array from the stream's own SPS (max_dec_pic_buffering + 1 + a small margin), falling back to FFmpeg's default when it can't be read | Designed, not started | FFmpeg allocates 4 + 16 surfaces for HEVC/H.264 up front: 20 x 23.7 MiB = 478 MiB at 4K P010 (ledger 16). A typical film needs ~9, saving ~260 MiB. Not urgent within the 3328 MiB app limit. Risk: a pool too small fails decoding, hence SPS-derived and opt-in. |
| rav1g route: avoid the per-frame copy of direct output into the player pool | Idea | One full-frame GPU copy per frame (GPU copies are ~2.4x slower in the idle state). The copy exists because handing decoder images to Media Foundation deadlocked the fixed image pool (bale.45), so any change needs a new design for that. |
| FFmpeg 9.x | Deferred | 9.0/9.1 bring nothing the fork needs (HEVC hardening, a driver-compat SRV flag). Easier once Bale drops dav1d zero-copy, so native FFmpeg needs no dav1d patch (`p1`). |

## Waiting on others

| Item | Waiting for |
|---|---|
| Direct reads of decoder frames by the tone map (`bale_d3d11va_shader_resource`, bale.63/64; bale_gpu bit 64) | Bale: per-stage GPU timing (Bale `docs/resource-budget.md`). Off by default: the direct read removed the 23.9 MiB scratch, but app memory rose ~90 MiB unreported. Parked tests: does the extra stay flat or grow; read one fixed slice; GPU time on vs off. |
| dav1d D3D11 GPU grain (`bale_film_grain_mode=3`, bale.60) | Owner: visual check on the TV. Console: works for NV12 and P010, 1.11 vs 1.51 cores (1080p 10-bit, Series X). |
| rav1g untested paths | Console runs: HDR passthrough (needs an HDR display), 8-bit NV12 direct output (shared UAV NV12 not yet confirmed on the console), film-grain films. |
| rav1g as the production AV1 default | Owner's decision. Bale's debug builds already use it; release builds keep dav1d. |

## Parked or on hold (owner decisions)

- **Upstream pull requests:** not before Bale is stable, and only with the owner's approval.
- **D3D12 / SM6 tone map:** parked by the owner (2026-11-18).
- **rav1g pipeline cache (ABI 2, `rav1g_device_create_with_cache`):** on hold; creating the device with the cache removed the GPU device on the console.
- **Caller-supplied duration for forward-only inputs (engine player):** idea, never requested.
- **dav1d zero-copy seek out-of-memory (4K AV1 normal seek exhausts the upload pool):** known, unfixed by choice; Bale uses fast seek for AV1 and the dav1d route no longer uses zero-copy.

## Owner to-dos

- Check whether ~115 MB of FFmpeg PDBs ship in Bale's MSIX.
- Delete the old copy of this repo on the previous (exFAT) drive once the move to the new SSD is settled.

## Done recently

| Release | What |
|---|---|
| bale.65 | rav1g headers from v0.6.0-alpha.1 (perf record 624 bytes) |
| bale.64 | Shader-readable decoder pools only when bale_gpu reads them in place (bit 64) |
| bale.63 | Probe for shader-readable decoder pools; passed on the console |
| bale.62 | GPU memory ledger (bale_mem_account, categories 16–22) |
| bale.61 | Dithered 8-bit stage output (`bale_tonemap_output_depth=8`) |
| bale.60 | D3D11 GPU film grain for dav1d without zero-copy (`bale_film_grain_mode=3`) |
| bale.59 | Height limit for the software AV1 fallback (`bale_av1_fallback_max_height`) |
| bale.55–58 | rav1g memory logging, player texture cap, 10 output images, header updates |
