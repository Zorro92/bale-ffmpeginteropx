# Bale wrapper build

[`.github/workflows/bale-wrapper.yml`](../.github/workflows/bale-wrapper.yml) builds
`FFmpegInteropX.UWP.Lib` directly from this fork's checked-out commit. No wrapper
patch series is applied. Pushes to `main` and pull requests run only the fast pin,
PowerShell syntax, fixture and source-guard checks on Linux; manual runs also build
and repack the native UWP x64 wrapper on Windows.

The build verifies and repacks the original nuget.org 2.1.0 package. It retains the
winmd, managed projection, XML, targets and x86/ARM64 payloads byte-for-byte. Only
the x64 DLL is replaced, with its PDB and `SOURCES.txt` added, package version and
content types updated, and the invalidated signature removed. The built and
original winmd public type-name sets must match; this is not a full ABI/member
comparison. Artifacts include the package and SHA256, provenance, comparison
result, build log and corresponding source archive.

## Native FFmpeg

Since bale.82 the wrapper links Bale's native FFmpeg 9.1 (`bale/ffmpeg-native.json`: package
`9.1.0-bale.dav1d.1.5.5`, tag `native-ffmpeg-9.1-dav1d-1.5.5`; NuGet writes 9.1 as 9.1.0, the tag
keeps FFmpeg's name, and the pin check accepts both). It has no FFmpeg patches: Bale's dav1d
user-picture-buffer patch (`p1`, 8.1.2) existed only for the zero-copy route, which Bale no longer
uses. That code stays here, but on stock FFmpeg dav1d never asks for the caller's buffers, so a
stream with `bale_zero_copy=1` fails its first frame (`ZeroCopyMissed`) and the app falls back. The
build script finds the `avcodec-N.dll` (63 for FFmpeg 9) instead of naming the major. dav1d is 1.5.5:
1.5.4's MSVC atomics shim no longer builds with current MSVC (C7780). FFmpeg 9 removed no API this
fork uses; gains: HEVC tile-parallel software decoding, HEVC hardening, a d3d11va data-race fix, and
autodetected d3d12va hwaccels (see "D3D12 video decode probe").

## Tone map option

`MediaSourceConfig.FFmpegOptions["bale_tonemap"]` accepts decimal strings:
`"0"` = off (default), `"1"` = BT.2390 EETF, `"2"` = Hable. The wrapper consumes
this key; it is never passed to FFmpeg. It applies only when provider selection
chooses `D3D11VideoSampleProvider` and the source is 10-bit with either codecpar
transfer SMPTE 2084 (PQ, tone mapped), or HLG (ARIB STD-B67) or an SDR transfer
(BT.709, SMPTE 170M, BT.2020 10/12-bit) on BT.2020 primaries and the BT.2020
non-constant-luminance matrix (HLG converted to SDR; SDR gets a gamut conversion to
BT.709 only; Windows otherwise shows such video as if
it were BT.709, desaturated). Other providers/formats log one ignored-request
diagnostic and keep upstream behaviour. BT.709 SDR (transfer 1/6/14/15, primaries 1, matrix 1) is
eligible only when `bale_tonemap_output_width/height` asks for a smaller size and
bale_gpu exports `BALE_TONEMAP_SDR709` and `BALE_TONEMAP_OUTPUT_DOWNSCALE`: the stage
then only downscales (no tone curve, no gamut change) and outputs P010. 8-bit sources
are eligible when bale_gpu exports `BALE_TONEMAP_INPUT_NV12` (NV12 decoder slices are
read as 10-bit codes); otherwise 10-bit input is required. Unspecified tags (2) count as BT.709 on this route
(transfer unspecified or 1/6/14/15, primaries and matrix unspecified or 1) for video of
720p and larger, and are sent to bale_gpu as BT.709; other explicit tags (BT.601, a
BT.2020 matrix with BT.709 primaries, mixes) are declined. Unset/zero does not load the tone-map ABI or process frames.

Active output is fixed-size NV12 with BT.709 transfer, primaries and matrix,
limited range (16–235), and no stream/sample HDR mastering or content-light
attributes. The source `HasHdrMetadata` flag is retained; `IsHdrActive` is false.
The shader receives each frame's colour code points, mastering primaries/white
point/luminance (in nits) and content-light metadata. Frame side-data payloads
win over coded stream side data, with size checks before either payload is read.
Both curves use source peak 1000 nits, target white 100 nits, desaturation 0;
these defaults match Silo's `tonemapx bt2390 peak=100` setup.

The optional shader is loaded from the app's packaged `bale_gpu.dll`, tone-map
ABI 1. `Source/bale_tonemap.h` is a verbatim copy of Bale's
`native/bale-gpu/include/bale_tonemap.h` (Bale owns the ABI; copy it again,
unchanged, whenever Bale changes it). `Source/BaleToneMap.h` includes it and
asserts the configuration/frame sizes (40/72 bytes). A self-test mismatch at
create returns `BALE_TONEMAP_E_SELFTEST` (0x8004BA01) and is logged as such. No new native
FFmpeg build is needed. The fork creates an NV12 RTV/SRV output pool; bale_gpu
accepts decoder-bound P010 arrays and copies an input slice internally if needed.

Initialization, including bale_gpu's self-test, waits until the first frame:
MediaStreamSource supplies its renderer device after the stream descriptor is
created. An eligible request always declares SDR, even when ABI loading or
initialization will fail. Missing exports, ABI mismatch, self-test/create/apply
failure, device errors, software fallback, unsupported per-frame colour data,
or a frame-size change latch a fatal error and use the existing one-shot
`MediaStreamSource.NotifyError` path so Bale can replan to server transcode.
No failed or unprocessed frame is delivered as SDR. Fixed `Bale tonemap` logs
report activation once per stream and the first failure with its HRESULT.

The processor runs under FFmpeg's D3D11 device lock followed by the device's
multithread protection; the fork Flushes after processing before releasing
those locks. Pool operations use a separate mutex outside that context lock.
Processed callbacks return the stored texture without reopening a closed MF
surface; weak originating-pool references prevent recycling into a replacement
device pool. Seek flush retains the hardware path's existing return semantics.

Evidence/limits: Xbox Series X P010 HDR10 d3d11va decode was already verified in
real time, but this processing stage, its texture recycling and concurrency are
not console-confirmed. No console testing was performed for these changes.
Runtime shader correctness/performance and compatibility with the final ABI
header still require review and console validation.

### Display-sized output

`bale_tonemap_output_width` / `bale_tonemap_output_height` (decimal, 0 = off) give a
target size for the stage's output. When the visible video is larger and the loaded
bale_gpu exports `BALE_TONEMAP_OUTPUT_DOWNSCALE`, the stage fits the video inside the
target with its aspect kept, at even sizes and at most 4x smaller per axis (e.g.
3840x1600 into 1920x1080 gives 1920x800). bale_gpu then area-downscales in
display-linear light while tone mapping, and the media type reports that size. The
size is fixed when the stream is created. Without the capability the output stays at
the source size, with a warning. Logs: `Bale tonemap downscale WxH -> wxh (target ..)`,
`Bale tonemap active: curve=N wxh from WxH`.

Why: on the Xbox, the presentation path leaks driver state each time Media
Foundation's video processor downscales 4K SDR to a smaller display (the process dies
after ~23 opens, also with Windows' own decoder and no FFmpeg), while 4K HDR
passthrough, 1080p SDR, and 4K SDR converted at native size all survived 40 opens.

### SDR white (brightness)

`bale_tonemap_white_nits` (0–400, default 0 = 100 nits as before; 1–79 act as 80; bale.77) sets the SDR
white the tone map maps to, `BaleToneMapConfig::target_white_nits` for the stage and
`Rav1gDisplay::target_white_nits` for rav1g's display pass, so both give the same picture. The BT.2390
EETF ends at 2.03 times it (203 nits for 100), so a higher value maps more of the source into the
SDR range and makes the tone-mapped picture brighter. Bale exposes it as a setting. The source peak
stays the stage's own choice (Bale picks it per frame from the metadata; rav1g applies the same rule).
Console (Bale 0.1.749, bale.77): `white=200` gave `tone map SDR white=200 nits` both on the HEVC stage
route and on the rav1g display-pass route (`display pass 1920x1080 depth=10`); playback was normal.

### Output depth (dithered 8-bit)

`bale_tonemap_output_depth` (0–10, default 0; bale.61) chooses the stage's output depth:
`8` makes the stage write NV12, ordered-dithered in its own pass, when the loaded bale_gpu exports
`BALE_TONEMAP_OUTPUT_NV12_DITHERED` (32; static 8x8 Bayer, ±1 code, zero mean; `Source/bale_tonemap.h`
copied from Bale 17a07b8). Any other value, or a bale_gpu without the bit, keeps today's choice:
P010 when the stage can write it, else plain NV12. Decided when the stream is created (the
capability needs no device), so the media type is NV12 from the start and keeps the tone-map
mark. It applies wherever the stage runs (D3D11 hardware decode and the rav1g route); the generic
part is `VideoFrameProcessor::DithersOutputFormat` and `SelectOutputFormat`. Log: `Bale tonemap
output format=P010|NV12|NV12 dithered`.

Why: owner's bit-depth rule (2026-11-19), "work at source depth, deliver at display depth". On an
HDMI output below 30 bpp an 8-bit result is shown anyway; dithering it in the stage's pass, from
the stage's full-precision result, avoids the banding of a later plain 10-to-8 step and saves the
P010 bandwidth. Bale sets 8 only for stage mounts on such displays, not for HDR passthrough or AV1.

Console (Series X, 24 bpp SDR display, bale.61, Bale 0.1.700, 2026-11-19): a 4K HDR film tone-mapped
with `8` logged `Bale tonemap output format=NV12 dithered`, an NV12 media type, 24.0 fps and no
dropped frames in three runs; without it, P010 as before. Bale's guard accepted both.

### Retired debug options

`bale_tonemap_output` (0–15) and `bale_tonemap_format` (0–3) were debug switches used to
find the Xbox presentation leak. They are retired: still accepted with their old ranges
(an unknown `bale_*` key would fail the open, and older Bale builds may still send
them), ignored, and logged once as `Bale option <key> retired; ignored`. The stage now
always outputs P010 when bale_gpu can write it (NV12 only for an older bale_gpu).

### rav1g delivery diagnostics

Every 240 delivered samples (and at a seek and at stream end) the rav1g route logs
`Bale rav1g: delivery samples=N rate=../s latency avg=.. max=.. ms decode call avg=.. max=.. ms pts step min=.. max=.. ms
backwards=N gaps=N queue max=N`: samples delivered per wall-clock second, decode-to-deliver latency (from feeding a temporal unit to
handing its frame to the player), the timestamp steps between delivered samples (in sample
time, as the player receives them; bale.49 printed stream time-base units by mistake), steps that
went backwards, gaps over 1.5 sample durations, and the deepest ready-frame queue. The first 24
samples after an open or seek are also listed: `first samples pts ms=a,b,c,...`.

Since bale.52 the line goes on with `inflight avg=.. max=N target=N wait avg=.. max=.. ms output
avg=.. max=.. ms pool blocked=N`: temporal units in flight (fed to rav1g and not yet delivered,
counted when a frame is taken for delivery, that frame included) against the feeder's target;
the time the sample thread waited for a decoded frame (0 when one was queued); `OutputFrame` time
(GPU fence wait, copy or stage, release signal and flush); and how often `get_image` had to wait
for a free image in the window. The decode call is timed on the feeder thread. Since bale.66
`decode call ...` is followed by `read avg=.. max=.. ms gap avg=.. max=.. ms`: the packet read
(demux, on the feeder thread) and the time from the end of one decode call to the start of the next
(the read plus any wait for the units in flight to drop below the target; a seek's pause is not
counted). A long gap with a short read means the feeder waited for in-flight units to drain,
while a long read means the input was slow.

Since bale.67 the line also has `image release wait n=N avg=.. max=.. ms`: how often, and how long,
`get_image` waited on the CPU for an image's D3D11 readers (the stage or the player copy) to finish.
On bale.66 rav1g spent 13–19 ms per frame in its output callback on 4K60 AV1 10-bit
(`wait output`, 40/60/100 Mbps; Series X, Bale 0ba928d) with `pool blocked=0`: there was always a
free image, but the pool took the first free one, typically the image the stage had just been given,
and waited for its release fence. Delivery stayed at 44.9–46.4 fps, and more in-flight units
(6) or player textures (32) did not help. The pool now takes a free image whose readers are done,
else creates one while it has room (up to 10, 23.7 MiB each at 4K 10-bit: within the 3328 MiB app
limit), and only then waits, for the image released first. On bale.67 (Bale f71f93a, same clips) that raised delivery to
47.0/50.4/50.2 fps, with the pool full (10 images, peak 4 in use) and `pool blocked=0`, but about 210
of 240 acquires still waited 16–19 ms: every free image still had D3D11 work pending, so the stage's
GPU work runs several frames behind rav1g (GPU ~9.5–10 ms a frame). The remaining limit is GPU time,
not the pool.

Since bale.70: `bale_gpu_output_images` (0–16, default 0 = 10; 1–3 act as 4) sets the output images per
stream (23.7 MiB each at 4K 10-bit: 16 is ~379 MiB, within the 3328 MiB app limit), and the in-flight
target stays below it. The delivery line's `image release wait` part goes on with `full n=N in use
avg=.. free pending avg=.. of C`: of the acquires that found every image either in use or still read
by D3D11 work, how many images were in use (with rav1g, queued for output or being output; Bale's
decode-ahead queue holds player textures, not these images) and how many were free with D3D11 work
still pending, out of C. Mostly "free pending" means the D3D11 side lags (more images let rav1g run
further ahead); mostly "in use" means rav1g and the output queue hold them. Console (Bale 301411f, 4K60 AV1 40 Mbps, 30 s): with 10 images
(decode-ahead 8) 57.1 fps, 114/240 waits at 12.0 ms, at a wait 1.5 in use and 8.5 free pending,
4.8% missed; with 16, 57.6 fps, 96 waits at 9.3 ms, 1.3 in use, 12.4 pending, 4.7% missed (+142 MiB);
decode-ahead 4 instead of 8: 57.8 / 58.0 fps, 4.0 / 4.1% missed. The waits are D3D11 work still
pending, so more images only fill with it; Bale keeps 10. A/B on the 40 Mbps clip (30 s): without the downscale stage (rav1g's 4K
P010 copied straight to the player) delivery was 57.5 fps, `image release wait` 24/240 at ~10 ms,
rav1g's output wait 1.2 ms and 5.0% missed frames; with the stage (4K to 1080p) 47.9 fps, 233/240
at 18.8 ms and 20.6% missed. So bale_gpu's 4K downscale costs ~10 ms of GPU per frame; the fix is in
bale_gpu (Bale).

`bale_gpu_decode_inflight` (1–8, default 4) sets how many temporal units the feeder keeps in
flight; it stays below the stream's output images (8 up to bale.56, where 8 acted as 7).

The output images are created on demand up to 10 per stream (bale.57; 8 before). rav1g holds up to
its pipeline slots + 2 (rav1g main 40e38ec uses 4 slots above 2560x1440, so up to 6); frames in
the ready queue and the one being copied or staged hold the rest, and every image goes back right
after its copy or stage. On 4K with 3 slots, 4 were created and `get_image` never waited (bale.55).
Images not created yet still count against `memory_available` (up to 10 × 23.7 MiB at 4K 10-bit).

`bale_rav1g_perf=2` (bale.72, debug only) adds one line per frame for the stage, host clock in ms
(QPC ticks x 1000 / QPF, the base of rav1g's `complete_ms`): `Bale rav1g stage u=<unit> pts=<100 ns>
mode=stage|display|copy take=.. submit=.. release=.. gpudone=..` (since bale.74 `mode`: the provider's stage, rav1g's display
output copied into the player texture, or direct output copied). `u` is the fork's unit counter (rav1g's `user_data`), `pts`
the sample's timestamp in 100 ns as handed to Media Foundation (Bale keys its renderer log on it); take = the sample thread took the
frame, submit = the stage or copy submitted and flushed, release = the image back in the pool,
gpudone = the image's release fence passed (the D3D11 work on it finished; a thread-pool wait on the
fence). About 60 lines/s at 4K60. Since bale.73 (rav1g v0.6.0-alpha.3 or later) it also logs one line
per rav1g record: `Bale rav1g frame u=<unit> pts=<100 ns, -1 if no longer known> show= sef= intra=
out=<output_path> decode=<cpu FRAME> entropy=<ENTROPY_WALL> waitout=<WAIT_OUTPUT> waitslot=<WAIT_SLOT>
gpu=<gpu_span_ms, -1 without timestamps> gap=<gpu_gap_ms> done=<complete_ms>` (ms; `done` on the same
QPC clock). `u` is av1_perf_frame::user_data, the pts is the fork's for that unit; hidden frames and
show-existing copies have their own records. The callback is process-wide: with several streams on
perf=2 at once, the last one resolves the pts. Use `2` only with a rav1g.dll from v0.6.0-alpha.3 on: an older library's
record ends at 624 bytes, and the frame line reads `user_data` past it (the record carries no size,
so the fork can't tell). `1` reads only fields before 624 and works with either. Console (Bale 1dd031e: bale.73 with rav1g
v0.6.0-alpha.3, 4K60 AV1 60 Mbps, 45 s): 1797 frame and 1797 stage lines, all joinable on pts with
Bale's per-sample `frame:` log.

`bale_rav1g_perf` (0/1, default 0) appends rav1g's own timing run to the delivery line:
` rav1g perf records=N decode thread avg=.. max=.. ms gpu avg=.. max=.. ms shown=N showable=N
sef=N` (rav1g's decode thread from frame setup to submit, `AV1_PERF_CPU_FRAME`; the GPU span of
the frame's command list; records with `show_frame`, `showable_frame` and `is_sef`). Since bale.66 it
ends with ` wait output avg=.. max=.. slot .. fb .. image .. ms entropy wall avg=.. max=.. ms`, the
decode thread's waits per record (`AV1_PERF_CPU_WAIT_OUTPUT`: in the output callback, i.e. a blocked
`get_image`; `WAIT_SLOT`, `WAIT_FB`, `WAIT_IMAGE`) and the tile decode's wall time
(`ENTROPY_WALL`). It installs
the process-wide `rav1g_set_perf_callback` while at least one stream with the key is open (with
several, their frames mix) and removes it when the last one closes; a library without that export
logs `Bale rav1g: perf statistics unavailable` once and the stream plays without it. The record
layout (`Source/av1_perf.h`, rav1g v0.6.0-alpha.1, 624 bytes since bale.65; 616 for v0.4.0-alpha.2
to v0.5.x, bale.58–64; 592 in alpha.1, bale.57 and before) is not covered by rav1g's interface version: copy it again from the rav1g release Bale
ships, and set the key only with a wrapper whose layout matches the shipped `rav1g.dll` (a mismatch
reads the wrong fields). Diagnostic only; production never sets
it.

With the player-side pool (bale.46+), the decoder's fixed pool no longer limits the player's
buffer, so decode-ahead and BufferTime can be set as for dav1d. Mind the memory: each buffered
frame is a player-side texture (1080p P010 ≈ 6 MiB, 4K ≈ 25 MiB).

After each delivery line and at stream end (bale.55) the route also logs where the stream's GPU
memory goes: `Bale rav1g: memory (delivery|destroy) decoder=.. MiB images=N x .. = .. MiB player=N x
.. = .. MiB stream total=.. MiB app=.. MiB`. `decoder` is rav1g's `device_size` (its own buffers,
from `rav1g_decoder_configuration`); `images` the decoder's output images that exist now (the
fixed pool, created on demand); `player` the player-side textures that exist now (bale.56; created so far on bale.55) (direct copies,
or the stage's display-sized output), each the size the player receives; `app` is
`AppMemoryUsage` for the whole process. Not counted: the process-wide rav1g device and its
pipelines (shared by every stream, created by the prewarm) and the stage's internal resources in
`bale_gpu.dll`. Sizes are computed from format and dimensions (NV12 1.5, P010 3 bytes a pixel),
not queried from the driver, so alignment padding is not included.

`bale_gpu_output_textures` (0–64, default 0 = no limit; 1–3 act as 4, bale.56) caps the
player-side textures of a GPU decoder stream. Without it the pool grows to whatever the player and
the decode-ahead queue hold at once: 25 textures with BufferTime 500 ms and 19 with 250 ms on 4K
AV1 → 1080p stage output (bale.55, Series X), 5.9 MiB each. With a limit, a frame that finds all
textures out waits up to 150 ms for the player to return one, then grows the pool anyway and
counts an overrun; textures above the limit are let go when they come back, so the pool shrinks
again after an overrun. The memory line shows `player=` as the textures that exist now and
`(limit=N overruns=N)`. A limit below what the player needs to start (its BufferTime in frames plus
a few for rendering) costs a 150 ms wait per frame while it fills, so keep it above that and check
overruns stay 0.

Console, Series X, 4K AV1 10-bit SDR → 1080p stage, rav1g `slots=3 parse=2 device=501.5 MiB`,
inflight 4 (bale.55/56, 2026-11-18; idle app baseline 228 MiB, 275 MiB after prewarm):

| BufferTime | limit | player textures | stream total | app | fps | start |
|---|---|---|---|---|---|---|
| 500 ms | none | 25 × 5.9 = 148.3 MiB | 744.7 MiB | 1225 MiB | 23.92–24.20 | |
| 250 ms | none | 19 × 5.9 = 112.7 MiB | 709.1 MiB | 1190–1197 MiB | 23.74–24.06 | |
| 500 ms | 16 | 94.9 MiB, 0 overruns | 691.3 MiB | 1179–1205 MiB | 23.69–23.98 | 9.9 s (during prewarm) |
| 250 ms | 12 | 71.2 MiB, 0 overruns | 667.6 MiB | 1157–1182 MiB | 23.92–23.99 | 5.5 s |

No dropped frames in any run. Seeks took 0.8–1.3 s with a limit, and app memory rises about 25 MiB around a seek. Inflight 3 saves one
decoder image (23.7 MiB) at a small cost in steadiness.

`bale_rav1g_parse` (0–8, default 0 = rav1g's choice: 1 frame in parse and 1 parse thread up to
1440p) asks rav1g for N frames in parse and N parse threads. With one frame in parse, rav1g parses
each unit inside the decode call on the feeder thread; the "decode call" timing shows that cost.
rav1g's budget guard may still create fewer.

The feeder's packet reads are interruptible: a disabled stream's packets are dropped by the reader,
so a read for it would otherwise run to the end of the input. The feeder stops a read when it is
paused (seek, Starting, stream switch), stopped, or its stream is disabled (bale.53; on bale.52 a
seek right after another seek could read the rest of the file and stall the seek).

### rav1g prewarm and threading notes

`FFmpegInteropX.dll` exports a plain C function, `int bale_rav1g_prewarm(void)` (no WinRT API
change). It creates rav1g's process-wide D3D12 device and starts compiling its pipeline states
(~99, about 6.5 s on a Series X) on a background thread with 6 compile threads, and returns at
once: 0 = started, 1 = already started, -1 = could not start a thread. Logs `Bale rav1g: prewarm
started` and `prewarm ready ms=N (device ms=D pipelines=S)` (or `prewarm failed (status=..)`):
since bale.85, N runs until every pipeline state exists (`rav1g_device_wait_pipelines`), D is the
device creation alone and S that call's status (0 = OK, -1 = the library lacks the export). Up to
bale.84 the line printed only D (e.g. `ms=24` with rav1g alpha.2), not the compiles. The adapter is rav1g's choice,
the first hardware adapter (the only one, and Media Foundation's, on Xbox); each stream still
checks it against MF's. Without prewarm, the first stream creates the device with 2 threads.

`FFmpegMediaSource` members that take the source lock can block while the sample thread waits on
a decoder: `CurrentVideoStream`, `CurrentAudioStream`, `MetadataTags`, `PlaybackItem`,
`GetMediaStreamSource`, `CreateMediaPlaybackItem`, `StartBuffering`, `ExtractThumbnail`, the
filter setters and `Close`. Don't call them on the UI thread during playback.

## Compressed video colour option

`MediaSourceConfig.FFmpegOptions["bale_compressed_color_info"]` accepts decimal
strings: `"0"` = off (default), `"1"` = on. The wrapper consumes the key; it is
never passed to FFmpeg. When enabled, video handed to a Windows system decoder
uses the same codecpar and coded side-data mapping as FFmpeg-decoded video:
primaries, transfer function, YUV matrix, nominal range, MaxCLL/MaxFALL and
mastering display luminance/primaries. HDR primaries, transfer functions and
static metadata follow the same `HdrSupport`/`useHdr` policy as the uncompressed
providers; non-HDR values, matrix and range use the existing mapping regardless
of that policy. Audio is unchanged. Unset/zero leaves compressed media types
unchanged. No new native FFmpeg build is needed.

VP9 needs container colour information because its bitstream does not identify
the PQ transfer function and BT.2020 primaries or carry HDR10 static metadata.
Without these media-type attributes, the system decoder cannot infer that a
VP9 profile 2 stream is HDR10. HEVC also gains the container's static metadata,
in addition to any colour/HDR information carried in-band. The option covers
all compressed video providers, including the H.264/HEVC NAL subclasses.

Debug builds emit one fixed diagnostic per successfully initialized opted-in
system video stream: `Bale compressed colour info: transfer=%d primaries=%d matrix=%d range=%d hdr=%d`.
The colour fields are AV code points; `hdr` is the source's `useHdr` policy flag,
not a claim that HDR metadata was present or that the display activated HDR.

Evidence/limits: the reported Xbox Series X VP9 profile 2 HDR10 media type had
none of these colour/HDR attributes. The available test display is SDR, so the
HDR-TV effect remains unverified. This option stays off by default pending the
owner's HDR-TV test. No console testing was performed for this change. A generic,
default-on form that propagates container colour information while retaining
the HDR policy would be the natural upstream shape, subject to review and testing.

## Fast seek without a playback session

Upstream FastSeek only runs when `FFmpegMediaSource.PlaybackSession` is set to a
`MediaPlayer` session: it uses the session's position to choose the seek direction.
Players without one (e.g. `IMFMediaEngine` in rendering mode, which also ignores
`MF_MEDIA_ENGINE_SEEK_MODE_APPROXIMATE`) therefore always get full seeks, which decode
every frame from the keyframe to the target (for 4K software AV1 that ran the
zero-copy upload out of GPU memory on the console).

`bale_fast_seek_without_session` = `"1"` (default `"0"`) enables FastSeek without a
session, backward-only: buffered and demuxer seeks take the keyframe at or before
the target and report it through the `MediaStreamSource` Starting request
(`SetActualStartPosition`), so the player's clock starts there. Seeks never land
after the target; they land up to one keyframe interval early. The stream-switch
skip heuristic, which needs the session position, is off in this mode. A session,
when set, keeps upstream behaviour. Each such seek logs
`Bale seek: sessionless fast seek target=.. landed=.. ms`.

## GPU AV1 decoder option (rav1g)

`MediaSourceConfig.FFmpegOptions["bale_av1_decoder"]` accepts decimal strings: `"0"` =
today's paths (default), `"1"` = decode AV1 with [rav1g](https://github.com/Zorro92/rav1g)
(AV1 in D3D12 compute shaders) on `GpuDecoderSampleProvider`. The wrapper consumes the key;
it is never passed to FFmpeg. Design and review: [design-rav1g-provider.md](design-rav1g-provider.md).
Frame grabbers never use it. With `"1"`, `bale_zero_copy`, `bale_d3d11_upload` and
`bale_film_grain_mode` do not apply to the stream (rav1g applies film grain itself).

Code: generic `GpuVideoDecoder.h` (decoder interface), `GpuDecoderImagePool.h` (output
images), `GpuDecoderSampleProvider.h` (the provider); all rav1g glue in `BaleRav1g.h`.
`Source/rav1g.h` is rav1g's `crates/rav1g/include/rav1g.h`, copied verbatim (rav1g
v0.8.0-alpha.3, `25f8a04`, whose changes over alpha.1 are comments only; interface version 3 since bale.78; version 2 in bale.74–77, version 1 before;
copy it again, unchanged, when rav1g changes it). The loader accepts exactly the header's version, so
bale.78 and later need a rav1g.dll of interface version 3 (v0.8.0-alpha.1 on), bale.74–77 version 2
(v0.7.x) and wrappers up to bale.73 version 1: a
mismatch logs `Bale rav1g: interface version N, expected M` and the stream falls back to dav1d. Pin
wrapper and DLL together.
`Source/av1_perf.h` is rav1g's `libav1/dx/av1_perf.h` at tag v0.8.0-alpha.3 (`25f8a04`; the record unchanged since alpha.1, alpha.3 removes the declarations of its retired test switches; 640 bytes, with the GPU stage `AV1_PERF_GPU_DISPLAY` and `user_data` at 632, unchanged since v0.7.0-alpha.1; 632 bytes in v0.6.0-alpha.3), copied verbatim
(only for `bale_rav1g_perf`). No new
native FFmpeg build is needed; Bale ships `rav1g.dll` in its package.

**Fallback at open.** `BaleRav1gDecoder::TryOpen` checks: `rav1g.dll` loads
(`LoadPackagedLibrary`, no import library), every export it uses is there,
`rav1g_abi_version()` equals the header's `RAV1G_ABI_VERSION`, the profile is Main (4:2:0 or
monochrome), `rav1g_peek_stream_info` reads the container's sequence header (av1C config
OBUs; without one, the probed pixel format) with depth 8 or 10, and the process device
exists. Any failure logs `Bale rav1g: fallback to dav1d (reason, status=0x..)` and the
stream opens on today's dav1d path with the keys above honoured, unless
`bale_av1_fallback_max_height` (0–8192, default 0 = no limit; bale.59) is set and the
container's height is above it: then the stream is refused (`Bale AV1: refusing video stream
(GPU decoder declined, height N above the fallback limit M)`) and, for the default video stream,
the open fails with `MF_E_UNSUPPORTED_FORMAT`, as a zero-copy refusal does, so the app can replan
(Bale: 1080, so a 4K source never lands on the software decoder). A height the container doesn't
give (0) is never refused. The decoder itself is
created on the first temporal unit, after Media Foundation's device is known; a failure
from then on (creation refused, decode or drain status, device removed, fence wait, a
frame the declared type can't carry) latches a fatal stream error, logged once as
`Bale rav1g: stream failed (reason, hr=0x..)` and raised through `NotifyError` (status
`0x80042B2N` = rav1g status N; `E_INVALIDARG` → unsupported media format). Since bale.85 a failed
decode or drain adds ` decoder: <kind>: <detail>`: rav1g's `rav1g_decoder_error` and
`rav1g_decoder_error_detail`, copied right after the failed call (fixed library texts, kept to
printable ASCII and 240 characters; cleared by a successful call).

**Device.** One `Rav1gDevice` per process, created on the first rav1g stream, never reset.
rav1g creates the D3D12 device and queue itself on the first hardware adapter (this DLL has
no `d3d12.lib` import and a UWP app cannot load `d3d12.dll` by name); the stream checks
that Media Foundation's D3D11 device is on the same adapter (LUID) and fails otherwise.
Each decoder's output fence is opened on the D3D11 device (`OpenSharedFence`), again after
a recreate. Settings: `rav1g_settings_default`, the sequence size and depth from
`rav1g_peek_stream_info`, `RAV1G_OUTPUT_SEMI_PLANAR`, `shared_fence`, the output callbacks
and `memory_available` = `AppMemoryUsageLimit − AppMemoryUsage` minus the bytes of output
images not created yet. `low_memory`, slots and threads stay rav1g's (unless
`bale_rav1g_parse`); of the process-global `rav1g_set_*` switches only the perf callback is set,
and only with `bale_rav1g_perf`. The device handle comes from the MSS device manager at the first
Starting request; a lost device fails the stream.

**Flow and threads.** Since bale.52 the provider has its own feeder thread (started at the
first sample request, priority from `bale_decode_thread_priority`): it reads packets
(`GetNextPacket`; `FFmpegReader` serialises reads with the other threads through its demux
lock) and calls `rav1g_decoder_decode` with user data = index into a table of {pts, duration,
generation} while fewer than `bale_gpu_decode_inflight` units are in flight (fed and not yet
delivered). With one frame in parse rav1g parses on that thread, so the next units are parsed
while the GPU decodes earlier ones and the sample thread delivers. rav1g's `frame` callback
(completion thread) only queues the frame. The sample thread (the decode-ahead worker when
`bale_decode_ahead_frames` > 0, as Bale sets for AV1, else the MSS request thread) waits for a
queued frame, has the immediate context wait for its fence (`ID3D11DeviceContext4::Wait`, on
the GPU) under the device's `ID3D10Multithread` protection, then copies it into a player-side
texture or runs the stage. At end of stream the feeder drains rav1g; the sample thread returns
end of stream once the queue is empty. `NEEDS_RECONFIGURE` drains, recreates the decoder and
decodes the unit again (on the feeder). Matroska may leave the sequence header out of blocks:
a decoder created from the container's header gets it in front of its first unit.

Locks: the provider's frame lock guards the frame queue, the timing table and the feeder's
state; it is never held while rav1g is called, and it may be taken before the image pool's lock
(never after). rav1g is never called with the samples lock or the D3D11 multithread lock held.
Order: source lock → decode-ahead lock → frame lock → pool lock. The feeder pauses whenever the
source pauses the decode-ahead worker (after the worker has stopped, since a sample request it
is in may wait for a frame): around seeks, flushes, stream switches, Starting requests and
device checks. A flush, `ResumeAfterCancel` and a stream switch therefore never run beside a
feeder step. Close stops and joins the feeders after the decode-ahead worker and before the
reader stops; the destructor does the same. A disabled stream (end of stream, cancel, switch)
gets no packets, so the feeder idles until it is enabled again.

The decode-ahead worker still wraps this provider when `bale_decode_ahead_frames` > 0: the
feeder supplies the decode parallelism, and the worker turns decoded frames into finished
samples ahead of MF's requests, so the MSS request thread, which holds the source lock (also
for audio requests), seldom waits for the GPU or the fence. Without the key the request thread
waits for frames itself, which also works. Each finished sample holds a player-side texture.

**Images.** 8 per stream, never more: D3D11 textures (`RENDER_TARGET | SHADER_RESOURCE |
UNORDERED_ACCESS`, `MISC_SHARED_NTHANDLE | MISC_SHARED`; without unordered access if the
driver refuses, then rav1g copies) opened in D3D12 once. `get_image` waits for a free
image of the request's size (rounded to even) and format (NV12 for 8-bit, P010 for
10-bit); images of an older size are released as they come back. It returns non-zero only
to cancel (stop, seek) or on failure. Images return on the sample's `Processed` (texture
stored per sample) or, for processed output, right after the stage; the releaser signals a
D3D11 fence that `get_image` waits for before rav1g writes the image again. A flush also
takes back the images of samples MF still holds, as the D3D11 path does.

**Seek, stop.** MSS Starting and Close first cancel waits without taking the source lock:
a `get_image` wait for a free image (since bale.52 the cancel only ends waits; a request that
finds a free image still gets it, so a Starting request no longer fails the unit the feeder is
decoding) and the sample thread's wait for a frame (it checks every 20 ms). Flush (feeder
paused) bumps the generation, drains rav1g (image requests fail meanwhile; drain errors are
part of the seek), returns older frames unsent and skips packets until a key frame; the
decoder is kept. Starting ends the cancel and, if the cancel failed a decode, drains and
waits for a key frame too. Exact (non-fast) seeks deliver frames from the key frame on, as
the other FFmpeg paths do; Bale uses fast seek for AV1.

**Output.** Fixed at stream creation:

| Source | Output | Mark |
|---|---|---|
| SDR, no larger than `bale_tonemap_output_width/height` (or no target) | rav1g's texture: NV12 (8-bit) or P010 (10-bit), visible size rounded to even, source colour attributes | rav1g |
| SDR larger than the target, not BT.2020 (also without `bale_tonemap`) | stage, downscale only, P010 at the display size (as `BaleToneMap::Create` decides) | tone map |
| PQ/HLG/BT.2020 with `bale_tonemap` | stage, tone map (+ downscale), P010 | tone map |
| HDR without `bale_tonemap` | rav1g's P010 with HDR attributes (useHdr policy) and per-sample HDR metadata | rav1g |

The rav1g mark is the media-type attribute `{6d553633-9287-4856-bc9f-0c23af00e184}` =
UINT32 1 (`BaleRav1gDecoder::OutputMark`); stage output carries the tone-map mark. When
the stage declines (no bale_gpu capability, odd size), the stream stays on direct output.
Colour per frame: rav1g's description, the container's where the bitstream has none.

**Logs.** `Bale rav1g: loaded <rav1g_version> (interface 1)`, `Bale rav1g: output WxH
NV12|P010 direct|processed hdr=N`, `Bale rav1g: active WxH bits=N slots=S parse=P threads=T
device=X MiB`, `Bale rav1g: destroyed (frames=.. discarded=.. images created=.. peak in
use=.. blocked=.. output created=..)`, plus the fallback and failure lines above. The
stream reports `DecoderEngine::FFmpegD3D11HardwareDecoder` (no new engine value: the API is
frozen).

Evidence/limits: compiled in CI only; not console-tested. To check on the console: real-time
4K, md5/visual parity with dav1d, pool counters over seeks and stop/start cycles, that
Media Foundation releases enough samples for 8 images (MSS `BufferTime` on this route
should stay below 8 frames), the direct-write path (`OUTPUT ... direct` in rav1g's log) and
both fallbacks.

(That note predates the console runs; Bale's debug builds have used rav1g since.) Console, film
grain and 4K60 (Bale's report, Series X, Bale 0.1.791 / bale.84 / rav1g 0.8.0-alpha.1, 2026-11-26):

- Film grain: on a paused heavy-grain frame (cropped to the picture), rav1g is pixel-identical to
  dav1d with CPU grain and with D3D11 GPU grain (RMSE 0); each differs from grain off by 1.38%.
  rav1g logs no separate grain line.
- 1080p60 10-bit grain clip (SVT-AV1), two runs: 0 missed frames, decode 58.8/60 fps, 0
  discarded, P010 direct output. Device Portal CPU about 5.4% and 4.7% on average, against about
  12–14% (dav1d, D3D11 GPU grain) and 30–53% (dav1d CPU grain); GPU busiest engine 24–35%; rav1g
  device 132.6 MiB.
- 4K60 10-bit at 40 Mbps, output 1080p P010 processed: 0 missed frames, 58.8/60 fps, CPU about
  22%, GPU busiest engine 65–86%, rav1g device 488.5 MiB (slots=4 parse=4 threads=4). Four seeks
  completed (8 frames discarded, starved=48 across them); destroyed cleanly. No fallback, failure,
  device removal, or rav1g warning in the session.

Sized against: the GPU is the limit at 4K60 (65–86% of the busiest engine on a ~1.86 TFLOP/s GPU),
while CPU stays far below the 4 shared cores; the device's 488.5 MiB fits the 3328 MiB per app.

## D3D11 GPU film grain without zero-copy

`bale_film_grain_mode=3` (bale.60) puts AV1 film grain on the GPU for software libdav1d streams
that are **not** zero-copy, with Direct3D 11 only: no D3D12, no `bale_zc_*`, no
`bale_d3d11_upload`. Values of the key: 0 = dav1d applies grain on the CPU (default, upstream);
1 and 2 = GPU grain on the zero-copy path (unchanged; without zero-copy they keep dav1d's grain);
3 = this path, and on a zero-copy stream the same as 2.

Decided before the decoder opens, because dav1d can't start applying grain later. It needs a 4:2:0
8/10-bit input whose output is NV12 (8-bit) or P010 (10-bit), the grain exports of
`bale_gpu.dll`, and a once-per-process probe: a private D3D11 device on the default adapter that
must support NV12/P010 staging textures and pass the grain pass's self-test
(`bale_grain_gpu_create`). Then dav1d exports the grain instead of applying it and the open logs
`Bale film grain: mode=gpu-d3d11`. Anything missing keeps dav1d's own grain for that stream and logs
`Bale film grain: mode=dav1d (reason, hr=0x..)`.

Per frame, swscale converts the picture into a mapped STAGING texture (a ring of three), and
`bale_grain_gpu_apply` draws it with the frame's grain into a pooled render-target texture
(`RENDER_TARGET | SHADER_RESOURCE`, not shared) that goes to Media Foundation; a frame without
grain parameters is copied. The upload device is taken on the first Starting, as for zero-copy, and
the grain handle is created then, before any decode. From then on a failure (device, staging map,
grain pass) fails the stream like a zero-copy failure (`NotifyError`, the app replans): dav1d no
longer applies the grain itself, so there is no CPU fallback after the open. Logs: `Bale film
grain: D3D11 staged upload active: NV12|P010 WxH`, `... staged frames=N grained=N` every 240 frames,
and the upload timing line with `repack=0` (fill = swscale, gpu = grain or copy).

Measured before this path existed (Series X, 1080p 10-bit AV1 with grain, Bale 0.1.695, 23.97 fps,
no drops): dav1d CPU grain 1.39–1.51 cores, grain off 1.23, D3D12 zero-copy GPU grain 0.95.

Console check (Bale's report, Series X, Bale 0.1.791 / bale.84, 2026-11-26; 1080p60 10-bit AV1
film-grain clips encoded with SVT-AV1, two runs each): `Bale film grain: mode=gpu-d3d11`, 0 missed
frames. Device Portal CPU averaged about 12–14% (steady) with this path against about 30–53%
(bursty) with dav1d's CPU grain. On the same paused frame, GPU and CPU grain differ by RMSE 0.73%,
less than either differs from grain off (1.24–1.32%); visually identical.

## GPU memory ledger

Optional. The wrapper reports its GPU memory to `bale_gpu.dll` when the DLL exports both functions of
ABI 1: `uint32_t bale_mem_abi_version(void)`, which must return 1, and
`void bale_mem_account(uint32_t category, int64_t delta_bytes, int32_t delta_count)`, which must be
thread-safe. As Bale confirmed, bale_gpu's account path is atomics only: no locks and no D3D11 calls.
Bale's `mem` view uses them to show GPU memory per component beside the Xbox graphics total.
Without both exports, or with another version, nothing is reported and nothing else changes. The only log
line is `Bale memory ledger: active` (INFO), once, when the ledger loads. The code is
`Source/BaleMemoryLedger.h`.

Categories (fixed numbers):

- 16 `DecoderFrames`: the d3d11va decoder's texture array, one per decoder pool.
- 17 `ToneMapOutput`: output textures of the tone-map or downscale stage.
- 18 `Dav1dUpload`: the software AV1 upload pool and its STAGING ring.
- 19 `Rav1gOutputImages`: rav1g's output images (its fixed pool).
- 20 `Rav1gPrewarmResident`: the process-wide rav1g device's presence; one count, no bytes, never subtracted.
  The decoders' buffers are not counted here (category 22).
- 21 `OtherTextures`: copy textures of the plain d3d11va path, and output textures of the rav1g path without a stage.
- 22 `Rav1gDecoderBuffers`: the rav1g decoder's own device buffers, `device_size` from its configuration; one
  count per decoder (none when `device_size` is 0). Added when the decoder is created and subtracted when it
  is destroyed, including by the destructor, so a recreate after a reconfigure counts the new size.

Size rule: width x height x bytes per pixel x array slices, with mips ignored (all of these have one). Bytes
per pixel: NV12 1.5, P010 3, R8 1, R8G8 2, R16 2, R16G16B16A16 8, and 4 for every other format (BGRA8, RGBA8
and R10G10B10A2 among them). Driver padding is not counted, so the figure is a little under the driver's
allocation. Category 22 is not a texture: its bytes are rav1g's `device_size`, not this rule.

Ticket: a texture is counted once. The first report attaches a ticket, an `IUnknown` under the private GUID
`BaleMemoryTicketGuid` set with `SetPrivateDataInterface`. The ticket adds its bytes and count when it is made
and subtracts them in its destructor. D3D11 destroys the ticket with the texture, on whichever thread drops the
last reference, so the count falls when the memory goes. Reporting a texture that already has a ticket does
nothing, so callers may report every reuse. If the attach fails, the ticket is released at once, which
subtracts it again. Because `bale_mem_account` can run inside D3D11's release path, it must not call D3D11 or
take a lock that a D3D11 caller can hold.

Evidence/limits: not console-tested. The check for an existing ticket reads the private data's size. If the
console does not report interface data that way, each report replaces the ticket instead: the totals stay
balanced, but each report costs an allocation. To check on the console: the per-category figures return to idle
after stop/start cycles and follow the textures in use. Category 22 falls back to zero when each rav1g stream stops.

## Decoder pool sized from the stream

`bale_d3d11va_pool_from_stream` (0/1, default 0 = FFmpeg's pool; bale.68) sizes the d3d11va
decoder's frame pool from the stream. FFmpeg allocates that pool once, as one texture array that
cannot grow, with 4 surfaces plus 16 for H.264 and HEVC (the most pictures either standard lets a
stream keep): 20 x 23.7 MiB = 478 MiB at 4K 10-bit (ledger 16). An HEVC stream states its decoded
picture buffer in its SPS (`sps_max_dec_pic_buffering_minus1 + 1`, the largest over the
sub-layers); films typically need 5 or 6. With the key, the fork's `get_format`
(`Source/D3D11DecoderPoolSize.h`, generic) reads that from the codec configuration (hvcC or Annex
B), builds the pool itself with `avcodec_get_hw_frames_parameters`, and gives it the buffer plus 3
surfaces (frames FFmpeg holds for output, and the providers' brief use while they copy or process a
frame), never more than FFmpeg's own size. A typical 4K 10-bit film then gets ~9 surfaces
(~213 MiB, ~265 MiB less). Since bale.69 also:
- **H.264:** `max_dec_frame_buffering + 1` from the SPS's VUI bitstream restriction, or without it
  the level's MaxDpbMbs over the picture size (Table A-1, at most 16), never fewer than
  `max_num_ref_frames`; plus 3. Typical 1080p films: 20 → ~8 surfaces (~35 MiB less at 1080p 8-bit).
- **VP9:** its 8 reference slots + the current frame, plus 2 (VP9 never holds frames back for output
  order): 12 → 11, one surface (23.7 MiB at 4K 10-bit) less.
Since bale.79 a stream whose codec configuration has no parameter sets (e.g. a 23-byte hvcC in MKV,
with VPS/SPS/PPS in-band only) is sized from the SPS of its key frames: before each key frame goes to
the decoder the fork walks its NAL units (length-prefixed per hvcC/avcC, or Annex B), and the
get_format that packet triggers uses the result. Bale found this on a library 4K HEVC Main 10 MKV
that kept FFmpeg's 20 surfaces (478 MiB); its in-band SPS gives a buffer of 6, so 9 surfaces. Console (Bale e894ac9, bale.79): that file logged
`9 surfaces 3840x2176 (codec 173, stream buffer 6, FFmpeg's pool 20)`, ledger 9 surfaces / 215.2 MiB,
a seek to 40 min and no missed frames; a 30-minute 4K HEVC HDR10 soak kept 8 surfaces throughout, app
980–1030 MiB, no missed frames or stalls. Bale made the key its release default for hardware HEVC,
H.264 and VP9 (Bale 8ed6c50). This console's H.264 decoder is 1080p only, so 4K H.264 never reaches
d3d11va here. Anything
it still can't read (a parse error, other codecs) keeps FFmpeg's pool. Logs (since bale.71 prefixed `Bale ` and logged for every branch): `Bale D3D11 decoder pool: N
surfaces WxH (codec C, stream buffer B, FFmpeg's pool D)`, or why it kept FFmpeg's pool.

Sized against: the app memory limit (3328 MiB with `hevcPlayback`, 1024 MiB without) and consoles
with less memory than the Series X. Limit: an SPS that arrives later in the stream with a larger
buffer and no format change keeps the smaller pool, and decoding then runs out of surfaces; that's
why it's opt-in. The SPS reader matched FFmpeg's `trace_headers` on x265 test streams (buffer 3 to 7,
with and without temporal sub-layers, hvcC and Annex B) and survives random and truncated input. The
H.264 reader matched it on x264 streams (progressive, interlaced, High 10; buffer 1 to 5; avcC and
Annex B) and on hand-built SPSs without a VUI (level 3.0 interlaced, 4.1 1080p, 5.1 4K), with
scaling lists and with HRD parameters, and survives random and truncated input.

Console (Series X, bale.68, Bale 19e6a77, 2026-11-22; 30 s 4K60 clips, two seeks each, idle state):
4K HEVC HDR10 60 Mbps with the tone map and 4K HEVC SDR 100 Mbps with the downscale each went from
20 surfaces (478 MiB) to 8 (191 MiB), with 0 missed frames, 0 stalls and 0 drops either way; the
ledger matched and there were no decode errors. A VP9 film kept FFmpeg's own VP9 pool of 12 (4 + 8),
as expected since only HEVC is read.

bale.69 and bale.70 never resized VP9: `avcodec_get_hw_frames_parameters` reports 1 work surface plus
the references (9 for VP9, 17 for HEVC and H.264), and FFmpeg's own pool then adds 3 (decode.c, "4 base
work surfaces"). The fork compared its 11 with that 9 and kept FFmpeg's pool, which came out at 12; HEVC
worked only because 8 is below 17. Since bale.71 it compares with FFmpeg's real pool (parameters + 3).
Console (bale.71, Bale b1e4261): a 4K VP9 film logged `Bale D3D11 decoder pool: 11 surfaces 3840x2160
(codec 167, stream buffer 9, FFmpeg's pool 12)`, ledger 11 surfaces (261 MiB), no missed frames. A
1080p H.264 film (forced to d3d11va; by default Bale plays 1080p H.264 with the system decoder) logged
`8 surfaces 1920x1040 (codec 27, stream buffer 5, FFmpeg's pool 20)`, ledger 8 (22.9 MiB instead of
57.1), no missed frames.

## Shader-readable decoder pools (retired experiment)

Retired in bale.76: `bale_d3d11va_shader_resource` is still accepted (0/1) but ignored, and
`Source/BaleD3D11vaShaderResource.h` is gone. The direct read it enabled removed the stage's 23.9 MiB
scratch copy but raised app memory by ~70–90 MiB that nothing reports (likely driver shadow copies of
decoder-layout surfaces), with no change in fps or drops; the owner dropped it (2026-11-23). The
record of what was measured follows.

`bale_d3d11va_shader_resource` (0/1, default 0) asks whether the stage could read d3d11va decoder
frames in place instead of copying them. FFmpeg decodes into one texture array per pool, and
bale_gpu's tone map copies each frame into a scratch texture first, because a shader resource view
of a decoder-bound array is taken to be invalid in D3D11. That copy is one full-frame GPU copy per
frame on the hardware-decode path (about 24 MB at 4K P010, ~0.6 ms at the idle-state GPU copy rate
of ~40 GB/s) plus a ~24 MiB scratch texture. Sized against: the idle state's ~2.4x slower GPU
copies; the GPU's shader cores are otherwise lightly loaded on this path (fixed-function decode).

With `1`, when the hardware device is set up (first Starting, and after a device change) the fork
creates a small two-slice NV12 and P010 array with `DECODER | SHADER_RESOURCE` and the stage's
plane views of slice 1, and logs `Bale d3d11va shader resource probe: NV12 hr=0x.. P010 hr=0x..;
decoder pools get SHADER_RESOURCE|unchanged`. Only if both formats pass does it add
`SHADER_RESOURCE` to the d3d11va device's bind flags, which FFmpeg 8.1 ORs into every decoder pool
made from that device (FFmpeg 9.1 sets the flag on its own, for driver compatibility). bale_gpu
still copies until it accepts decoder-bound sources; the probe answers whether it could, and the
decoder must keep working with the flag.

Console (Series X, bale.63, Bale 0.1.711, 2026-11-21): the probe passed for both formats
(`NV12 hr=0x00000000 P010 hr=0x00000000; decoder pools get SHADER_RESOURCE`), so D3D11 on the Xbox
does create shader views of slices of decoder-bound arrays. A 4K HDR HEVC film on the d3d11va +
tone-map route (dithered NV12, 3840x2160 to 1920x1080) then played 2910 frames at 24.02 fps with
none dropped or corrupted, and 985 more at 23.99 fps after a seek; no errors, no device removal,
memory unchanged (app 1189 MiB; decoder array 20 slices, 478 MiB). Next: bale_gpu reads such
sources directly instead of copying.

Since bale.64 the flag is added only when the loaded bale_gpu exports
`BALE_TONEMAP_INPUT_DECODER_SRV` (64; Bale d68c966, `Source/bale_tonemap.h` copied from it): its
`bale_tonemap_apply` then reads a decoder-array slice in place when the input has `SHADER_RESOURCE`,
and falls back to its copy once if D3D11 refuses the view. Without the bit the key logs `probe:
skipped (bale_gpu reads decoder arrays through a copy)` and changes nothing.

Console A/B (Bale 0.1.712, bale_gpu d68c966 + bale.63, 4K HDR HEVC tone map, two pairs, 45 s in,
2026-11-21): direct reads removed the 23.9 MiB scratch texture, but app memory rose from
1185/1191 to 1255/1247 MiB (memory nobody reports up by ~90 MiB); 24 fps, no drops, seeks fine
either way. The bind flag alone costs nothing (bale.63 with the old, copying bale_gpu: 1189 MiB).
Likely the driver keeps shader-readable copies of decoder surfaces it stores in a decoder-only
layout (~3-4 slices' worth), so the copy may only have moved into the driver. Bale keeps it off by
default until per-stage GPU timing shows a saving.

## rav1g display pass (trial)

`bale_rav1g_display` (0/1, default 0 = the stage, as upstream; Bale sets 1 by default and 0 for the
A/B; bale.74, rav1g interface version 2) lets rav1g write what the
stage would write for an AV1 stream on the rav1g route: the frame fitted into the stage's output size
and tone-mapped (BT.2390, 1000-nit peak, 100-nit white, no desaturation) or gamut-converted to BT.709
SDR, P010 (or NV12, dithered when the stage would dither it), by one compute pass at the end of
rav1g's own command list (`Rav1gSettings::display`). The fork then copies that display-size image
into its player texture instead of running `BaleToneMap::Process`, and the decoder's images are
display-sized (4 x 5.9 MiB at 1080p instead of 4 x 23.7 MiB). It applies only where the stage runs (HDR
on an SDR display, SDR larger than the display) and not for curve 2 (Hable). The container's colour
code points fill what the sequence header leaves unspecified, and its chroma siting what it leaves
unknown. If rav1g can't do the configuration (`RAV1G_STATUS_INCAPABLE`) the decoder is created without
it and the stage processes the frames as before; so it is when the renderer's device can't create the
display-size images with unordered access (checked once before the decoder is created, since bale.75;
logged `display pass not used (no unordered-access images)`); the generic part is
`GpuVideoDecoder::SetDisplayOutput` and `GpuDecodedFrame::displayOutput`. Logs: `Bale rav1g: display
pass requested`, then `display pass WxH depth=D` or `display pass declined (incapable); the stage
processes the frames`. The delivery line's perf part adds `display n= avg= max= ms` (the pass's GPU
time) and the per-frame line `display=`. rav1g measured the pass at 1.0/1.2 ms per 4K frame to 1080p
(SDR/HDR10), 1.1/1.5 to 1440p and 1.5/1.8 at 4K 1:1 on the console, against Bale's stage at up to
~5.6 ms SDR and ~9 ms HDR10. Sized against: GPU time per 4K60 frame (16.7 ms) and the idle state's
slow GPU copies (the fork's copy is of the display-size image only).

Console (Series X, bale.74 with rav1g v0.7.0-alpha.1, Bale 93a7861, 2026-11-23; 4K60 AV1 60 Mbps to a
1080p SDR display): `display pass 1920x1080 depth=10`, no declines. SDR: 0% missed frames with the
display pass against 3.4–4.9% with the stage; HDR10: 0% against 12–13%. The pass took 1.0 ms (SDR) and
1.18 ms (HDR10) of GPU per frame. rav1g's output images: 47.5 MiB (8 at 1080p) against 237.3 MiB at 4K
with the stage.
Design: rav1g `docs/bale-display-pass.md` at v0.7.0-alpha.1.

Since bale.78 (rav1g interface version 3) the fork also passes the container's MaxCLL and mastering
display maximum as `Rav1gDisplay::max_cll_nits` / `mastering_max_nits` (0 unknown; the stream's own
metadata OBUs win), so rav1g tone-maps from the content's peak as Bale's stage does, and logs `Bale
rav1g: display source peak N nits` whenever the peak a frame used changes
(`Rav1gFrame::display_source_peak_nits`). Debug A/B against the stage's `fixedpeak` / `bilinearchroma`:
`bale_rav1g_display_fixed_peak` and `bale_rav1g_display_bilinear_chroma` (0/1, default 0) set rav1g's
process-wide `av1_set_display_fixed_peak` / `av1_set_display_bilinear_chroma` at each decoder
creation (resolved optionally; a library without them ignores the keys). Console (Bale 463cc3e, bale.78 with rav1g
v0.8.0-alpha.1, a 4K AV1 HDR10 film): `display source peak 452 nits` from the container's metadata,
1000 with the fixed-peak switch; display pass 1920x1012 depth=10.

## Software frames through the stage

Since bale.80 software-decoded video (any codec, `UncompressedVideoSampleProvider`) goes through the
same stage as d3d11va frames: `bale_tonemap`, `bale_tonemap_output_width/height` (an SDR source larger
than the target is downscaled without a curve), `bale_tonemap_output_depth` and
`bale_tonemap_white_nits` apply as on the hardware path. It reuses the staged upload of
`bale_film_grain_mode=3`: swscale writes each frame into a mapped STAGING texture (ring of three) of
the stage's input format at the decoded size (NV12 for 8-bit when the stage reads NV12, else P010; so
4:2:2, 4:4:4 and 12-bit sources reach the stage as P010), the upload copies it (or applies the D3D11
grain pass) into a shader-readable texture, and the `VideoFrameProcessor` writes the processed
output, which the stream presents with the fixed BT.709 contract and the tone-map mark, like the D3D11
provider. A device, upload or stage failure fails the stream (`NotifyError`), never an unprocessed
frame; so does a frame whose size differs from the stream's. Not for zero-copy streams or frame
grabbers. Logs: `Bale software stage: WxH NV12|P010 -> wxh NV12|P010`, `... active`, `... frames=N`
every 240 frames, or `declined` (odd size, or an input the stage can't read). Media Foundation's own
CPU copy of the frame goes away too (swscale writes the GPU staging texture directly).

Why: Bale's software claims (H.264 High10/4:2:2, HEVC RExt, VP9 profiles 1/3 at 1080p; 4K30 H.264
next) must not let Media Foundation scale 4K software frames to a 1080p display (the per-open msvproc
leak) and need the tone map for HDR. Sized against: the ~3-core CPU budget (the frame is touched on
the CPU once, by swscale) and the idle state's GPU copies (one staging-to-texture copy per frame). Not
console-tested yet.

Since bale.81, conversions beyond a 4:2:0 8-bit interleave (4:2:2 and 4:4:4, more than 8 bits, other
formats; in this stage path and in the regular CPU path) use swscale's slice threads
(`Source/ThreadedScaler.h`: `sws_alloc_context`, the `threads` option, up to four, and
`sws_scale_frame` into the caller's planes, the destination taking the source's colour description so
only the format and size change). 8-bit 4:2:0 keeps the single `sws_scale`, a plain interleave. Frame
grabbers keep the old path. Bale measured 4:2:2 10-bit 1080p30 at 1.8–2.1 cores with the single
call; this is for 4K 4:2:2 and 1080p60.

Console (Bale 63349b5, bale.81): the stage ran on 4K H.264 software to 1080p at 1.6/1.9/2.8 cores
(the three bitrates tested), with app memory ~400 MiB lower than with Media Foundation scaling; 25
consecutive 4K opens had no missed frames and no leak; a 4:2:2 HEVC HDR10 software stream was
tone-mapped (P010 to NV12), no missed frames; the hardware and AV1 routes were unchanged. The
threaded swscale left 1080p non-4:2:0 CPU flat to +9% against bale.79 (the threads add work, they
don't remove it; the conversion's wall time per frame is what they shorten, which matters only
where one core can't keep up).

## D3D12 video decode probe

`bale_d3d12va_probe=1` (default 0) logs, for each hardware-decoded video stream at open, whether the
GPU would decode it through D3D12 video, FFmpeg's `d3d12va` hwaccel (`Source/D3D12VideoDecodeProbe.h`).
Decoding is unchanged: the stream keeps d3d11va. One line:

`Bale d3d12va probe: supported|not supported|no D3D12 profile for this stream (codec C profile P WxH
D-bit): hwaccel=0|1 device=<hr> level=<max feature level> video=<hr> profiles=N check=<hr> flags=F
config=C tier=T ffmpeg=<av error>`

- `hwaccel`: the FFmpeg decoder has a d3d12va hwaccel (native FFmpeg 9.1 builds them for H.264, HEVC,
  VP9, AV1, MPEG-2 and VC-1, autodetected, no configure change).
- `device`: `D3D12CreateDevice` at feature level 11_0 on the default adapter; `level`: the highest
  feature level it reports (0xc000 = 12_0).
- `video`: `ID3D12VideoDevice` query; `profiles`: decode profiles the device reports.
- `check`, `flags`, `config`, `tier`: `D3D12_FEATURE_VIDEO_DECODE_SUPPORT` for the stream's profile,
  NV12/P010 and size (flag 1 = supported).
- `ffmpeg`: `av_hwdevice_ctx_init` of a d3d12va context on that device (0 = FFmpeg accepts it).

The probe creates the device itself: FFmpeg's `av_hwdevice_ctx_create(D3D12VA)` asks for feature
level 12_0 and failed on the console with `AVERROR_UNKNOWN` (bale.82, Bale 0.1.778: HEVC 4K 10-bit,
H.264 1080p and VP9 1080p/4K all `device=-1313558101`), so bale.82's lines only showed that FFmpeg
can't create its own D3D12 device there, not whether the console decodes through D3D12. rav1g creates
its D3D12 device at 11_0 on the console.

The device is created and released inside the probe (one D3D12 device for a few milliseconds per
open; nothing stays allocated). Why: upstream PR #484 adds D3D12 hardware decoding; FFmpeg's D3D12
decoders use one texture per surface instead of a texture array, which could let the stage read
decoder output in place (the array is why the retired direct-read experiment cost ~90 MiB) and size
the pool per surface. Upstream's maintainers don't know whether the Xbox exposes D3D12 video
decoding at all; this answers that before any decode route is written.

Console (bale.83, Bale 0.1.779, Series X): **D3D12 video decoding is available.** HEVC Main10
3840x2160, H.264 High 1920x1080 and VP9 profile 0 at 1920x1080 and 3840x2160 all reported
`supported`: `device=0` (D3D12CreateDevice at 11_0 succeeds), `level=0xB000` (11_0 is the highest
feature level the app partition gets, which is why FFmpeg's 12_0 device creation fails), `video=0`,
`profiles=11`, `flags=0x1`, `config=0x0` (no reference-only allocations or other constraints),
`tier=2`, `ffmpeg=0` (FFmpeg's d3d12va accepts a caller-created device). Playback (d3d11va) was
unaffected: 0 missed frames on all four.

## In-band caption packet tap

`bale_cc_sink=<token>` (decimal, 1..999999999; unset = off) hands every compressed packet of a
H.264, HEVC or MPEG-2 video stream to Bale's caption decoder in `bale_gpu.dll`, which parses A/53
cc_data (SEI) or GA94 user data, reorders by PTS, decodes CEA-608/708 and renders. The fork parses
nothing (`Source/BaleCaptionSink.h`). ABI 1, resolved with `LoadPackagedLibrary`/`GetProcAddress`:

```c
uint32_t bale_cc_abi_version(void);  /* must be 1 */
int32_t  bale_cc_push(uint64_t token, const uint8_t *data, size_t len, int64_t pts_100ns,
                      uint32_t codec, uint32_t nal_length_size);
int32_t  bale_cc_flush(uint64_t token);
```

- `codec`: 1 H.264, 2 HEVC, 3 MPEG-2 (other codecs are not tapped).
- `nal_length_size`: 1, 2 or 4 for length-prefixed H.264/HEVC (from avcC/hvcC, MP4/MKV); 0 for
  Annex B (MPEG-TS, raw) and MPEG-2.
- `pts_100ns`: the packet's PTS on the timeline of the `MediaStreamSample` timestamps (start offset
  and stream delay applied); `INT64_MIN` when the packet has none. Packets arrive in decode order.
- `data` is the demuxed packet, read-only and valid only during the call.
- Where: `MediaSampleProvider::GetNextPacket`, as a provider takes the packet for decoding, so every
  route (system decoder, d3d11va, software, GPU decoder) is covered and a buffered seek hands the
  kept packets again. Called on the sample thread (or a GPU decoder's feeder thread): return quickly,
  never block, never call back into the player.
- `bale_cc_flush` on every flush of the stream: seek, stream switch, close.
- Off: one compare per packet, no copy, no call. A missing export or another ABI version logs
  `Bale captions: sink unavailable; tap off` once. Each tapped stream logs `Bale captions: tap on
  (codec N nal_length_size N)` and, when it closes, `Bale captions: tap closed (packets=N flushes=N)`.
- Frame grabbers are never tapped. A remount is a new media source: pass the key again.
- Console (bale.84, Bale 0.1.790/791, 2026-11-26), as Bale reported it: the tap worked on the six
  cea608 test clips (H.264 in MP4, MKV and TS); it logged `tap on (codec 1 nal_length_size 4)`, and the
  packet and flush counts were as expected.

## Console evidence (Xbox Series X, app mode, SDR display)

Measured through Bale's debug server; full notes are in Bale's `docs/player-plan.md`.

- d3d11va (`VideoDecoderMode.Automatic`) decodes HEVC Main10 and VP9 profile 2 as P010, including
  4K and Dolby Vision profile 8.1 (RPUs ignored), in real time. The Xbox media pipeline does no
  HDR-to-SDR conversion of tagged P010 on an SDR display, so `bale_tonemap` is needed there.
- `bale_tonemap=1` (BT.2390, 1000-nit peak, 100-nit white) against the Silo server's transcode,
  luma min/p1/p50/p99/max and saturation: 1080p HDR10 HEVC 0/4/62/169/218, 0.62 vs server
  0/4/61/177/223, 0.60 (untone-mapped 0/6/69/128/154, 0.40); 1080p HDR10+ base layer
  0/9/32/147/164, 0.62 vs 0/9/32/149/166, 0.61. Hable (`2`) is darker (max 198). bale_gpu's
  self-test passes on the console. Other sources (e.g. BT.709 SDR) log `request ignored` and play unchanged.
- HLG (frame transfer 18 on BT.2020) through the same stage looks correct on the console (bale.29).
- Bit depth (bale.42, Bale 0.1.659): 10-bit 4:2:2 and 4:2:0 software decode → P010; 8-bit
  4:2:2 and full-range → NV12; the tone-map and SDR downscale routes deliver marked P010
  1920x1080 at 4K sources, accepted by Bale's guard, 0 drops.
- Display-sized output (bale.38–40): 4K HDR tone-mapped and 4K SDR VP9 (unspecified tags)
  downscaled to 1920x1080 by the stage each survived 40 opens, where Media Foundation's own
  4K SDR downscale died at about 23.
- rav1g AV1 (bale.54, rav1g v0.4.0-alpha.1, Bale 0.1.680, 1080p 23.976 10-bit film, 4 units in
  flight): 23.95–23.99 fps, 0 dropped, delivery ~24.1/s; 40 back-to-back seeks in 46 s; 20
  stop/start cycles, every pool released and every D3D11 device destroyed; clean end of stream;
  memory 804 MiB playing, 338 MiB idle, no growth across 21 opens. Units in flight 2/3 gave
  22.6–23.5/24.0–24.3 frames/s. First run (bale.44–45): a null codec descriptor crash and a pool
  deadlock (MF held decoder images), both fixed.
- GPU cost of the stage (slice copy included): about 20 points of 3D-engine use, about 8 ms per
  4K24 frame (HEVC and VP9 alike); CPU unchanged.
- Stream side data (c3a9adb, 2c08f7c): MaxCLL/MaxFALL 4057/321 and mastering 1000/0.0001 nits
  now match Windows' own MKV source (previously garbage, then a truncated minimum).
- Memory: with `hevcPlayback` the app has one 3328 MiB heap that includes GPU memory
  (`GetExtendedMemoryInfo` graphics partition 0/0). 1080p HEVC uses ~0.6–0.7 GiB, 4K VP9 HDR
  1.2–1.3 GiB; no growth over 5-minute runs on either d3d11va build.
- `bale_compressed_color_info=1` (with HDR colour info enabled): 4K HDR10 VP9 reaches the system
  decoder with PQ, BT.2020, MaxCLL and mastering attributes (previously none).
- Not yet verified: the display-matrix rotation fix, and what `bale_compressed_color_info` changes
  on an HDR TV (the test display is SDR).

## Cut a release

Choose a fresh version (the first fork build is `2.1.0-bale.23`):

```sh
gh workflow run bale-wrapper.yml -R Zorro92/bale-ffmpeginteropx -f version=2.1.0-bale.N -f publish=true
```

Use `-f publish=false` (the default) for a full dry run: it builds and uploads the
same artifacts without creating a tag or release. `publish=true` refuses a tag
already present in either this fork or `Zorro92/bale-for-silo`, where older wrapper
releases live. Releases are immutable: never replace an asset or reuse a version.
Only the publish job has repository write permission. Publishing here does not
update Bale's consumer version or SHA256 pin.

## Source guard and upstream merges

[`ffmpeginteropx-wrapper.json`](ffmpeginteropx-wrapper.json) pins the original
package, SDK, toolset, library version and last merged `upstream_base`. The base
must be an ancestor of HEAD, and the checkout must be clean (including staged and
untracked files). The diff from that base to HEAD may touch only:

- `.cpp` and `.h` files under `Source/`, including nested directories.
- Fork-owned `bale/**`, `.github/workflows/bale-*.yml`, `AGENTS.md`, `NOTICE` and `README.md`.

IDL, project files, `packages.config`, `Build/` and all other upstream files are
protected. Renames check both the old and new paths. API/ABI changes need a
separate projection/package design, not an exception to the guard.

To merge upstream, fetch `upstream`, merge `upstream/master`, and bump
`upstream_base` in the wrapper pin to that merged upstream commit in the same
push. Review the merge and keep the public WinRT API compatible with the retained
2.1.0 projection. The guard checks the fork's changes relative to the new base;
it does not approve upstream changes on your behalf.

Native FFmpeg is still built and released in
[`Zorro92/bale-for-silo`](https://github.com/Zorro92/bale-for-silo).
[`ffmpeg-native.json`](ffmpeg-native.json) retains its native build provenance,
including its independent `upstream_commit`. The wrapper downloads that exact
release package and verifies its SHA256 and embedded dav1d version; it never
builds FFmpeg, restores upstream FFmpeg packages, or checks out `Libs` submodules.
