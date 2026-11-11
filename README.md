# vodsync

Watch two streams of the same moment side by side, lined up to the second.

When two people stream the same thing, their VODs start at different times,
run at different lengths and sit on different platforms. vodsync aligns them
by wall-clock time rather than by position, using the `EXT-X-PROGRAM-DATE-TIME`
stamps in the HLS manifests, so scrubbing moves both panes to the same real
moment — and replays the chat that was on screen at that moment too.

Twitch and Kick. Windows and Linux, including the Steam Deck.

## What it does

- **Wall-clock sync.** One timeline drives every pane. A pane whose recording
  stopped and restarted mid-broadcast is handled as several sections, so the
  alignment survives a dropped feed.
- **Chat replay** with 7TV and BTTV emotes, avatars, a search, and one tile per
  chat source so two streamers' chats can run beside each other.
- **Layouts**: side by side, vertical split, a draggable and resizable
  picture-in-picture, and a theater mode that gives the video the whole window.
- **Per-pane nudge** for when a platform's timestamps are slightly off.
- **Channel browser** that lists a name's recent VODs on both platforms at once,
  which is the quickest way to spot a simulcast pair worth syncing.
- **Live streams** as well as finished VODs.
- **Sessions** are saved as you go, not only on the way out.

In progress: downloading VODs for offline playback. The core is in
`src/vs_download.c`; there is no UI for it yet.

## Building

Nothing here needs a package manager, but four dependencies are not in the
repository and have to be put in place first. `third_party/clay/clay.h` and the
mpv headers are included; the rest are not:

| Into | What |
|---|---|
| `third_party/raylib/` | a raylib release for your compiler |
| `third_party/mpv/` | `libmpv-2.dll` and `libmpv.dll.a` (Windows), or the distro's libmpv |
| `third_party/yt-dlp/` | the `yt-dlp` binary |
| `third_party/twitchdownloader/` | `TwitchDownloaderCLI` |

Then:

```
python tools/build.py          # Windows, MSVC
python tools/build_linux.py    # Linux
```

`tools/deck-container/README.md` covers the Steam Runtime "sniper" build used
for the Steam Deck, and `tools/installer/vodsync.iss` is the Inno Setup script
for the Windows installer.

Chat replay runs out of process and needs Python; playback does not.

## Releases

Built binaries are attached to GitHub Releases rather than committed — the
Windows installer and the Steam Deck archive are each around 100–190 MB, and
GitHub rejects any file in a repository over 100 MB.

Before publishing binaries, read `NOTICE.md`: whether the bundled libmpv is a
GPL or an LGPL build changes what distributing them requires.

## Notes

`NOTES.md` is the engineering log — why things are built the way they are, what
broke on the way, and what is still unverified. It is written for whoever has
to change this code next.
