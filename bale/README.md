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
