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
