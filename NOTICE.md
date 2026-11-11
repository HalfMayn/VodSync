# Third-party components

vodsync itself is MIT licensed (see `LICENSE`). It is built against, and its
released binaries ship alongside, the components below. None of them are kept
in this repository — `.gitignore` excludes `third_party/` binaries, and they
are fetched by hand as described in `README.md`. Each licence below was read
from the copy actually vendored, not assumed.

| Component | Used as | Licence |
|---|---|---|
| [Clay](https://github.com/nicbarker/clay) | `third_party/clay/clay.h`, compiled in | zlib/libpng |
| [raylib](https://github.com/raysan5/raylib) | window, input, GL, text; linked | zlib/libpng |
| [mpv](https://github.com/mpv-player/mpv) (libmpv) | video decode and playback; dynamically linked | **GPLv2-or-later** — see below |
| [yt-dlp](https://github.com/yt-dlp/yt-dlp) | separate process, for VOD lookup and download | Unlicense (public domain) |
| [TwitchDownloader](https://github.com/lay295/TwitchDownloader) | separate process, for chat export | MIT |

Clay's licence is stated in `clay.h`; raylib's in the `LICENSE` of its
release archive; yt-dlp's and TwitchDownloader's in the `LICENSE` and
`COPYRIGHT.txt` shipped in their own distributions.

## The bundled libmpv is a GPL build

mpv's own header is explicit that its two parts are licensed differently: the
client API is ISC, while **the core is GPLv2-or-later by default and
LGPLv2-or-later only when built with `-Dgpl=false`**. Which applies depends on
how the particular libmpv being shipped was compiled.

Run `vodsync --where` to see it. For the Windows build vendored here:

- **mpv v0.42.0-1-g3138fc8ee**, built by
  [mpv-winbuild-cmake](https://github.com/shinchiro/mpv-winbuild-cmake) in CI.
- Its meson command line lists every `-D` flag explicitly, and **`-Dgpl=false`
  is not among them** — so the default, `gpl=true`, applied. This is a GPL
  build.
- Its FFmpeg (N-128420-gbaa68e68b) carries x264 and x265, which FFmpeg will
  only link with `--enable-gpl`, so that side is GPL too.

The Linux and Steam Deck archive bundles `libmpv.so.1` from the Steam Runtime
"sniper" repository — Debian's libmpv, which Debian packages as GPL-2+.
Confirm it the same way, by running `--where` from the Linux build.

### What that means for distributing binaries

Nothing for this repository: the source here is MIT and carries no GPL code.
It matters when **shipping a built binary**, because the Windows installer and
the Deck archive each bundle libmpv, making them a combined work containing
GPLv2+ code:

- MIT is GPL-compatible, so combining the two is permitted. The MIT source
  stays MIT and can still be used under MIT by anyone.
- The **combined binary** is effectively under GPLv2+, and the GPL's
  source-availability terms apply to it. In practice that means shipping
  mpv's licence text with the binary and making the corresponding mpv source
  available — naming the exact upstream build above is the usual way.
- To keep a released bundle permissive instead, swap in a libmpv built with
  `-Dgpl=false` and re-check with `--where`.

This file records what was measured. It is not legal advice; if the licensing
of a public release matters to you, have someone qualified look at it.
