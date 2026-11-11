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
| [mpv](https://github.com/mpv-player/mpv) (libmpv) | video decode and playback; dynamically linked | see the note below |
| [yt-dlp](https://github.com/yt-dlp/yt-dlp) | separate process, for VOD lookup and download | Unlicense (public domain) |
| [TwitchDownloader](https://github.com/lay295/TwitchDownloader) | separate process, for chat export | MIT |

Clay's licence is stated in `clay.h`; raylib's in the `LICENSE` of its
release archive; yt-dlp's and TwitchDownloader's in the `LICENSE` and
`COPYRIGHT.txt` shipped in their own distributions.

## libmpv needs checking before binaries are distributed

mpv's own header is explicit that the two parts are licensed differently:
the client API is ISC, while **the mpv core is GPLv2-or-later by default and
only LGPLv2-or-later when built with `-Dgpl=false`**.

Which of those applies depends on how the particular `libmpv-2.dll` /
`libmpv.so` being shipped was compiled, and that is not recorded anywhere in
this repository. It matters only for *distributing built binaries*, not for
this MIT-licensed source:

- If the vendored libmpv is an **LGPL** build, shipping it dynamically linked
  alongside an MIT application is straightforward.
- If it is a **GPL** build, distributing the combined binary brings GPL
  obligations with it, including making the corresponding source available.

So before attaching installers or the Steam Deck archive to a release, check
where that libmpv build came from and how it was configured. On Linux the
Steam Runtime "sniper" build links the distribution's own `libmpv`, which is
a packaging question rather than a bundling one.

This file records what is known. It is not legal advice.
