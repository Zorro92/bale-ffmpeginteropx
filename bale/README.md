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
chooses `D3D11VideoSampleProvider` and the source is 10-bit with codecpar transfer
SMPTE 2084 (PQ). Other providers/formats log one ignored-request diagnostic and
keep upstream behaviour. Unset/zero does not load the tone-map ABI or process frames.

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

## Console evidence (Xbox Series X, app mode, SDR display)

Measured through Bale's debug server; full notes are in Bale's `docs/player-plan.md`.

- d3d11va (`VideoDecoderMode.Automatic`) decodes HEVC Main10 and VP9 profile 2 as P010, including
  4K and Dolby Vision profile 8.1 (RPUs ignored), in real time. The Xbox media pipeline does no
  HDR-to-SDR conversion of tagged P010 on an SDR display, so `bale_tonemap` is needed there.
- `bale_tonemap=1` (BT.2390, 1000-nit peak, 100-nit white) against the Silo server's transcode,
  luma min/p1/p50/p99/max and saturation: 1080p HDR10 HEVC 0/4/62/169/218, 0.62 vs server
  0/4/61/177/223, 0.60 (untone-mapped 0/6/69/128/154, 0.40); 1080p HDR10+ base layer
  0/9/32/147/164, 0.62 vs 0/9/32/149/166, 0.61. Hable (`2`) is darker (max 198). bale_gpu's
  self-test passes on the console. Non-PQ sources log `request ignored` and play unchanged.
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
