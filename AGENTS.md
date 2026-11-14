# bale-ffmpeginteropx — guide for coding agents

This is Bale's fork of [FFmpegInteropX](https://github.com/ffmpeginteropx/FFmpegInteropX).
[Bale](https://github.com/Zorro92/bale-for-silo) is an Xbox client for Silo; it consumes this
fork as the package `FFmpegInteropX.UWP.Lib 2.1.0-bale.N`. Read the fork section of
[README.md](README.md), [bale/README.md](bale/README.md) (CI, releases, source guard, upstream
merges) and, for how Bale uses the decoder, Bale's `AGENTS.md` ("Playback decoder policy").

## Write code as if it could go upstream

Bale targets the Xbox only; upstream serves the whole Windows ecosystem. We are **not** sending
pull requests upstream now, and nothing goes upstream without the owner's decision and very
thorough review and console testing (probably not before Bale is stable). Still, write every new
change so it *could* be offered later without a rewrite:

- **Match upstream's code.** C++/WinRT as upstream writes it: same file layout (one sample
  provider per class, headers beside sources), naming (PascalCase types and methods, camelCase
  members), `winrt::com_ptr`/`check_hresult`, `DebugMessage` for debug output, 4-space indent,
  LF line endings in the repo. Read the neighbouring upstream code before adding to it.
- **Keep generic features generic.** Something any FFmpegInteropX app could use (hardware decode
  paths, colour/HDR handling, tone mapping, subtitle output) is written as a general feature with
  neutral names and no Bale assumptions.
- **Keep Bale/Xbox glue behind one seam.** Calls into Bale's `bale_gpu.dll`, Bale's tuning
  defaults, and Bale-only failure policy (e.g. "fail so the app asks the server to transcode") sit
  behind a small, clearly named interface or `Bale*.h` header, so the generic part can be lifted
  out without them.
- **Small, topical commits.** One feature or fix per commit, a subject in the imperative and a
  body that says *why*, so a change can later be cherry-picked onto a clean upstream branch.
- **Record evidence.** Console measurements, A/B results and known limits go in the commit body or
  `bale/README.md`. An upstream reviewer will want them; so will we.
- The existing Bale commits (former patches 0001–0022) stay as they are; don't rewrite history.
  If one becomes an upstream candidate it gets reimplemented cleanly on its own branch.

## Hard constraints

- **The public WinRT API is frozen.** Bale ships the original 2.1.0 winmd and managed projection,
  so the IDL, project files, `packages.config`, `Build/` and other upstream files must not change.
  The source guard in CI (`bale/scripts/ffmpeginteropx-wrapper-common.ps1`) enforces this; don't
  widen it without the owner. Changes go in `Source/**/*.cpp|h` only.
- **Options are `bale_*` keys in `MediaSourceConfig.FFmpegOptions`** (decimal strings), consumed
  by the wrapper and never passed to FFmpeg. Each new key is opt-in (unset = upstream behaviour),
  has a documented range and default, and is described in the commit as an upstream reviewer would
  want, so it can later become a real `MediaSourceConfig` property.
- **Logging:** fixed diagnostic strings, numbers and HRESULTs only. Never log URLs, paths, headers,
  tokens or exception text. FFmpeg's native log provider stays off in Bale's production builds.
- Native FFmpeg (and its configure flags) is built in Bale, not here. A change that needs a new
  native FFmpeg build (e.g. a hwaccel) needs a matching Bale native release first.

## Building and testing

- Development is on Linux; the wrapper builds only on Windows in GitHub Actions. Pushes to `main`
  run the fast checks; run them first locally with `pwsh` (`bale/scripts/test-*.ps1`).
- A full build is a dry run: `gh workflow run bale-wrapper.yml -R Zorro92/bale-ffmpeginteropx
  -f version=2.1.0-bale.N -f publish=false`. Only publish (`publish=true`) when asked; releases
  are immutable and Bale pins them by SHA256.
- Behaviour is verified on the console through Bale's debug server (Bale's
  `playback-test-on-xbox` skill), and only when the owner says the console is free.

## Conventions

- Push straight to `main`; no feature branches (upstream PR branches, when there are any, are the
  only exception). CI must stay green here and in Bale.
- End commit messages with the agent's `Co-Authored-By` line. Use `git commit -F` with a heredoc;
  don't put backticks inside a double-quoted `-m`.
- Never commit IPs, tokens, server addresses, local absolute paths or media titles.
- Licence: Apache-2.0 like upstream. Keep `LICENSE`; list newly added or modified upstream files in
  `NOTICE`.
