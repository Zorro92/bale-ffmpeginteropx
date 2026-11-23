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
not the pool. A/B on the 40 Mbps clip (30 s): without the downscale stage (rav1g's 4K
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
started` and `prewarm ready ms=N` (or `prewarm failed (status=..)`). The adapter is rav1g's choice,
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
v0.6.0-alpha.1, `88c05f0`, interface version 1; copy it again, unchanged, when rav1g changes it).
`Source/av1_perf.h` is rav1g's `libav1/dx/av1_perf.h` at tag v0.6.0-alpha.1, copied verbatim
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
`0x80042B2N` = rav1g status N; `E_INVALIDARG` → unsupported media format).

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
Not console-tested yet.

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

## Shader-readable decoder pools (experiment)

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
