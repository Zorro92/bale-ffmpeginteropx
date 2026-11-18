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

### Retired debug options

`bale_tonemap_output` (0–15) and `bale_tonemap_format` (0–3) were debug switches used to
find the Xbox presentation leak. They are retired: still accepted with their old ranges
(an unknown `bale_*` key would fail the open, and older Bale builds may still send
them), ignored, and logged once as `Bale option <key> retired; ignored`. The stage now
always outputs P010 when bale_gpu can write it (NV12 only for an older bale_gpu).

### rav1g delivery diagnostics

Every 240 delivered samples (and at a seek and at stream end) the rav1g route logs
`Bale rav1g: delivery samples=N rate=../s latency avg=.. max=.. ms pts step min=.. max=.. ms
backwards=N gaps=N queue max=N`: samples delivered per wall-clock second, decode-to-deliver latency (from feeding a temporal unit to
handing its frame to the player), the timestamp steps between delivered samples (in sample
time, as the player receives them; bale.49 printed stream time-base units by mistake), steps that
went backwards, gaps over 1.5 sample durations, and the deepest ready-frame queue. The first 24
samples after an open or seek are also listed: `first samples pts ms=a,b,c,...`.

With the player-side pool (bale.46+), the decoder's fixed pool no longer limits the player's
buffer, so decode-ahead and BufferTime can be set as for dav1d. Mind the memory: each buffered
frame is a player-side texture (1080p P010 ≈ 6 MiB, 4K ≈ 25 MiB).

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
`eebfbd7`, interface version 1; copy it again, unchanged, when rav1g changes it). No new
native FFmpeg build is needed; Bale ships `rav1g.dll` in its package.

**Fallback at open.** `BaleRav1gDecoder::TryOpen` checks: `rav1g.dll` loads
(`LoadPackagedLibrary`, no import library), every export it uses is there,
`rav1g_abi_version()` equals the header's `RAV1G_ABI_VERSION`, the profile is Main (4:2:0 or
monochrome), `rav1g_peek_stream_info` reads the container's sequence header (av1C config
OBUs; without one, the probed pixel format) with depth 8 or 10, and the process device
exists. Any failure logs `Bale rav1g: fallback to dav1d (reason, status=0x..)` and the
stream opens on today's dav1d path with the keys above honoured. The decoder itself is
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
images not created yet. `low_memory`, slots, threads and the process-global `rav1g_set_*`
switches are never touched. The device handle comes from the MSS device manager at the
first Starting request; a lost device fails the stream.

**Flow and threads.** The sample thread (the decode-ahead worker when
`bale_decode_ahead_frames` > 0, as Bale sets for AV1, else the MSS request thread) reads a
packet, calls `rav1g_decoder_decode` with user data = index into a table of {pts,
duration, generation}, and repeats until a frame is queued. rav1g's `frame` callback
(completion thread) only queues the frame. The sample thread has the immediate context
wait for the frame's fence (`ID3D11DeviceContext4::Wait`, on the GPU), under the device's
`ID3D10Multithread` protection, then hands the texture to MF or runs the stage. End of
stream drains. `NEEDS_RECONFIGURE` drains, recreates the decoder and decodes the unit again.
Matroska may leave the sequence header out of blocks: a decoder created from the
container's header gets it in front of its first unit.

**Images.** 8 per stream, never more: D3D11 textures (`RENDER_TARGET | SHADER_RESOURCE |
UNORDERED_ACCESS`, `MISC_SHARED_NTHANDLE | MISC_SHARED`; without unordered access if the
driver refuses, then rav1g copies) opened in D3D12 once. `get_image` waits for a free
image of the request's size (rounded to even) and format (NV12 for 8-bit, P010 for
10-bit); images of an older size are released as they come back. It returns non-zero only
to cancel (stop, seek) or on failure. Images return on the sample's `Processed` (texture
stored per sample) or, for processed output, right after the stage; the releaser signals a
D3D11 fence that `get_image` waits for before rav1g writes the image again. A flush also
takes back the images of samples MF still holds, as the D3D11 path does.

**Seek, stop.** MSS Starting and Close first cancel any `get_image` wait without taking
the source lock (a wait can last a whole pause and the waiting thread may hold it).
Flush bumps the generation, drains rav1g (image requests fail meanwhile; drain errors are
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
