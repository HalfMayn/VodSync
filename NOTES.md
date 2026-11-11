# vodsync — working notes

Local app that plays two or more Twitch/Kick VODs side by side and keeps them
aligned on real wall-clock time.

**Status: working.** Video renders on both platforms, synced. Cross-platform
drift measured at 51–66 ms on a real Twitch/Kick simulcast.

---

## Build and run

```bash
python tools/build.py app        # -> build\vodsync.exe
python tools/build.py test       # -> build\vs_test_align.exe (headless resolver/alignment check)
python tools/build.py installer  # -> build\installer\vodsync-<ver>-setup.exe
```

`tools/build.py` captures the environment from `vcvars64.bat` and invokes
`cl.exe` by absolute path. Don't try to drive this from the shell directly —
MSYS mangles `cmd.exe` arguments (`/d` becomes a path, `nul` becomes
`/dev/null`), which cost an hour.

Toolchain on this machine: MSVC 14.38, Windows SDK 10.0.22621,
`/std:c17 /Zc:preprocessor`.

### Runtime flags

| Flag | Purpose |
|---|---|
| `<url> [<url> ...]` | Load VODs on startup (max 4) |
| `--selftest <secs>` | Run N seconds, dump state + render-command stats, screenshot to `selftest.png`, exit |
| `--at <secs>` | Seek this far into the master VOD during a selftest. Note the number is a position in the **master**, which may start at a different wall-clock instant from the other pane |
| `--watch <secs>` | Selftest: report every pane's position, wall clock and part count once a second, starting from the `--at` seek. The only way to tell a pane that is frozen from one that is playing |
| `--layout <0\|1\|2\|3>` | Start in split / grid / focus+PiP / solo+chat |
| `--vsplit` | Start the split view stacked rather than in columns |
| `--swap` | Test hook: exchange the split view's two panels, as the Swap button and `X` do |
| `--text-scale <f>` | Start at this UI text scale (0.7-5.0) |
| `--fullscreen` | Start borderless fullscreen |
| `--chat-vod <url>` | Load a Twitch VOD as the chat source (no video) |
| `--pip <x> <y> <sx> [sy]` | Start with the PiP at this position (0-1) and scale; `sy` defaults to `sx` |
| `--pip-drag <x1> <y1> <x2> <y2>` | Test hook: grab the PiP's resize grip at one point and release at another, printing the strip box before and after |
| `--reset` | Test hook: print what is loaded, reset, print it again |
| `--chat-vod-commit <url>` | Test hook: type a url into the chat panel's "chat from another VOD" field and press Enter |
| `--theater` | Start in theater mode |
| `--live-back <s>` | Selftest: scrub this far off the live edge, then go back |
| `--size <w> <h>` | Window size (default 1600x950) |
| `--open-menu` | Open the layout dropdown at start, to test theater pinning |
| `--toggle-solo <pane>` | Test hook: the double-click solo toggle |
| `--export-logo <path>` | Write the mark to a PNG and carry on |
| `--export-logo-set <dir>` | Write the icon sizes as PNGs and exit (used by the icon build) |
| `--chat <path>` | Load a chat log on startup |
| `--where` | Print the exe folder, the state folder, and the yt-dlp and python commands, then exit |
| `--browse <channel>` | Open the channel browser on this channel at startup |
| `--pick <n>` | Test hook: load browser row n from both platforms into slots 1-2 |
| `--chat-auto` | Start with chat auto-update enabled |

### Debug environment variables

| Variable | Effect |
|---|---|
| `VODSYNC_MPV_LOG=<level>` | Forward mpv's own log (`v`, `debug`, `warn`, …) to stderr |
| `VODSYNC_PROBE_FBO=1` | Every 400th render, read mpv's target FBO back and print pixel sum/nonzero count |
| `VODSYNC_HWDEC=<spec>` | Override the hwdec priority list (`no`, `auto-copy`, `nvdec`, …) |
| `VODSYNC_BG=#RRGGBB` | Force mpv's background colour — the key diagnostic below |
| `VODSYNC_MUTE=1` | Mute every player (useful when testing two at once) |
| `VODSYNC_CACHE_MB=<n>` | Per-pane demuxer cache in MiB (default 96) |
| `VODSYNC_CHAT_STUB=1` | Chat export generates synthetic messages, no network |
| `VODSYNC_CHAT_TOOL=<path>` | Path to TwitchDownloaderCLI |
| `VODSYNC_YTDLP_COOKIES=<browser>` | Use your own browser session for yt-dlp (`firefox`, `chrome`, ...) |
| `VODSYNC_NO_ICON=1` | Skip window-icon generation (kept from debugging the issue below) |
| `VODSYNC_FAKE_MOUSE_X/Y=<px>` | Pretend the pointer is here, for testing reveals. Feeds Clay's hover state too, so proximity and hover agree |
| `VODSYNC_FONT=<path>` | Use this .ttf instead of the per-platform candidates |
| `VODSYNC_YTDLP=<cmd>` | How to invoke yt-dlp |
| `VODSYNC_PYTHON=<cmd>` | How to invoke the bundled helper scripts |
| `VODSYNC_LIVE_LOG=1` | Report every live playlist refresh (edge, held segments) |
| `VODSYNC_LIVE_START_SEGS=<n>` | Segments behind the edge a live pane starts, default 4 |
| `VODSYNC_TEXT_CACHE=<words>` | Clay's measure-text cache size, to force the overflow path |
| `VODSYNC_AUTOSAVE_PATH=<file>` | Use this autosave file, and write it even under `--selftest` |

---

## What is verified working

- **Resolution.** Twitch and Kick both resolve through yt-dlp to plain
  CloudFront HLS. No login required, and **no ads** — the raw manifests contain
  the recording only; ad insertion is a player-side concern we bypass.
- **Alignment.** Both platforms emit `#EXT-X-PROGRAM-DATE-TIME` on *every*
  segment, so one parser puts both on an absolute wall-clock axis.
- **Sync engine.** Master drives the clock; followers get a coarse seek when far
  out and a ±8% playback-rate trim when close. Measured follower drift:
  **34–66 ms**, including **49–66 ms across platforms** on a real Twitch/Kick
  simulcast playing the same moment in both panes.
- **Clay UI.** All three layouts (resizable split / 2×2 grid / focus+PiP) with
  the dropdown, per-pane volume + solo, stacked unified wall-clock bar *and*
  per-pane scrubbers, keyboard nudge and frame-step, bookmarks, saved sessions,
  independent chat panel. **0 Clay errors.**
- **GPU decode.** `nvdec` confirmed active on 1920×1080 H.264.
- **Channel browser.** Type a channel name, get both platforms' VODs side by
  side. Verified end to end: `--browse juliakins --pick 1` loaded a matched
  pair and synced to 66 ms.
- **Four layouts**, fullscreen, and UI text scaling — all verified on screen.
- **PiP geometry and its resize drag.** 18 checks: the 16:9 base, `sc()`
  reaching it at display scale 2, the two axes moving independently, the stage
  clamp, and the latched corner holding still through a grow and a shrink.
- **Reset, and the chat source move.** 41 checks: the content cleared and the
  preferences kept, the new `pip` and `source_fill` session lines surviving a
  save and a load, a text scale of 4.5 loading instead of reverting, the chat
  panel's new VOD field creating a chat-role source, a Kick url refused, and
  every layout laying out with 0 Clay errors. The saved channels are checked
  in memory *and* on disk, read back by a second process so that the proof
  does not rest on the same run that did the clearing.
- **The same 94 checks on Windows, on Linux, against the shipped bundle through
  `vodsync.sh`, and inside a Fedora container against the unpacked tarball.**
- **What an installed copy resolves to**, which is not something the suites
  can reach: the state folder falling back out of an ACL-denied directory, a
  bundled `yt-dlp.exe` winning over python, and the version resource reading
  back out of the binary. See "What an installed Windows copy needs".
- **Video rendering.** All three layouts verified on screen with live video
  from both platforms at once (`--layout 0|1|2`), 60 fps, 0 Clay errors.

### Why PROGRAM-DATE-TIME and not the API timestamp

yt-dlp's `timestamp` field is consistently *later* than the first segment's PDT:

| | API `timestamp` | first PDT | skew |
|---|---|---|---|
| Twitch (zackrawrr) | 2026-11-05T18:39:33Z | 18:39:30.847Z | **+2.15 s** |
| Kick (xQc) | 2026-11-04T22:14:29Z | 22:14:26.897Z | **+2.10 s** |

Using the API value would bake in a constant ~2 s error. Both platforms show
the same skew because Kick runs on Amazon IVS, which descends from Twitch's own
stack — their manifests are near-identical dialects (Kick manifests even carry
`EXT-X-TWITCH-ELAPSED-SECS` tags). One parser serves both.

**Cross-platform clocks agree.** On a juliakins simulcast the two platforms'
PDT start times for the same session were
`2026-11-04T22:25:00.145Z` (Kick) and `2026-11-04T22:25:00.144Z` (Twitch) —
**1 ms apart**, with identical 30442 s durations. Ingest-clock skew between
Twitch and Kick is not a thing you need to correct for.

**Caveat that no API can fix:** PDT records *ingest* time, not capture time. Two
streamers watching the same moment still differ by their own encoder and upload
latency, which varies by seconds and is not discoverable from metadata. PDT gets
you to within a few seconds automatically; the manual nudge closes the rest.
That is why the nudge control is mandatory, not a nicety.

---

## Solved: video rendered black (three stacked causes)

This took a long time because three independent faults produced one symptom.
Recorded in full because each is a trap worth recognising again.

### 1. raylib leaks `GL_BLEND` into mpv

mpv assumes a clean fixed-function state and does not defensively disable what
it does not use. raylib enables alpha blending by default. `glClear` **ignores**
`GL_BLEND`, so mpv's background still painted while its video quad was blended
away — which is exactly why the diagnostic below was so confusing.

The giveaway: with `VODSYNC_BG=#FF00FF` the probe reported `nonzero=224928` on a
792×729 target, and the letterbox bars are 792 × 142 × 2 = **224,928 pixels
exactly**. mpv was painting only the parts that went through `glClear`.

Fix: `prepare_gl_for_mpv()` in `vs_player.c`, called *before*
`mpv_render_context_render`. There was already a `restore_raylib_gl_state()`
afterwards — the state needs sanitising in **both** directions.

### 2. Clay emits a RECTANGLE *after* the CUSTOM command for the same element

The video element had `.backgroundColor = VS_STAGE` alongside `.custom`. Clay
emits both commands, background second:

```
#31 custom     box=(8,76 792x729)
#32 rectangle  box=(8,76 792x729)   <- painted straight over the video
```

Fix: never set `.backgroundColor` on an element carrying `.custom`. Put it on
the parent. This is the one to remember — it silently defeats *any* custom
rendering, and the symptom (pane shows exactly the background colour) looks
like "nothing drew" rather than "something drew then got covered".

### 3. Vertical flip

mpv's `FLIP_Y` and raylib's bottom-up render textures cancelled out. Fixed with
a negative source height in `DrawTexturePro`.

### Also fixed along the way

`TakeScreenshot` must be called **inside** the draw block *and* after
`rlDrawRenderBatchActive()`. After `EndDrawing` the back buffer is undefined;
before the flush, raylib has not submitted its batched geometry. Getting this
wrong produced screenshots that disagreed with what was really on screen and
sent the investigation sideways twice.

### Dead ends, so they are not retried

- **Not hwdec.** Identical black output under `no`, `auto-copy` and `nvdec`.
  `nvdec` is the right GPU path here and works fine.
- **Not the FBO handoff, sizing or the custom dispatch.** All verified directly.
- **Not mpv.** It logged zero warnings or errors at `debug` level throughout,
  and `mpv_render_context_render` never returned an error. Every one of these
  three faults was on our side of the boundary.

## Test material

**Juliakins simulcasts to both platforms** — this is the real cross-platform
overlap pair, which no other channel tried provided:

Verified matching pair (same session, both platforms):

- Kick:   `https://kick.com/juliakins/videos/01a1a8aa-6630-7e7b-b19b-34f55ffcaa9e`
- Twitch: `https://www.twitch.tv/videos/2919692356`

```bash
cd build && VODSYNC_MUTE=1 ./vodsync.exe --selftest 75 --at 7200   "https://www.twitch.tv/videos/2919692356"   "https://kick.com/juliakins/videos/01a1a8aa-6630-7e7b-b19b-34f55ffcaa9e"
```

Beware: `https://www.twitch.tv/videos/2920387816` is juliakins' *latest* Twitch
VOD but a **different session** from the latest Kick VOD — they do not overlap.
Use `builds_test_align.exe <urls...>` to print each VOD's wall-clock window
and their overlap before assuming a pair matches.

For a controlled sync test, pass the **same** URL twice — error should converge
into the ±0.08 s deadband.

Note: most arbitrary Twitch/Kick VOD pairs do **not** overlap in wall-clock
time. When they don't, the app currently parks the non-overlapping source at its
end, which is technically correct but reads as a bug. Worth a clearer warning.

---

## Memory and storage

**Nothing is ever written to disk, and memory does not grow with VOD length.**
mpv's demuxer cache is a fixed ring buffer (`cache-on-disk=no` is set
explicitly), so a 9-hour VOD costs exactly what a 10-minute one does.

Measured working set, playing continuously, sampled every 15 s:

| Panes | Settles at | Behaviour |
|---|---|---|
| 2 | ~740 MB | flat from ~60 s onwards |
| 4 | ~1.3 GB | flat from ~90 s onwards |

It climbs while the caches fill and then plateaus — the numbers above were
still flat at 120 s+ and do not drift.

The budget is `VODSYNC_CACHE_MB` per pane (default **96 MiB** forward, a third
of that backward). This was originally 192/64, which measured ~1.0 GB for two
panes and would have meant ~2 GB for four; 96 MiB still buffers roughly two
minutes of 1080p60 and halves the footprint. Raise it if you seek backwards a
lot and want the cache to cover it.

The other allocations are small by comparison: the segment timeline is 24
bytes per segment (a 9-hour VOD is ~3900 segments, under 100 KB), and the FBO
textures are sized to the pane, not the video.

**Chat is the one thing that scales with length**, because the whole log is
held in RAM at 576 bytes per message. A quiet 3-hour stream is a few MB; a very
busy 9-hour one at 200k messages would be ~115 MB. Fine in practice, but the
fixed 512-byte text field is the wasteful part if it ever matters.

## Live streams

A channel URL (`twitch.tv/<name>`, `kick.com/<name>`) loads the stream itself,
on either platform, in any combination. The channel browser shows a red
**LIVE NOW** row at the top of each column when that channel is streaming.

### Why the app writes its own playlist

A live playlist is a sliding window about 30 seconds long. Pointing mpv at it
gives two problems that make syncing impossible:

- mpv starts wherever ffmpeg decides (three segments from the end of whatever
  window *it* fetched, not the one we parsed), so position 0 maps to an
  unknown wall clock - and the whole design rests on knowing it exactly.
- Nothing is seekable. A minute in, the segments you started on are gone.

So the app keeps every segment it has seen and writes a playlist of its own
into `%TEMP%\vodsync_live_<pid>_<n>.m3u8`, which is what mpv plays. Position 0
is then *our* first segment, whose PROGRAM-DATE-TIME we parsed - the same
arithmetic a VOD gets - and the seekable window grows for as long as you
watch instead of sliding out from under you. Measured on a Twitch/Kick
simulcast: both panes held 6.2s behind their own live edge for 150s, with
60-70ms between them, which is the same accuracy the VOD path gets.

Things that had to be right, each one found the hard way:

- **Appends only.** Writing a new copy and renaming it over the old one - the
  usual way to avoid a torn read - does not work: mpv keeps playing the
  version it first read and stops at its end. Appending is also what
  `EXT-X-PLAYLIST-TYPE:EVENT` means.
- **One `WriteFile` per update.** Buffered stream writes can be split, and a
  reader landing mid-write hands ffmpeg half a segment URL.
- **`EXT-X-MAP` has to be carried through.** Some Twitch channels now stream
  fragmented MP4 rather than MPEG-TS; those fragments are undecodable without
  the initialisation segment that tag points at. Dropping it fails with
  `mov,mp4,m4a,3gp,3g2,mj2: error reading header` and a pane that never loads.
  The tag is written where it comes into force, and inherited across the
  trimmed head of the playlist.
- **`demuxer-lavf-o=live_start_index=0`**, or ffmpeg starts three segments
  from the end of our file too.
- **`protocol_whitelist=file,crypto,data,http,https,tcp,tls`**, set in the
  same option. A playlist opened from a *file* may only pull nested URLs
  over `file`, `crypto` and `data`, so every https segment in it is refused
  with `Protocol 'https' not on whitelist`. Windows' vendored libmpv did not
  enforce this and the live panes worked; libmpv 2.5 on Linux does, and they
  never load - the first thing the Linux build hit that was a real bug
  rather than a portability nit. The value has commas in it, so it uses
  mpv's length-prefixed form (`%35%`) to survive the key/value split.

### How much of a live channel is scrubbable

The window the platform hands over at resolve time - about 30 s, already
published - **is kept whole**. It used to be trimmed to the last four
segments, on the reasoning that mpv starts at segment 0 and starting half a
minute behind looks like lag. That conflated two separate questions, and
answered the wrong one: the trim set the start position by throwing away the
only history that exists when a channel opens. Nothing recovers it later.
Accumulating from there grows the recording forwards, so after a minute of
watching you had under ten seconds of scroll-back, which reads as a live
pane simply not being scrubbable.

Start position is now set where the pane opens, by seeking to the live edge
and leaving that window sitting behind the playhead. `VODSYNC_LIVE_START_SEGS`
still trims, for testing.

A live channel is therefore scrubbable across its published window plus
however long you have been watching - and no further back. That is a
platform limit, not a choice: a channel url simply does not offer more. To
go hours back in a broadcast that is still running, open its `/videos/` url,
which does.

### The clock near the edge

Near the live edge the wall clock runs on **real time**, not on the master's
decoder position (`vs_app.c`, `live_mode`). The edge advances in real time, so
that is the only clock that keeps pace with it, and it makes every pane a
follower steered by the same seek-and-trim loop - one pane buffering cannot
drag the others off the edge. Scrub back more than `VS_LIVE_NEAR` (20s) into
the recording and the master drives again, exactly as it does for a VOD.

Two margins keep the playhead inside what mpv can actually read:

- `VS_LIVE_FLOOR` (4s) below the newest published segment. Without it the
  playhead reaches the end of the playlist, mpv hits EOF - and a live playlist
  that has hit EOF does not restart on its own.
- `VS_LIVE_BOTTOM` (8s) above the start of the buffer. A seek into the opening
  moments does not take: mpv reports no error, and simply carries on where it
  was - which looks like the scrubber snapping back to live.

That bottom margin has to be measured **per pane**, not off `wall_lo`
(`vs_live_floor`). The window is a union - lowest start, highest end - which
is right for VODs covering different stretches, but the union low here is
whichever pane tuned in first. Clamping to it handed the other pane an instant
inside the few seconds of its own playlist it cannot seek into, so scrubbing
to the far left bounced back to live even though the margin existed. Taking
the highest of the per-pane floors fixed it: verified scrubbing 40s back
(clamped to the floor), both panes landing and playing on, master error
0.000s, then GO LIVE returning to the edge.

The toolbar shows **LIVE** when at the edge and **GO LIVE  -m:ss** when behind;
`G` does the same. Each pane header carries a LIVE badge, which turns to
OFFLINE when the stream ends - noticed after 25s without a new segment, after
which the floor is dropped and the recording plays out to its end.

### Chat on a live stream

The exporter works on VODs, and a channel URL is not one. Twitch records the
stream as it goes, so the app looks up that in-progress VOD in the background
and binds the chat log to it, topping it up every 30s rather than 60s. The
first export starts near the playhead (`wall - stream start - 120s`), not at
offset 0: a stream can be 30 hours old, and exporting the whole day's chat
before showing a line is not worth it. Verified live: 186 messages, cursor
tracking the playhead, anchor from the VOD's own start.

The chat lane also measures from its first message rather than from its
anchor. Measuring from the anchor stretched the whole wall-clock bar across
the 32 hours since that stream began, leaving the part being watched a few
pixels wide.

## Live VODs (watching behind a stream in progress)

### A live stream and a live VOD are not the same thing

`is_live` is **True for both**, and that one fact cost a working feature.
Adding live-channel support keyed the whole decision on it, so an
in-progress recording started going down the live path - which keeps a
window near the edge and discards the history behind it. Five hours of
perfectly seekable broadcast arrived as a few seconds you could not scrub
around in. The VOD path had handled these correctly all along; it simply
stopped being reached.

The url is the honest signal, and `vs_url_is_vod` asks it:

| url | what it is |
| --- | --- |
| `twitch.tv/juliakins` | a live channel: a ~30 s sliding window |
| `twitch.tv/videos/2921350139` | the same broadcast, in progress: one seekable manifest |
| `kick.com/misterarther` | a live channel |
| `kick.com/misterarther/videos/<id>` | the same broadcast, in progress |

A `/videos/` url is a VOD however `is_live` answers, so it gets the VOD
path: no accumulated playlist of our own, no live floor, no `live_mode` -
just a manifest that is re-read as it grows. Verified on both platforms'
in-progress broadcasts: a seek 15 minutes back from the live end landed
exactly on both panes (69 ms apart), and `growing=1` kept extending the
timeline.

The rest of this section is the machinery that path needs, because **the
manifest is a snapshot**. Resolving a VOD fetches its playlist once, so `duration` and the
timeline are frozen at that instant. Without refreshing, playback of an
in-progress stream stops at whatever the duration happened to be when you
loaded it.

`vs_refresh_begin` re-reads the manifest every 20 s for any source that looks
live and extends the timeline in place. That is a single HTTP GET — the
CloudFront manifest URL stays valid and simply grows — so it costs nothing
compared to re-running yt-dlp.

**`EXT-X-ENDLIST` is not a usable "finished" marker.** Measured on a live
juliakins simulcast: Twitch omitted it (correct), but **Kick emitted it on a
VOD that was still growing**. So "is this live?" is decided by comparing the
VOD's end against the wall clock — within 5 minutes of now counts as still
recording (`vs_source_is_growing`).

Verified against a live simulcast: both platforms reported `growing=1` and
their durations advanced 22241 → 22361 s over 120 s of real time, in lockstep,
while staying 39 ms in sync. Playback also no longer stops at the end of the
timeline while any source is still growing — it holds until the next refresh
extends it.

### Live re-sync guard

Observed once, on a live source, while a first-time full chat export was
competing for CPU and network: the master's position jumped forward ~36 minutes
in a single tick. Both panes stayed in sync with each other, so the whole
session simply moved. It did not reproduce on a warm run, and the likely
mechanism is mpv re-syncing toward the live edge of a growing playlist after
the demuxer fell behind.

`vs_tick` now rejects a master jump over 60 s that we did not ask for (no
recent seek) on a growing source, and pulls the player back to the expected
position instead of letting the jump drag the session. The root cause is
**not** confirmed, so this is a guard rather than a fix.

**Backwards needs a far tighter bound than forwards**, because playback
never runs backwards on its own: any backwards jump we did not ask for is
mpv having reloaded or restarted the file, with its position momentarily
reading near zero. A symmetric 60 s threshold let that through - measured at
31 s, so comfortably under it - and accepting it handed the session a wall
clock below the window, which `vs_recompute_window` then clamped to the
bottom. Watched from the outside: scrubbing back threw the playhead to the
start. The bound backwards is now 3 s.

### A seek undone by its own stale position

The one that made this look intermittent. `vs_seek_wall` moves the wall and
asks mpv to seek; mpv's `time-pos` still reads where playback **was** for a
frame or two. The clock then followed that stale position straight back into
the wall, undoing the seek on the very next frame.

Whether it bit depended on whether the target segment was already buffered -
which is why scrubbing back sometimes worked and sometimes snapped back,
with nothing to tell the two apart. The wall is now **held** where a seek
put it until mpv's position agrees with what was asked for, or until 2 s say
it is not going to.

Visible in `--live-back`: the first frame after the scrub reports
`pos 43.10 want 18.16` and the wall stays put; the next reports
`pos 18.97` and replay runs on from there.

## A Kick VOD is not necessarily one recording

Reported as "the Kick one would not let me seek - it would jump on Twitch but
the Kick one stayed frozen", on misterarther's 2026-11-08 VOD. It was not a
sync bug and not a seek bug. That recording is in **two pieces**.

Kick records to MPEG-TS, and when a streamer's feed drops, the playlist gets
an `#EXT-X-DISCONTINUITY` (plus Kick's own `#EXT-X-TWITCH-DISCONTINUITY` -
Kick runs on Amazon IVS, which is Twitch's own stack, hence the tag names and
the `EXT-X-TWITCH-TOTAL-SECS` header). On that VOD the cut is 1513.084s in,
and segment 151 is simply missing. **After the cut the stream's timestamps
restart at zero**, and ffmpeg's HLS reader does not rebase them.

What that looks like from the app, measured one seek at a time:

| seek to | what happens |
|---|---|
| 600, 1400 | lands, plays on normally |
| 1510 (inside the last segment before the cut) | lands, then falls back to **position 2** - playback crossed the cut and the timeline restarted under it |
| 1520, 2000, 10000 | the command returns **ok**, `time-pos` reports the target exactly, `demuxer-cache-time` is unavailable, and no frame ever arrives |

So the pane is not refusing the seek; it is waiting for a timestamp nothing in
the second timeline carries. `seek_refused` never fires, because mpv did
accept it. On a 5h47m VOD that is 5h22m frozen.

### What does not fix it

Measured, not reasoned about - every one of these still freezes at 1520:

- `hr-seek=no` (a keyframe seek rather than an exact one);
- `correct-pts=no`;
- `demuxer-lavf-o=fflags=+genpts`;
- rewriting the playlist locally as `#EXT-X-PLAYLIST-TYPE:VOD` instead of
  `EVENT` - so the EVENT tag is not the problem;
- rewriting it with the discontinuity tag **removed**, which only takes away
  ffmpeg's warning that the timeline breaks.

The shape of the playlist is not the issue. The timestamps are.

### What does

Split the playlist at every cut and write one playlist per continuous
section. A section on its own seeks perfectly anywhere, because it is then
the only timeline ffmpeg has - the tail of that VOD, as its own playlist,
reports `duration 19332.701`, which is exactly `20845.785 - 1513.084`, and
seeks to 7, 8487 and 19000 all land and play on. Then hand mpv an **ffconcat**
of the sections, because ffmpeg's concat demuxer *does* rebase timestamps
across its inputs.

```
ffconcat version 1.0
file s0.m3u8
duration 1513.084
file s1.m3u8
duration 19332.701
```

The `duration` lines are load-bearing. Without them concat reports the file
as 48 seconds long and seeking is nonsense. With them, `duration` reads
20845.785 and a seek to 10000 lands at 10000.251.

mpv's own EDL (`edl://s0.m3u8,start=0,length=...;`) does the same job and was
tried first, since it needs no `safe=0`. It was measured landing **56s out**
on the far side of a seam (asked 1520, got 1576) where ffconcat was exact, so
ffconcat won. Seeks either side of the seam - 1400, 1500, 1510, 1513, 1514,
1600 - all land within a second and play on.

`safe=0` is needed because concat treats paths in its input list as unsafe by
default, and `protocol_whitelist` because a playlist opened from a *file* may
otherwise only pull nested URLs over file, crypto and data - the same wall the
live path hits. Both are set only for a source whose manifest is an
`.ffconcat`, which is one this app wrote in its own temp directory.

### The gap is the part that matters for sync

Section 0 ends at 01:27:30.685 and section 1 begins at 01:27:39.985: **9.3
seconds of wall clock that no frame covers**, and the concatenated timeline
does not contain it. The old `wall - pdt_start` arithmetic would therefore
read every instant after the cut 9.3s early and hold the Kick pane
permanently out of step with its Twitch counterpart - which is the one error
this app exists to prevent. Landing a seek a second late is forgivable; that
is not.

So `vs_pos_for_wall` and `vs_wall_for_pos` map through the **segment table**
when a source has sections, binary-searching the real (position,
`PROGRAM-DATE-TIME`) pair the parser already recorded for every segment. A
wall clock that falls inside the gap lands on the last frame before it, since
that is the nearest frame that exists. `nsect <= 1` keeps the plain
subtraction, so every Twitch VOD, every live pane and most Kick VODs take
exactly the path they always did.

`vs_recompute_window` now asks for the same two instants **by position**
(`vs_wall_for_pos(s, 0)` and `vs_wall_for_pos(s, s->duration)`) rather than
spelling the arithmetic out again, which is what stops the scrubber being 9.3s
short at the top of a sectioned VOD.

### How common this is

A survey of misterarther's six most recent VODs on each platform:

- **Kick: 2 of 6** have a discontinuity (one cut 1513s into a 5h47m VOD; three
  in a 7-second stub). All MPEG-TS.
- **Twitch: 0 of 6.** All fMP4 with an `EXT-X-MAP`, all one continuous
  timeline. Twitch re-muxes a VOD; Kick leaves the recording as it happened.

So this is a Kick hazard rather than a general one, and it is not rare.

### Parsing trap

`#EXT-X-DISCONTINUITY` must be matched as a **whole line**, not as a prefix.
The header's `#EXT-X-DISCONTINUITY-SEQUENCE:0` opens with the same 20
characters, and taking that for a cut marks segment 0 as the start of a new
section and swallows the real cut further down. Cost: one wrong survey that
reported every VOD as having a discontinuity before segment 0.

### Still growing

A VOD being recorded can acquire a cut while it is being watched, and a
sectioned one gets longer. concat reads its input list **once, when it
opens**, so a rewritten set of section files is invisible to it - and seeking
into a tail the app knows about and mpv does not is the same freeze again. So
a sectioned VOD that grows has its files rewritten and sets `sect_reload`,
and the pane re-opens them at the instant being watched via
`vs_player_reload`. Not verified against a real in-progress Kick VOD with a
dropped feed, which is a thing that has to happen rather than be arranged.

## Swapping the split view's two panels

`X`, or the **Swap** button that appears beside **Stacked/Columns** when the
layout is Split. Left/right in columns, top/bottom when stacked - one bool,
read two ways, and the toast says which it was.

It is a property of the **view**, stored and restored with the session beside
`split_vertical` and `split_ratio`, and the sources themselves do not move:
the master, the focused pane, the per-pane nudges and the chat binding are all
untouched, and `1`-`4` still select the same streams they did. Only which
panel each slot is drawn in changes.

The divider ratio stays with the visual slot, so the panels exchange places
and keep the geometry they are placed in, the way swapping panes in a tiling
window manager does - drag the divider to 70/30, swap, and the stream that was
small is now large.

Verified without needing any video, because the pane boxes in the `--selftest`
layout readout are keyed by **source slot**: `pane0` is reported at x=8 w=792
normally and at x=808 after `--swap`, which is exactly one 8px divider past
the end of the first panel, and at y=52 / y=464 with `--vsplit`. Six checks.

Noticed while writing that test, and left alone: at ratio 0.5 the left panel
is **8px wider** than the right, because `SplitA` is a `CLAY_SIZING_PERCENT`
of the whole stage while `SplitB` takes what is left and the divider's 8px
comes out of `SplitB`. Cosmetic, older than this feature, and not worth
disturbing the divider-drag tests for.

## Chat export ("update chat")

A finished VOD is exported once; one still being recorded is topped up every
60 s, resuming from the last offset reached so each update only fetches the new
stretch. Controls live in the chat panel: **Update now**, an **Auto** toggle,
and the offset reached so far. `U` triggers an update from the keyboard.

The chat binds automatically to a loaded Twitch VOD, preferring a chat-only
source (a Twitch VOD added with `VS_ROLE_CHAT` resolves its timeline for the
anchor but never opens a player) over a video one. That is what makes chat
replay work for a muted or video-less Twitch VOD.

### The app does not scrape Twitch

`tools/vs_chat_sync.py` drives an external exporter that you install. Twitch
publishes no documented VOD-chat endpoint, so rather than bake a scraper for an
undocumented one into this project, the fragile part stays in a tool that can
be swapped or updated independently:

1. **TwitchDownloaderCLI** (recommended) — on PATH, or `VODSYNC_CHAT_TOOL`,
   or simply vendored at `third_party/twitchdownloader/`, which the script now
   looks for itself. That last fallback is why running the exe no longer
   reports "no chat exporter installed" with nothing configured.
2. **chat-downloader** — `pip install chat-downloader`.

With neither installed the UI reports "no chat exporter installed" and the
script exits 2 with instructions.

### The chat panel going blank

Reported as: chat not rendering at certain sizes, with a text-size change
bringing it back.

**Not reproduced.** Chat renders correctly across text scales 0.8 - 2.0 and
window heights 460 - 950, measured by counting lit pixels in the chat column,
and forcing Clay's measure-text cache down to 1024 words raised no error.

What was found instead is a mechanism that produces exactly this symptom. The
chat list was a scroll container using `Clay_GetScrollOffset()`, and
`Clay_UpdateScrollContainers` had **drag scrolling enabled** — so a few pixels
of pointer movement while clicking a control inside the panel scrolls it. The
offset is absolute while the content height changes with every new message,
so a stale value can park the viewport past the end and the panel looks empty.
Changing the text size re-measures everything and the clamp brings it back,
which matches the reported workaround.

Two changes remove the class of failure rather than one trigger:

- The list is **pinned to its newest message** every frame, computed from
  `Clay_GetScrollContainerData`, instead of carrying a free offset. Chat is
  tied to the playhead, so scrubbing is how you move through it.
- **Drag scrolling is off** app-wide; the wheel still works.

Clay's limits were also raised (elements 8192 to 16384, measure-text cache
16384 to 131072 words) and a `TEXT_MEASUREMENT_CAPACITY_EXCEEDED` error now
drops the cache on the next frame instead of leaving text unmeasured. Clay
errors raise a toast too — they went only to stderr before, which nobody sees
when the app is launched from Explorer.

If it recurs, that toast is the thing to look for.

### Verified against the real exporter

TwitchDownloaderCLI 1.56.5 is vendored in `third_party/twitchdownloader/`
(gitignored; re-download from the releases page if missing).

Its JSON matches what `extract()` expects exactly:
`comments[].content_offset_seconds`, `commenter.display_name`,
`message.body`, `message.user_color`, plus `video.created_at`.

Measured on a finished VOD, real chat:

| Step | Result |
|---|---|
| Export from 25000 s | 712 messages in **6 s** |
| Top up from the offset reached | **0 new** (dedupe holds) |
| Overlapping earlier range (24000 s) | +95, only the genuinely new rows |
| Ordering / duplicate rows | sorted, **0 duplicates** |

End to end in the app against the live stream: **4029 real messages**, the log
keeping pace with the live edge, both panes 72 ms in sync. A warm re-run did
incremental top-ups only (4029 → 4056 in seconds).

The `--stub` mode is still there for testing the merge logic with no network.

**Bug this caught:** anchor precedence was wrong. The script preferred
Twitch's `video.created_at` over an anchor already in the log, so every top-up
after the first overwrote the accurate PDT anchor with a value **2.9 s late**
(22:25:00.144Z vs 22:25:03Z) — enough to visibly desync chat. Correct order is
now `--anchor` → existing `#start` → `created_at`.

## Unlisted, hidden and sub-only VODs

Short version: **the browser cannot list them, a direct URL usually works, and
restricted ones need your own session.**

- **Twitch has no "unlisted" setting.** Its states are public, subscriber-only
  (published but access-controlled), and deleted. A VOD missing from the
  channel page is normally expired or deleted rather than hidden.
- **Kick does have hidden/unlisted and subscriber-only states**, set per VOD in
  the Streamer Dashboard. A hidden VOD stays reachable by its direct URL.
- **The browser lists only what the channel page exposes**, so by definition it
  cannot show a hidden VOD. Paste the direct URL instead — the `URL` toggle in
  each empty pane takes any `twitch.tv/videos/<id>` or
  `kick.com/<channel>/videos/<uuid>` and resolves it the same way.
- **Access-controlled VODs need authentication.** Set
  `VODSYNC_YTDLP_COOKIES=firefox` (or `chrome`, `edge`, ...) and yt-dlp will
  use your own logged-in session, which reaches VODs your account is entitled
  to. This is not a paywall bypass: without an entitlement the platform still
  refuses, and nothing here tries to work around that.

**Not tested**, since there was no hidden or sub-only VOD to hand: the cookie
path is wired through resolution, listing and timestamp backfill, but unproven.

A related gap this turned up, now fixed: the Twitch listing used
`filter=archives`, which **omits highlights and uploads**. For juliakins that
was 29 VODs listed out of 40 actually available. It now uses `filter=all`.

## The app mark

A rounded badge holding two play triangles — Twitch purple and Kick green —
over a shared timeline with a playhead. Two streams on one clock. It is
generated in code (`src/vs_logo.c`), so there is no binary asset in the repo
and it stays sharp at any size. Used for the window icon and in the toolbar.

**Deliberately not the Twitch or Kick logos.** Both brands' guidelines
prohibit incorporating their marks into another product's logo, and it would
imply an endorsement that does not exist. The two brand colours carry the same
meaning, and the app already uses them for the platform dots and timeline lanes.

### Two different icons

`SetWindowIcons` sets the **window and taskbar** icon at runtime. It does
nothing for the icon Explorer shows on `vodsync.exe`, which has to be a
resource compiled into the binary — that is `src/vodsync.rc`, the `rc.exe`
step in `build_resource()`, and `src/vodsync.ico`.

The .ico is generated from the app's own drawing code, so there is still no
hand-made asset:

```bash
python tools/build.py icon
```

Two passes, because the mark lives in the binary: build, have the exe export
PNGs at 16/24/32/48/64/128/256 (`--export-logo-set`, which runs before
`InitWindow` and so opens no window, since the CPU path needs no GL), pack
them with `tools/make_icon.py`, rebuild with the resource attached. Ordinary
`build.py app` runs pick the .ico up if it is there and quietly skip the
resource if it is not.

`tools/make_icon.py` writes the container by hand — a 6-byte header plus a
16-byte record per image, with PNG payloads — so there is no imaging
dependency.

Windows caches shell icons aggressively. If Explorer still shows the old one
for a path it has already seen, `ie4uinit.exe -show` refreshes it.

### Why the icon is rasterised on the CPU

There are two renderers for the same geometry. The toolbar draws with raylib
inside the normal frame. The window icon is built pixel by pixel instead
(signed-distance tests for the rounded rect, barycentric tests for the
triangles, supersampled 4x then resized).

That is not stylistic. Generating the icon through a `RenderTexture` at
start-up left the GPU in a state where **the second mpv instance could not
bring up its decoder** — `hwdec-current` came back empty and that pane stayed
black while the first pane was fine. Restoring GL state afterwards
(framebuffer, texture, shader, VAO, viewport) did **not** fix it, so the cause
is below the state we can reach. Bisected with `VODSYNC_NO_ICON`, which is
still there:

| | pane 0 | pane 1 |
|---|---|---|
| with render-texture icon | `hwdec=nvdec`, renders | `hwdec=-`, black |
| `VODSYNC_NO_ICON=1` | `hwdec=nvdec`, renders | `hwdec=nvdec`, renders |
| CPU-rasterised icon | `hwdec=nvdec`, renders | `hwdec=nvdec`, renders |

So: **do not create render textures before the mpv contexts exist.** If the
icon ever needs changing, change the CPU path, and keep the two in step
through the `LG_*` geometry constants they share.

(One later PiP run still showed an empty second FBO, but it did not reproduce
over three consecutive runs and `hwdec` was healthy — treated as a transient,
not the same fault.)

## Display scaling (4K and other high-DPI screens)

### Two ceilings, and why the slider lied

Three separate clamps bound text size, and on a high-DPI screen the wrong
ones bit first:

- `vs_ui_set_text_scale` bounds the text scale itself (was 2.0, now 3.0);
- `apply_scale` bounds text scale TIMES display scale (was 4.0, now 6.0);
- the atlas bounds the pixel size per font (was 64, now 96).

At 250% the middle one dominated: a text scale of 2.0 asks for 5.0 and was
cut to 4.0, so the top of the slider did nothing and 2x delivered 1.6x.
Body text at scale 4.0 is 16 x 4 = 64px, which hit the atlas clamp at the
same moment, clamping it twice over. Measured after raising both: scale 2.0
gives 80/65/55px and scale 2.5 gives 96/78/66, against 64/52/44 before.

The effective ceiling at 250% is now reached at a text scale of about 2.4,
where `apply_scale` caps at 6.0.

### Solved: the toolbar set a minimum width for the whole window

The chat column vanishing at high scale was never the chat panel's doing.
The toolbar is a single row of controls, none of which wraps or shrinks.
Narrow mode already drops its labels, and that is not enough: the buttons
that remain need about 626 unscaled pixels, which at 250% display scale and
a text scale of 2.0 is 3130 - wider than the window. That became a minimum
width for the window, the layout grew past the right edge instead of
fitting, and what sits on that edge is the chat column. The toolbar itself
looked perfectly normal, no Clay error was raised, and resizing appeared to
fix it because it changed which part of an oversized layout was visible.

Found by running with `--theater`, which takes the toolbar out of the
column: the body went from 3130 straight back to the window width and the
chat reappeared. That isolated it in one run after a lot of guessing at the
chat panel and the panes, neither of which was at fault.

The fix is `.clip = { .horizontal = true }` on the toolbar, with Clay's own
scroll offset, so the row cannot dictate the layout and scrolls when it is
too wide rather than losing its right-hand end for good.

Two earlier attempts that did NOT work, recorded so they are not retried:
clipping the pane header and footer (their minimum was not the binding one)
and capping the chat column's declared width (Clay was already honouring
that cap; the overflow was upstream).

Verified after the fix, with a real source and a chat log loaded, body
equals the window width in every case and the chat stays on screen:

| display | window | text scale | atlas px | result |
| --- | --- | --- | --- | --- |
| 250% | 2400 | 1.0 / 2.0 / 3.0 | 40/33/28 - 96/78/66 | fits |
| 250% | 3760 | 1.0 / 2.0 / 3.0 | 40/33/28 - 96/78/66 | fits |
| 100% | 1600 | 1.0 / 2.0 / 3.0 | 16/13/11 - 48/39/33 | fits |

## Window, layouts and text size

**Fullscreen** is `F11` or the toolbar button — both go borderless, via
`ToggleBorderlessWindowed` rather than `ToggleFullscreen`, so the monitor's
resolution is never changed. Verified: 1600x950 windowed becomes 2560x1440,
matching the monitor exactly. Clay re-reads the screen size every frame, so the
layout just reflows.

**Layouts** are split (draggable divider), 2x2 grid, focus + picture-in-picture,
**2 + 1** (two across the top, one along the bottom, using slots 1-3),
and **solo + chat** — one pane with the chat column forced visible even when no
log is loaded, since that is the point of choosing it. `L` cycles, or pick from
the toolbar dropdown.

**Double-clicking a pane** in any layout solos it with chat; double-clicking
again restores the layout that was in use before (`prev_layout`). Detection is
a 0.35 s window on the same pane, and it only fires when no pane control
consumed the click, since those return first.

**The PiP is movable and resizable on both axes.** Its position is stored as
a 0-1 fraction of the stage (`pip_x`, `pip_y`) so it stays put when the window
resizes. Its size is `pip_sx` and `pip_sy`, independent so the box can be
reshaped to the aspect of what is in it; the grab bar shows both. Drag the bar
at its top to move it and the corner grip to resize. Moving uses the grab bar
rather than the pane body because the pane already has its own controls.

The base pane is `VS_PIP_W` x `VS_PIP_H` = 300 x 169, **16:9**, and goes
through `sc()` like everything else. Both of those were wrong until recently
and are worth keeping straight — see "The PiP was the one thing that ignored
the display scale" below.

**The toolbar shows local time and UTC.** VOD timelines come from
PROGRAM-DATE-TIME, which is UTC, so showing both makes it clear what the
playhead is being compared against. The date shown is the local one.

**Text size** is `A-` / `A+` in the toolbar, or `Ctrl` with `+` / `-` / `0`,
over a **70%-500%** range. It is saved in the session file.

The ceiling has moved twice, and it lives in **two** places that have to
agree: the clamp in `vs_ui_set_text_scale` and the validator in
`vs_session.c`. A value the loader rejects is silently dropped back to the
default, so raising only the clamp makes a large text size look as though it
never persisted. The reason for 500%: `text_scale` is multiplied by the
display scale, and a TV over HDMI commonly reports 1.0, so the product that
`apply_scale` bounds is never reached and this clamp is the whole limit.
300% of a 16 px body font is 48 px, which is small at three metres.

Raising it exposed a second thing. The overlay panels — help, the chat
prompts, the layout menu, the browser's field — were **fixed pixel widths**,
so the text inside them grew with the text size and the panel did not; at the
top of the range the 430 px help box was narrower than one of its own lines.
They now go through `overlay_w()`, which scales them and then caps them
against the window, because a Clay `FIXED` element cannot shrink and an
overlay wider than the window would be pushed off the edge — the same failure
that used to make the chat column disappear.

This does **not** stretch glyphs. All three atlases are re-rasterised at the new
pixel sizes and `Clay_ResetMeasureTextCache()` is called, because scaling a
glyph atlas is exactly what made the text mush the first time round. The chrome
that has to contain text — button and field heights, the toolbar, pane headers
and footers, the status bar, the chat column width — scales with it through
`sc()`.

**Bug this caught:** `button()` and the chat message rows passed the *constant*
`VS_SIZE_SMALL` as `fontSize` instead of `font_px_for(...)`. At 140% the atlas
was rasterised at 18 px but Clay was told to draw at 13, so every button label
came out squashed while plain labels were crisp. Any new `CLAY_TEXT` must use
`font_px_for()`, never a raw `VS_SIZE_*`.

## The chat panel: collapsing and resizing

### Width is stored unscaled

`chat_w` is in **unscaled** pixels and goes through `sc()`, exactly as the
theme's `VS_CHAT_W` did. That is what makes a dragged width survive a change
of text size or a move to a different display: store it in real pixels and
the column would be the wrong size the moment either changed. 0 means "never
dragged, use the theme default", which is also why 0 is accepted by the
session loader when every other value has to be in [120, 900].

The 45%-of-window cap in `chat_col_width` still applies on top, so dragging
it wide cannot push the layout past the window - verified by loading a
session asking for 880 at 250% scale, which came out at 720 (45% of 1600)
rather than the 2200 it asked for.

### The grab bar

A thin sibling on the column's left edge, `ChatGrip`, modelled on the split
divider: a separate element that lights up under the pointer, so the drag
does not fight the panel's own clicking and scrolling. Drag id 50.

The width is computed as `(body right edge - pointer x) / g_scale`, measured
fresh each frame rather than accumulated from a grab offset, so the edge
cannot drift away from the cursor during a long drag.

### Collapsing

Folded, the column becomes a strip one button wide (`sc(26)`) rather than
being removed from the layout. Two reasons: there has to be somewhere to
click to get it back - a panel you cannot restore is not collapsed, it is
gone - and leaving it in place means the stage does not jump a second time
when it returns. The grab bar is not drawn while folded; there is nothing to
resize.

Toggled by the `>` button in the chat header, the `<` button on the strip, or
the backslash key. `chat_w` is kept while collapsed, so expanding returns to
the width you dragged rather than the default.

### Collapsing needed a third state, because the panel is not always there

Reported from the Fedora laptop: in split mode there was no chat panel at
all, backslash said "chat shown" / "chat collapsed" anyway, and nothing
appeared. In solo it was fine.

Not a zero-sized panel - the element was never declared. `want_chat` only
asked for a chat column when there was something to put in it:

```c
chat.state == READY || editing == VS_EDIT_CHAT || chat.state == FAILED
  || chat.auto_sync || chat.vod_url[0] || sync_busy() || layout == SOLO
```

A fresh install has no chat log and no Twitch VOD loaded, so in split every
clause is false. Solo forces it on regardless, which is why moving there
appeared to fix it. Measured: split with no log gave `chat ? (0,0 0x0)`,
solo with no log gave `chat 330x807`, split *with* a log gave `330x807`.

Two faults, then. The panel could not be asked for in split at all - the
"No chat log yet, press Update now" placeholder, which is how you load a
chat, was only reachable by changing layout. And the key toggled
`chat_collapsed`, which nothing read when there was no panel, then announced
it: the toast was describing a flag, not the screen.

`chat_vis` replaces the implicit rule with a stated one - `VS_CHAT_AUTO`
(what the condition above decides), `VS_CHAT_SHOW`, `VS_CHAT_HIDE`, the last
two outranking it. Backslash cycles the three states you can actually see:
full, a strip, gone. Three rather than two so there is always a way back,
and the toast now says which of them you are in. The `>` and `<` buttons set
`SHOW` as well as the collapsed flag, or collapsing a panel that was only
present on AUTO grounds could drop it entirely on the next frame.

`AUTO` is 0, so a session saved before this loads as AUTO and nothing
changes until the key is pressed.

### What is verified, and what is not

Verified on both platforms by loading sessions and reading the layout probe:
the dragged width applies exactly (500 -> a 500px column), the cap holds
(880 -> 720), the strip is 26px and pinned to the right edge, the grab bar is
6px at scale 1.0 and 15px at 2.5, it is absent when folded, and both fields
survive a save/load round trip. The layout still fits the window at every
scale tested, with zero Clay errors.

All three `chat_vis` states are verified by loading sessions, in split mode
with and without a chat log: AUTO absent / SHOW present / HIDE absent with
nothing loaded, and AUTO present / SHOW present / HIDE absent with a log, on
both platforms, with the value round-tripping through the session file.

**The drag gesture itself is not covered.** `VODSYNC_FAKE_MOUSE_X/Y` moves
the pointer but `IsMouseButtonDown` reads real hardware, so a headless run
cannot hold a button down. The arithmetic and the handler are exercised only
by hand. The same goes for the backslash *cycle*: each state it can land in
is tested, the key press that walks between them is not.

## Searching the chat

Filtering the drawn list would be searching sixty messages: it is pinned to
the playhead, so what is on screen is a keyhole onto the log. The search runs
over the whole log and the hits replace the list.

Each hit carries the moment it was said, and clicking one seeks every pane to
it - a hit you cannot go to is trivia. `Ctrl+F` opens the field (bare `F` is
still "find VODs", so that binding had to be guarded), typing filters live
with no Enter to remember, `Escape` or the `x` clears it.

`find_rebuild` only rescans when the query changes or the log grows, keyed on
both; a five hour chat is tens of thousands of lines and this is called from
the frame loop. Matching is an ASCII-folded substring over the message and
the author, capped at `VS_FIND_MAX` 500 hits with the count still reported
honestly ("1257 matches, showing the first 500").

Verified against an independent scan of the same 6514-message log: 245 / 59 /
250 / 0 hits for four queries, `LOL` and `lol` agreeing, and 500+ flagged for
a query matching 1257. 17 of 17 cases pass on each platform, with identical
column, list and row widths on Windows and Linux.

`VODSYNC_FIND` pins a query from the environment for headless runs, and
`VODSYNC_FAKEWIN` forces rows into the *ordinary* list, which otherwise draws
nothing without a loaded video. That second switch is why the next section
exists at all.

## Clicking a name

Answers "who is this, and what have they been saying?" without losing your
place. Every name in the chat list and in the search hits is a click target
(`ChatUser` / `ChatHitUser`), and clicking one opens a floating panel of that
person's messages.

**Bounded by the playhead, deliberately.** It shows what they had said by the
moment you are watching, not everything in the log - the whole point of the
chat panel is that it is in step with the video, and a popup listing what
someone says an hour from now would be a spoiler delivered by the UI.

The list of their messages is built once per name, and again only if the log
grows; the playhead bound is applied when drawing, which costs one comparison
per message of theirs rather than a scan of the whole log every frame. Since
the indices are ascending, drawing stops at the first one past the cursor.
Capped at `VS_WHO_MAX` 300, keeping the **most recent** - when a name catches
your eye it is what they have been saying lately that you want - and the
header says so when it has dropped any.

The rows carry an explicit `CLAY_SIZING_GROW(0, max)` maximum, because the
popup's list clips and would otherwise hand its children an unbounded width.
That is the same fault as the chat list had; see the next section. Verified
rather than assumed: the 487-character message in the test log measures 39px
in the popup at scale 1.0 - three lines of 13 - and 78px at 2.0, while a
short message stays on one line.

A click is resolved against the *previous* frame's layout, so the row to
message mapping (`g_row_msg`) is recorded as the rows are declared rather
than worked out again afterwards. Theater mode counts the popup as chrome
held open, via a `who_is_open()` forward declaration - the theater code is
compiled well before the popup's state exists.

Verified against an independent count of the same log, placing the playhead
with session fixtures at anchor + 0 / 600 / 1800 / the end: 0, 1, 9 and 183
messages shown for one author with 183 in the log, identical on both
platforms, and the same answer whichever case the name is clicked in.
`VODSYNC_WHO` opens it for a name so a headless run can see it; the click
that normally opens it is exercised only by hand.

## Three bugs under the chat list

Drawing a few hundred message rows on purpose, for the first time, turned up
a stack of faults in the oldest part of the chat panel. Worth reading in
order, because each one hid the next.

### A signed char indexed an array from before its start

`Raylib_MeasureText` measured byte by byte:

```c
int index = text.chars[i] - 32;
if (fontToUse.glyphs[index].advanceX != 0) ...
```

`char` is signed on MSVC, so every continuation byte of a UTF-8 sequence -
which is what an emoji is, and Twitch chat is made of them - gave a
**negative** index into the glyph array. Nothing bounded it against
`glyphCount` at the other end either. The advance came back as whatever heap
bytes happened to sit there.

The symptom was a list measuring 95,093,184 px wide, then 4,227,750,912, then
10,641,800,192 - **different on each run of the same command**, which is what
finally named it: a layout calculation does not vary run to run, a read of
uninitialised memory does. Clay raised no error, having no reason to doubt a
number it asked for.

The draw path was always correct, because `DrawTextEx` decodes UTF-8 itself.
That is the worse way round: the text is drawn, correctly, somewhere outside
the box it was measured into. Fixed by decoding with `GetCodepointNext` and
clamping the glyph index, bounded so a slice ending mid-sequence cannot read
past itself.

### A scroll container gives its children unbounded width

With the measurement honest, the list was still 1270 px inside a 479 px
viewport - deterministically now. The probe was extended to report the first
row, which settled it: `row0 1258x20`. Twenty pixels. **One line.**

`ChatList` clips vertically, which makes it a scroll container, and Clay
measures a scroll container's children against an unbounded width. With no
limit to wrap at, `CLAY_TEXT_WRAP_WORDS` never wraps - every message laid out
as a single long line and ran off to the right, behind the column's clip.

This was never about the search. **Long chat messages have never wrapped in
this app**, which is what "the chat disappears" looked like from outside: the
short messages that are most of chat looked fine, and anything long lost its
tail. Fixed by handing the limit down - `chat_row_px`, the column's width
less the padding, as the row's `CLAY_SIZING_GROW` maximum. The rows now
measure 453x80 where they measured 1258x20.

`chat_wrappable` backs it up by inserting a space inside any run longer than
26 characters, on UTF-8 boundaries, so a pasted URL or emote spam has a seam
to break at. On its own it changed nothing - the numbers came back identical
to the byte, which is how it was clear the text was not the constraint - but
with a bounded row it is what keeps one unbreakable word from pushing the
minimum back out.

### Two fixes that were reverted

Both are recorded because each looked right and neither was:

- **Clipping the list horizontally.** Bounded nothing, and produced 48,763
  Clay errors with 500 rows in it.
- **Clipping each row horizontally.** Bounded nothing either, and a clip
  makes every row a scroll container: a few hundred of them exhausted one of
  Clay's internal arrays - 17,612 `out of bounds array access` errors in a
  two second run.

The lesson both times: `clip` stops an element *dictating* a layout, which is
what fixed the toolbar, but it does not give a child a width to work inside.
Only a bounded maximum does that.

## Reset

**`Reset` in the toolbar clears what is loaded and keeps how the app is set
up.** Sources, the chat log, bookmarks, the playhead, the master assignment
and **the saved channel list** go. Text size, layout, chat width, chat
visibility and the PiP box stay, because those describe the screen the app is
on rather than the VODs that happen to be in it — and having them reset too
would make the button annoying to use for the thing it is for.

The channel list was kept at first, on the grounds that a list of channels is
a library rather than a session. Asked directly, the answer was to clear it too,
which is the better reading of a button labelled Reset: a library you cannot
empty from the UI is not much of a library. It is the one part of a reset
that is **not** undone by declining to save afterwards — `vs_channels_clear`
rewrites the file at once, while the session only follows at the next
autosave — so it is the strongest reason the button asks twice.

The list belongs to `vs_ui.c`, which owns `g_channels` and the file it is
written to, so the whole reset is **`vs_ui_reset`**: `vs_app_reset` for the
app state, then the channel list, then the UI state that lives up there (the
chat search, the viewer popup, the browser, Clay's measure cache). That is
one function rather than a sequence at the click site so that `--reset`
drives what a click drives. It *was* a sequence at the click site, and the
test then proved less than it looked like it did — it never touched the
channel list at all, because the click handler did the clearing and the test
hook called straight past it. `vs_app_reset` returns `bool` now, so a
declined reset leaves the channels and the UI state alone too.

It is a **two-click** button. The first click arms it and the label becomes
`Sure?`; the second within four seconds does it. Reset throws away every
loaded VOD and the autosave follows about twenty seconds later, so it is
effectively permanent, and one stray click next to `Save` and `Load` is not
an acceptable way to reach that. Not a modal, because this is not worth a
modal.

**It cannot reuse `vs_app_init`.** That `memset`s the whole struct, which
would drop every mpv handle, resolver thread and segment list on the floor.
`vs_app_reset` goes through `vs_remove_source` instead, which already bundles
the teardown in the right order, then `vs_chat_free`.

**It refuses while a chat export is running.** The export worker holds a
pointer to `a->chat` and writes into it as it goes, so clearing the log here
would race a live writer — and then the finished export would reload into the
log that was just emptied. `vs_chat_sync_busy` is checked first and the reset
says why it declined. This is the one path not covered by a test: driving it
needs an export actually in flight.

Verified, with a session fixture carrying three sources, a 6,514-message chat
log, two bookmarks and non-default preferences: after a reset, sources 0,
chat-role sources 0, messages 0, chat state 0, marks 0, master -1, playhead
0; and text scale 1.80, layout 2, chat width 420, chat visibility 1 and the
PiP box 1.50x0.75 all unchanged. Also verified with nothing loaded at all,
which exits 0 with no Clay errors.

## The PiP was the one thing that ignored the display scale

Reported from a 4K TV: the PiP looks small, and the scale control only goes
to 250%. Both true, and the cap was the lesser half of it.

`VS_PIP_W` and `VS_PIP_H` were **raw pixels**, never passed through `sc()`.
Every other dimension in the app goes through it, including the PiP strip's
own grab bar at `sc(15)` and its grip at `sc(16)` — so on a 4K screen at
200% the chrome around the panes doubled and the panes did not. The 2.5x cap
was then standing in for "do not let the PiP eat the stage", which is a bound
better expressed against the stage: it now clamps to 90% of the stage box,
which means the same thing on a 1280x800 Deck and on a 4K panel, where a
multiplier of an unscaled constant never did.

The strip's own `padding` and `childGap` were raw literals too, found by a
test expectation that disagreed with the app by exactly the amount those
contribute. At display scale 2 the panes doubled and the 4 px gutters between
them stayed 4 px.

### 300 x 224 is not 16:9, and that was the black bars

`VS_PIP_H` was 224 against a width of 300 — a 1.34:1 box. So every 16:9
stream was letterboxed with bars top and bottom **in the PiP and nowhere
else**, and those bars were the pane's shape, not the video's. It is 169 now,
16:9 to within half a pixel.

### Reshaping a pane does not crop it

Two axes (`pip_sx`, `pip_sy`) let the box be reshaped, but that alone cannot
remove bars that are baked into the stream, because `vs_ui_draw_custom`
letterboxes with `fminf(bw/tw, bh/th)` — reshape the box and the bars just
change shape with it.

So panes have a per-source **`Crop`** toggle, in the pane footer, which
switches that to a cover fit: it reads the largest centred sub-rectangle of
the frame matching the pane's shape and hands it to `DrawTexturePro` as the
source rect. The overflow is never sampled, so this crops with no scissor and
no second pass. Per source, because the bars belong to the stream and not to
the layout.

`pip_x`, `pip_y`, `pip_sx`, `pip_sy` and each source's `fill` are now saved
in the session. The PiP box never was, which was tolerable while it was one
number nobody tuned. `fill` goes on its own `source_fill` line rather than
into the `source` record: that record's url is found with `%n` after exactly
five fields, so a sixth would make every older session fail to parse and lose
its sources. It is keyed by the slot it was saved in and looked up by that,
because the loader re-adds sources into the first free slot, which is not
necessarily the one they came from.

## Resizing the PiP jumped, because the origin moved with the size

The resize drag was one line:

```c
s2 = (m.x - pb.x) / VS_PIP_W;
```

measuring the scale from the strip's **live left edge**, while the grip being
dragged sits at the bottom-**right**. And `pip_x` is a fraction of the space
the strip does *not* occupy:

```c
ox = pip_x * (stage_width - strip_width)
```

So growing the strip shrank that space and moved the left edge. The origin of
the measurement was a function of the size being measured from it — a
feedback loop. `pb` was also last frame's box, so each correction landed a
frame late, which turned the loop into an oscillation rather than a quiet
error. On top of that the resize recorded no grab offset, unlike the move
drag, so the box snapped the instant the grip was taken.

The fix latches the strip's top-left in stage pixels at mousedown, sizes
against that, and then re-solves `pip_x`/`pip_y` to put the latched corner
back where it was. The box grows from the corner under the pointer and the
origin no longer moves with it. Predicting next frame's box is exact rather
than hopeful: the width is `FIXED(pw)`, and the height is the slots plus the
chrome, which is measured once at drag start (`pip_oh`) so it does not matter
what the grab bar and padding happen to add.

The arithmetic **is** the fix, so it is not tested against a copy of itself:
`pip_resize_latch` and `pip_resize_apply` take the pointer explicitly, the
mouse handlers pass the real one, and `--pip-drag x1 y1 x2 y2` passes a
synthetic one. The test reads the strip box before and after and requires the
latched corner to be unchanged — grow and shrink, both axes, 18 checks. That
is the only way to measure this without a hand on a mouse: `IsMouseButtonDown`
reads real hardware, so the gesture itself is still unverified; every state it
can reach is not.

## Sessions

### Saved as you go, not only on the way out

Writing the session at exit is enough on Windows, where closing the window
is a clean exit however you do it. It is not enough anywhere a process gets
**killed**: Steam's Game Mode stops a non-Steam shortcut with SIGTERM, and
nothing after the frame loop runs. On the Deck that read as "the app never
saves anything".

Two changes, either of which would have been enough on its own, and both
worth having:

- the session is written **every 20 seconds** while the app runs, so what is
  on screen is never more than that out of date;
- **SIGTERM, SIGINT and SIGHUP are caught** (`vs_install_quit_handler`), so
  the frame loop ends normally and the last few seconds are saved too.

Verified by running the Linux build, killing it with `kill -TERM`, and
reading back a session file containing the source URL, layout, master and
playhead.

### Where state is written

`vs_state_dir` - the app's own folder when that is writable, which keeps a
copy you unzipped somewhere self-contained, and the per-user data directory
when it is not, rather than silently failing to save. That is
`$XDG_DATA_HOME/vodsync` (or `~/.local/share/vodsync`) on Linux and
`%LOCALAPPDATA%\vodsync` on Windows. `VODSYNC_STATE_DIR` overrides both.

`--where` prints it, which is the quickest way to answer "where did my
channels go". This document used to say the path was printed at startup. It
was not printed anywhere, by either build.

**Windows ignored `VODSYNC_STATE_DIR` entirely** until the reset work, while
the posix build had honoured it from the start and this document said it
worked on both. Every test suite sets that variable, so on Windows every
suite had in fact been reading and writing beside the exe — which is
`build/vodsync.exe`, the copy in daily use. The reset suite seeds some
channels and then checks they are gone, and the first run of it emptied a
real saved channel list. Recovered from the Nov 7 copy in `build-dev/`; the
asymmetry is fixed rather than worked around in the tests, because a test
that sets a variable the app ignores is worse than no isolation at all — it
looks isolated.

`--save-channel` printed its path from `vs_exe_dir` while the writer used
`vs_state_dir`, so on Linux it could name a folder the file was not in. It
asks the same function the writer does now.

## What an installed Windows copy needs that a built one does not

Everything below was found by asking what breaks when the app stops living
in its own build folder. None of it is the installer; all of it is what the
installer would otherwise ship broken.

### The state folder had no fallback, which is the one that would have bitten

Windows answered with the exe's folder and nothing else. Install to
`Program Files` and the session file, the channel list, the autosave and the
chat logs all try to write into a directory a standard user cannot write to.
The autosave fails silently, `Save` says "save failed" with no reason, and a
channel saved during the session is gone on restart. Linux had already
solved this; Windows now does the same with `%LOCALAPPDATA%\vodsync`.

**Writability cannot be read off the directory on Windows.** `Program Files`
denies writes through an ACL, not the read-only attribute, so `_access`
reports it as writable and the check would pass where it matters least.
`dir_writable` creates a file instead - the only honest test - with
`FILE_FLAG_DELETE_ON_CLOSE` so no probe is left behind even if the process
dies between the create and the close. The result is cached for the process,
since this is asked several times a session and the probe does real I/O.

Verified against a real ACL: a copy of the exe in a folder with
`icacls /deny <user>:(WD,AD)` applied reports its state folder as
`%LOCALAPPDATA%\vodsync` while naming the denied folder as its exe folder,
and the same copy in a writable folder still reports that folder. A copy in
its own build directory - which is every copy today - is unaffected.

### yt-dlp was the literal string "python -m yt_dlp"

So an installed copy on a machine without python *and* the `yt_dlp` module
could not resolve a single VOD, and it failed the way a dead url fails, with
nothing on screen to say why. Posix has preferred a `yt-dlp` beside the exe
from the start, because the Deck has no system python. Windows now does the
same: beside the exe, then `SearchPathA` for one on `PATH`, then the python
module as a last resort so a development machine behaves exactly as before.

Verified by putting a `yt-dlp.exe` beside a copy of the exe and watching
`--where` name it in preference to python.

Chat replay still needs python either way, because `vs_chat_sync.py` and
`vs_chat_prep.py` are scripts run through `vs_python_cmd` - no amount of
bundling fixes a missing interpreter. That is a real limit of an installed
copy and `--where` says so in as many words, rather than leaving it to be
discovered when an export does nothing.

### /MT was not a one-flag change, which is why the flag was checked first

The obvious way to drop the `vcruntime140.dll` dependency is `/MD` ->
`/MT`. It would not have linked: `third_party`'s prebuilt
`raylib.lib` carries `/DEFAULTLIB:MSVCRT` (seven of them, no `LIBCMT`), so a
static-CRT exe is a `RuntimeLibrary` mismatch at link time, and there are no
raylib sources in the tree to rebuild against.

The supported alternative is app-local deployment, and it is the better
answer here anyway: `vcruntime140.dll` is copied beside the exe by the build,
the way `libmpv-2.dll` already was. 117 KB, no redistributable installer, no
admin rights - which also keeps a per-user install free of a UAC prompt. The
import table says that is the only one needed: the rest of the CRT
references are the `api-ms-win-crt-*` forwarders, which are part of Windows
10 and 11, and there is no `msvcp140.dll` at all because nothing here is C++.

### The binary carried no version

`src/vodsync.rc` held one line, `1 ICON`. Explorer's Details tab was blank,
an installer had no version to read, and a missing `VERSIONINFO` is one of
the several things SmartScreen holds against a binary. The numbers now come
from `src/vs_version.h`, which the `.rc` includes, so a release is one edit
and the installer script can read the same file. Verified by reading
`FileVersion` and the rest back out of the built exe.

## The installer

`tools/installer/vodsync.iss`, built with `python tools/build.py installer`,
which needs Inno Setup 6.3 or newer (`winget install JRSoftware.InnoSetup`
installs 6.7.3, **per-user**, under `%LOCALAPPDATA%\Programs` - which is why
it wants no UAC prompt, and why `find_iscc` looks there before either
`Program Files`; checking only those two reported a freshly installed
compiler as missing).
The target packs what is already in `build\` and compiles nothing: the script
stops with an `#error` rather than packing a folder that is stale or missing
a DLL. The version is read out of `src/vs_version.h` and passed in as
`/DAppVer`, so the installer, the binary and the header cannot disagree -
which is the kind of mismatch nobody notices until after it ships.

**Per-user, no admin.** `PrivilegesRequired=lowest`, so `{autopf}` resolves
to `%LOCALAPPDATA%\Programs` and there is no UAC prompt. The CRT is
app-local, so there is no redistributable to chain-install either, which is
the other thing that would have forced an elevation.

That choice decides where state goes, and the two interact in a way worth
writing down. `vs_state_dir` picks the exe's folder when it is writable - and
a per-user install folder **is** writable. So an installed copy keeps its
channels, session and chat logs next to `vodsync.exe`, behaving exactly like
the portable copy, and the `%LOCALAPPDATA%\vodsync` fallback is only reached
by an admin install into `Program Files`. Both work. One has fewer
differences to remember, and `--where` prints which one you got.

**Components**, because the payload is lopsided: `vodsync.exe` is 864 KB and
sits beside a 117 MB `libmpv-2.dll` and a 66 MB `TwitchDownloaderCLI.exe`.
Chat replay is a component of its own - it is most of the download, and it
needs python regardless, since the chat helpers are scripts.

**`yt-dlp.exe` is vendored**, at `third_party/yt-dlp/`, and copied into
`build\` by `build.py` the way `libmpv-2.dll` is - which is what frees both
the development build and an installed copy from needing python to play
anything. It is still conditional in the `.iss`: without it the app falls
back to `python -m yt_dlp` and python becomes a requirement for playback,
not just for chat.

Fetched by hand, like the other binaries in `third_party`, and worth doing
the same way if you update it - a binary that goes into an installer is
exactly where a checksum earns its keep:

```bash
cd third_party/yt-dlp
curl -sfL -O https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe
curl -sfL -O https://github.com/yt-dlp/yt-dlp/releases/latest/download/SHA2-256SUMS
grep -i ' yt-dlp.exe$' SHA2-256SUMS   # compare with sha256sum yt-dlp.exe
```

2026.10.26 at the time of writing, 23.6 MB, and public domain (the Unlicense,
read from the repository rather than assumed), so shipping it is fine. Note
it is `yt-dlp.exe`, the x64 build - `yt-dlp_x86.exe` is the 32-bit one and
this installer is x64-only.

### Uninstalling does not take your data with it, and nearly did

Inno removes what it installed, so the channel list, sessions and chat logs
the app wrote are left behind. The uninstaller offers to delete them, with
**No** as the default button: someone uninstalling in order to reinstall
wants them kept.

The first version of that code called `DelTree` on `{app}`. The install
directory is the user's choice on the directory page - it could be a folder
they already keep other things in - so that was a way to destroy files that
were never ours, triggered by an uninstall. It deletes named patterns in the
install folder now (`vodsync.channels`, `vodsync.autosave`, `*.session`,
`chat_*.tsv`, `*.vodsync.tsv`) and only treats `%LOCALAPPDATA%\vodsync` as
disposable in full, because that is a folder the app creates and owns.

**And then a silent uninstall deleted the lot anyway.** `MB_DEFBUTTON2`
decides which button a human sees focused. It does **not** decide the answer
`/SUPPRESSMSGBOXES` gives, which for a `MB_YESNO` box is **Yes**. So the
first real test of the uninstaller - silent, because that is how a test
automates it - reported `channels KEPT: False`. Anything that uninstalls
without a window would have wiped the saved channels and the session with
nobody having agreed to it.

`UninstallSilent()` now returns early before the question is reached: an
uninstall with nobody to ask deletes nothing. Deleting someone's data needs
consent, and a suppressed message box is not consent - it is the absence of
the question. Worth remembering as a shape, not just a bug: a safe default
expressed as a *default button* is only safe while a human is looking.

### What cannot be fixed in the script

An unsigned installer gets SmartScreen's "Windows protected your PC" on
first run, and the only real cure is an OV code-signing certificate at a few
hundred a year. The `VERSIONINFO` block helps a little and honesty helps the
rest: **More info -> Run anyway**.

### Verified

Compiled: `vodsync-0.9.0-setup.exe`, 101 MB for a 206 MB payload. Then
installed, used and uninstalled for real, into a temp folder with
`/VERYSILENT /NOICONS /DIR=...`:

- **install exits 0** and lays down exactly the nine files it should - the
  seven payload files plus Inno's own `unins000.exe` and `.dat`;
- **an installed copy needs no python to play anything**: `--where` names
  `{app}\yt-dlp.exe`. The shipped copy runs (`--version` reports
  2026.10.26) and its SHA-256 still matches the hash published for that
  release after a round trip through LZMA2 - so what is installed is what
  was downloaded, not merely something of the right size;
- `TwitchDownloaderCLI.exe` lands at `{app}` and `vs_chat_sync.py`'s own
  `find_tool` resolves it there - run, not read, because that is the whole
  reason it is not in `{app}\tools`;
- the installed exe **starts**, finds `C:/Windows/Fonts/segoeui.ttf` and
  reports 0 Clay errors, which is the payload being complete:
  `vcruntime140.dll` and `libmpv-2.dll` beside it and nothing else needed,
  no MSVC environment, no repository;
- `--where` reports the install folder as the state folder, as the per-user
  design intends;
- Windows lists it in **HKCU** - no admin anywhere - as "vodsync 0.9.0" with
  `DisplayVersion 0.9.0`, so the number reached the installer and the
  binary's `VERSIONINFO` from the one header;
- a **silent uninstall** removes every installed file and the registry
  entry, and keeps `vodsync.channels`, `vodsync.autosave`, a `*.session` and
  a deliberately planted unrelated `.txt`. The last two are the two
  near-misses above, now pinned down by a test rather than by intent.

The whole cycle was run again after `yt-dlp.exe` joined the payload, because
"it worked before the file got 23 MB bigger" is not the same claim.

Not verified: the interactive uninstall actually deleting the files when
someone clicks Yes. That needs a hand on the dialog. The untested direction
is the one that keeps data, which is the right way round for this to fail.

### Saved channels save themselves

Pressing Save was a manual step, and the button only acted on a channel that
had already been looked up - pressing it after typing a name, before
pressing Find, did nothing and said nothing. Both wrong:

- anything that **plays** now has its channel remembered, whichever way it
  arrived: the browser, a pasted URL, a restored session, the command line.
  `vs_ui_frame` watches what is loaded rather than which button was pressed;
- Save works on what is typed, and says "type a channel name first" when
  there is nothing to save;
- `--save-channel <name|url>` adds one from the command line and exits,
  which is easier than typing on a Deck in Game Mode.

The name parser was also wrong for Twitch: it looked for `.com/` and took
what followed, so pasting a twitch.tv URL saved a channel called `https:`.
`vs_channel_normalize` now strips a scheme, then a host, and keeps the first
path segment.

### The files

Two files, in the state directory:

- `vodsync.session` — the manual slot. `Ctrl+S` / `Ctrl+O`, or the Save / Load
  buttons.
- `vodsync.autosave` — written on exit, read at start **only when no VOD is
  named on the command line**, so arguments always win. Kept separate so an
  autosave can never clobber something saved on purpose, and skipped under
  `--selftest` unless `VODSYNC_AUTOSAVE_PATH` points somewhere harmless.

It restores the sources, layout, split ratio, master, focus, per-pane volume
and solo, text scale, theater, bookmarks, the chat log and **the playhead**,
so a restart resumes at the same instant.

One subtlety: resolving the first source re-anchors the playhead to that VOD's
start, which would have thrown the restored timestamp away. `wall_pinned`,
set by a session load, suppresses that.

## The font, and three ways a bundle lies about being self-contained

Reported from the Fedora laptop: the text is very hard to read. It was not a
sizing problem. Finding the font was a list of five hardcoded paths, and on
Fedora **every one of them misses** - Fedora files fonts under the package
that provided them:

```
/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf
/usr/share/fonts/google-noto/NotoSans-Regular.ttf
/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf
```

So the app fell through to `GetFontDefault()` - raylib's built-in 10 pixel
bitmap font - and said nothing about it. That also explains why making the
text bigger did not help: scaling a bitmap font just scales the blocks.
Arch and Windows were fine, which is why it took a report from a third
platform to surface.

### Ask for a font by name, not by path

`vs_find_font` in the platform layer, in three stages: beside the executable
first, then the known distro paths as a fast path, then a depth-limited walk
of the font directories looking for a priority-ordered list of filenames. A
hardcoded path list is exactly the assertion that was wrong, so it is now
only a shortcut, never the answer. Only `.ttf` is considered - stb_truetype,
which raylib parses with, cannot read the CFF outlines in Fedora's
`Cantarell-VF.otf`.

It also prints a warning when it ends up on the bitmap font. A silent
fallback to something unreadable is the actual bug here; the wrong paths
were only how it happened.

**The bundle now carries `DejaVuSans.ttf`** (and its license), because the
search can only find what the target machine has, and a minimal install may
have no fonts at all.

### SDL3 is dlopen'd, so no dependency scan can see it

Launching the bundle on Fedora for the first time: `Failed loading SDL3
library`, and `ldd` reporting nothing missing. Both true at once. The
bundled `libSDL2-2.0.so.0` is **sdl2-compat**, a shim that opens
`libSDL3.so.0` with `dlopen` - which appears in no `NEEDED` record, so
neither `ldd` nor `bundle.sh`'s closure pass can know about it.

Testing on Arch had never caught it because Arch has its own
`/usr/lib/libSDL3.so.0`, and the shim found that instead. The bundle was
never self-contained; it was borrowing.

`bundle.sh` now copies such libraries from an explicit list, before the
fixed-point pass so their own dependencies come too. Anything opened at
runtime has to be named by hand - there is nothing to scan.

### A multiarch SDK will hand you a 32-bit library

With SDL3 bundled it still failed, now with `libdecor-0.so.0 => not found`
about a file sitting in `lib/`. It was the **i386** build: one of 260. The
loader skips a wrong-architecture candidate and reports it as missing,
which reads like a path problem and is not.

The cause was in the second pass:

```sh
src=$(find /usr/lib /lib -name "$soname" -print -quit)
```

The sniper SDK is multiarch - `/usr/lib/i386-linux-gnu` sits beside
`/usr/lib/x86_64-linux-gnu` - and `-print -quit` takes whichever it reaches
first. Any library that pass added could have been the wrong one.

Replaced with `find_lib64`, which searches the 64-bit directories first and
then **verifies the ELF machine type** rather than trusting the path, and
`bundle.sh` now refuses to finish if any file in the bundle is not x86-64.
A wrong-architecture library costs nothing to detect here and is close to
undiagnosable on someone else's laptop.

### Verified on Fedora, not inferred

The bundle is now unpacked and launched on a real Fedora, headless under
Xvfb with software GL, which is what turned all three of the above from
guesses into failures with addresses. Three font cases there:

| as shipped          | its own `dist/DejaVuSans.ttf`                     |
| bundled font gone   | `/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf` |
| no fonts at all     | the warning, then the bitmap font, exit 0         |

Zero layout errors in each. The whole suite - 17 layout and search cases, 6
visibility, 6 popup - also passes against the shipped bundle itself, run
through `vodsync.sh` on Arch, rather than only against a locally built
binary.

Two things a container cannot answer and that remain open: hardware decode
on real hardware, and audio output.

`libXi`, `libXtst` and `libXcursor` stay host-provided, like the other X
client libraries - every real desktop has them, and the Fedora install note
lists them. The minimal Arch WSL image did not, which is worth knowing
before reading a failure there as a bundle problem.

## Linux / Steam Deck build

A second build, deliberately separate from the Windows one, which is the
copy in daily use: `python3 tools/build_linux.py` -> `build-linux/vodsync`.
Both builds share every source file except one.

### The platform layer

The Windows-only code was never more than four concerns, so they now live
behind `vs_platform.h`, implemented twice:

| | Windows | POSIX |
|---|---|---|
| HTTP GET | WinHTTP | libcurl |
| run yt-dlp, read its output | `CreateProcess` + pipe | `posix_spawn` + pipe |
| background jobs | `CreateThread`, `Interlocked*` | pthreads, `__atomic_*` |
| install folder, temp dir | `GetModuleFileName`, `GetTempPath` | `/proc/self/exe`, `$XDG_RUNTIME_DIR` |
| append to the live playlist | `WriteFile` | one `write(2)` |
| clocks | `_time64`, `_gmtime64_s` | `time`, `gmtime_r` |

`vs_platform_win32.c` is the existing code **moved, not rewritten** - the
Windows build is in use and there was nothing to gain by disturbing it. The
rest of the app (the UI, the layout, the sync engine, Clay, raylib, libmpv)
was already portable and is untouched.

Two differences are real rather than mechanical:

- **No shell.** The POSIX side splits the command line itself, honouring the
  double quotes the app already writes, and execs directly. `popen` would
  have been shorter but would hand a URL containing `;` to `/bin/sh`.
- **yt-dlp and python are named per platform** (`vs_ytdlp_cmd`,
  `vs_python_cmd`). On Windows that is `python -m yt_dlp`; elsewhere a
  `yt-dlp` binary beside the executable if there is one, otherwise one on
  PATH. The Deck has no system Python, so bundling the standalone binary is
  how the app stays self-contained there. `VODSYNC_YTDLP` and
  `VODSYNC_PYTHON` override both.

### Deck specifics

- **Decode.** `VS_HWDEC` is `vaapi,vaapi-copy,auto-copy` on Linux - RDNA2
  decodes through VAAPI. The zero-copy path wants an EGL context; whether
  GLFW gives us EGL or GLX is not ours to decide, so the copy path is next
  in the list rather than left to chance.
- **GL entry points** are resolved by name at runtime
  (`glXGetProcAddressARB`, then `eglGetProcAddress`, then `dlsym`), so the
  same binary works under either.
- **Fonts**: Arch paths first, since SteamOS is Arch. `VODSYNC_FONT`
  overrides.
- **SteamOS has a read-only root that is replaced on update**, so build in a
  container and keep the result in `~/`; `build_linux.py desktop` writes a
  `.desktop` file, which is also what Steam needs to add it in Game Mode.
- The screen is 1280x800: split and solo+chat fit, the 2x2 grid is tight.
  Docked to a 4K TV is where the display scaling above earns its keep.

### Status: it builds and runs

Verified on Arch in WSL2 (clang 23.1, raylib 6.2.0, libmpv 2.5.0, libcurl
8.23) - Arch because SteamOS 3 is Arch. See tools/deck-container/README.md
for the exact commands.

- `vs_test_align` resolves a Twitch VOD through the whole platform layer -
  `posix_spawn` to yt-dlp, libcurl for the manifest, the PDT parser - and
  prints **the same numbers as the Windows build**: 1080p, 4508 segments,
  api skew +2.05s, the same window to the millisecond.
- The app itself opens a window through GLFW/X11, loads the VOD in libmpv,
  decodes (`hwdec=vaapi-copy`), renders frames into the raylib FBO, lays out
  the UI with the real font, and reports 0 Clay errors at 44 fps.

The whole first round of compile errors was four things, which says the
shape of the port was right:

| | |
|---|---|
| `_stricmp` in four places | now `vs_stricmp` in the platform layer |
| `tolower` without `<ctype.h>` | a Windows header had been providing it |
| `Sleep(100)` in the headless test | now `vs_sleep_ms` |
| `_GNU_SOURCE` defined twice | the build passes it; the source now guards |

`vs_platform_posix.c` itself compiled first time, which it would not have
done without the desk-check beforehand - that pass found a call to a
`curl_global_init_default` that does not exist, an error message reading
`argv[0]` after freeing it, and an `__atomic_load_n` through a `const`
pointer. Reading code catches some things. It also missed the chat log
being written one directory too high after the `vs_exe_dir` switch, which
only turned up by grepping for backslash paths - the compiler cannot see
that one either.

### Chat export on Linux

Works. TwitchDownloaderCLI 1.56.5 `Linux-x64` runs as-is: 18 messages
exported in 2.2s through `tools/vs_chat_sync.py`, producing the same TSV the
Windows build writes, anchor header and all. The app drives it end to end on
Linux too - `vs_proc_capture_code` spawning `python3`, the script spawning
the exporter, the "OK ..." line parsed back - verified against a live
Twitch stream.

Both builds of the exporter can live in `third_party/twitchdownloader/`
side by side, since the Windows one keeps its `.exe`. `find_tool` now asks
for the right one first and checks the executable bit, rather than taking
whichever name it finds - on Linux it was picking up the `.exe`.

### The shippable build (sniper)

Built in Valve's Steam Runtime 3 "sniper" SDK, unpacked as a plain chroot -
no Docker needed, `tools/deck-container/README.md` has the commands. gcc
10.3, glibc 2.31, and its apt repo does carry `libmpv-dev` (0.32), which was
the one dependency that might not have been there.

The binary's floor comes out at **GLIBC_2.17**, so the C side will start on
any Linux from the last decade. Everything else is about what travels with
it, which `tools/deck-container/bundle.sh` works out:

- **libmpv is not in the runtime that ships to users.** Valve publishes an
  mtree manifest of the Platform image; it has no libmpv at all. So libmpv
  and the parts of its dependency chain the target lacks come along - 257
  libraries, 307 MB, mostly ffmpeg and the samba stack libmpv's SMB support
  drags in.
- **Drivers must not travel**: libGL, libva, libdrm, the X11 and Wayland
  libraries all have to be the host's, or hardware decode and the display
  break. Those are an explicit exclusion list.
- The default bundle is **self-contained**, so the folder runs on SteamOS as
  it is. `VODSYNC_RUNTIME_PROVIDED=1` instead leaves out whatever Valve's
  runtime ships, which is far smaller but only works when Steam launches it
  inside the sniper runtime - which a non-Steam shortcut does not do.

Expect noise on stderr that is not a fault: ALSA complaining about a
missing sound card in a container, and fontconfig warnings when the
bundled (older) fontconfig parses a newer host configuration. The app
loads its own font by absolute path and does not depend on either.

**Verified**: the bundle was run on Arch - a distro generations newer than
the one it was built against - and played a Twitch VOD, decoding through its
own bundled libmpv, 0 Clay errors.

Four things this cost, each worth knowing:

- **`-Wl,--disable-new-dtags`.** The modern default emits DT_RUNPATH, which
  applies only to an object's own dependencies: the bundled libmpv then
  looked for its ffmpeg libraries without it and found nothing. DT_RPATH is
  inherited down the chain. The launcher also sets `LD_LIBRARY_PATH`.
- **Anchor the skip list.** Unanchored, `libm` matches libmpv and `libc`
  matches libcurl, so the one library that had to ship was the one left out.
- **Walk NEEDED records, not ldd.** Inside the SDK everything resolves -
  libmpv finds samba's private libraries through an RPATH of its own - so
  "what is missing" only becomes visible on the machine you copy to. What
  travels has to be read from the dependency records.
- **libnsl is not part of libc any more.** glibc split it out; excluding it
  as libc-adjacent left a bundle that would not start on a host without it.

### "No chat exporter installed" on the Deck

It was installed. `tool_path` only looked in `<exe>/../tools/`, the
development layout where the binary sits in `build/`. In a shipped bundle
the scripts are in `tools/` *beside* the binary, so the app handed python3 a
path that did not exist.

What made it hard to read rather than merely wrong: **python exits 2 for a
script it cannot open, and 2 is also the exporter's "not installed" code**,
so a missing helper was reported as a missing exporter - the wrong problem,
in the one place someone would act on it. Two fixes, both worth having:
`tool_path` now tries the shipped layout first and the development one
second, and a script that is genuinely absent says `chat helper missing:
<path>` instead of blaming the exporter.

Verified from the bundle afterwards: 6514 messages exported, the same count
the Windows build gets from the same VOD.

For a copy already on a Deck, `cp -r ~/vodsync/tools ~/` satisfies the old
path without re-copying 178 MB.

### Still untested

- **Real Deck hardware.** WSLg is not RDNA2: the zero-copy `vaapi` path, the
  1280x800 layout, Game Mode and the trackpads are all unverified.
- **Hardware decode on the Deck.** The sniper bundle reported `hwdec=no`
  under WSLg, which proves nothing either way: there is no RDNA2 and no
  radeonsi there. libva is deliberately left to the host, which is the right
  arrangement for VAAPI, but it is unverified on the real machine.
- Fonts: the Arch container needed `ttf-dejavu`. With no font at all the app
  falls back to raylib's built-in one and keeps going, which is how the
  first run looked.

## Theater mode

The `[ ] Theater` checkbox in the toolbar, or `T`. The chrome is **not dimmed
or faded** — it is simply not declared that frame, so the video takes the space
back. Each piece returns when the pointer is near where it lives:

| Chrome | Reveals when |
|---|---|
| Toolbar | pointer in the top ~78 px |
| Timelines + status bar | pointer in the bottom ~180 px |
| A pane's header / footer | pointer within that pane's x range and near its own top / bottom edge |
| PiP grab bar + corner grip | pointer over or just outside the PiP strip |

Pane chrome keys off the pane's **own** edges rather than merely being over
the pane, which is how a video player behaves; otherwise the bars would be
visible almost all the time. The chat column stays, since chat is content
rather than chrome. It works in every layout, PiP included, and is saved in
the session file.

Reveal depends on the real pointer, which a screenshot run cannot place, so
`VODSYNC_FAKE_MOUSE_Y` overrides the Y coordinate for testing rather than
warping the user's cursor.

**Revealed chrome floats; it never displaces the stage.** Originally the
toolbar and the timeline were simply added back into the root column, so
showing them re-laid out everything below: the stage shrank, and whatever
the pointer was reaching for moved out from under it. Near the edge of a
reveal band that also moved the pointer back out of the band, so the bar
flickered. In theater mode the stage now always has the whole window and the
chrome is drawn over it, with its own backing (`VS_SCRIM`). The same applies
to a pane's own header and footer, which float over the picture rather than
shrinking it. Measured: `body`, `pane0` and `video0` boxes are identical
with the chrome up, down, top-only and bottom-only.

Reveals also **linger** for 0.6s after the pointer leaves, and stay up while
the pointer is actually on them, so the edge of a band is not a knife edge.

**The PiP answers for itself.** Parked in its default corner it sits inside
the bottom reveal band, so reaching for it used to raise the timeline and
the transport too. Anything that reveals on proximity now asks
`pointer_over_pip` first - including the "pointer is on the bar" latch,
without which the bar raised on the first frame (before the PiP had been
laid out and could be tested against) would hold itself up for as long as
the pointer stayed on the PiP.

**Anything mid-interaction pins its chrome open** — an open dropdown, text
being edited, the help overlay, or a scrubber drag. Without that, opening the
layout dropdown and moving down to an option takes the pointer out of the
reveal band, the toolbar stops being declared, and the menu (which is attached
to a button inside it) disappears before the click lands. Verified with
`--theater --open-menu` and the pointer faked to mid-screen: the toolbar and
all five layout entries stay up.

## Choosing the chat source

**The chat panel has a `SOURCE` list** of every loaded Twitch VOD, labelled
with channel and start time so two VODs from the same channel are telling
apart. Click one to point the chat log at it. This is the normal way to pick
when several Twitch VODs are loaded — the chat you want is often not the video
you are watching, and it does not cost a pane.

Picking by hand sets `chat.manual`, which stops the automatic binding (and a
chat-source pane's takeover) from overriding the choice later. Switching source
resets `exported_to` and the log path, so the new chat exports to its own file.

**Chat from a VOD that is not on screen** is the `Chat from another VOD...`
button, also in the chat panel. It takes a Twitch VOD url and loads it as a
**chat-source** slot: that resolves the VOD's timeline for the anchor but
never opens a player (`vs_player.c` returns early for `VS_ROLE_CHAT`), so it
costs a slot and no decoding. It renders as an orange-bordered panel showing
the VOD's date, duration, id and title, with its own Update now / Auto
buttons. This is the muted-VOD case, and the one thing the `SOURCE` list
above cannot do, since that can only offer what is already loaded.

Binding does not happen at the moment the url is committed: the VOD's
PROGRAM-DATE-TIME is not known until it resolves. The commit clears
`chat.manual` and `chat.vod_url` instead, and `vs_tick`'s binder — which
already prefers a chat-only source over a video one — takes it the moment it
is ready.

Kick cannot be a chat source; `vs_add_source` refuses it with "chat replay is
Twitch only". Verified: committing a kick.com url adds no source at all.

### This used to be asked in the wrong place

Until recently the choice was a `LOAD AS: Video | Chat log` toggle on every
empty pane, and **again** in the channel browser's header for its target
slot. Three things were wrong with that. It asked before you had picked
anything, in the UI for picking video. It was duplicated, so the two could
disagree about a slot. And choosing wrong silently spent a whole video slot
on a chat log, which looks like the VOD failed to load.

The chat panel already had the better mechanism — a list that binds the log
to any loaded Twitch source — so the fix was mostly deletion: `g_as_chat`,
both toggles and their handlers went, every add path became
`VS_ROLE_VIDEO`, and the one capability the toggle had that the list did not
moved here as the button above. `VS_ROLE_CHAT` itself stays: it is still a
real state, just not something the VOD picker asks about.

## Channel browser

Typing a channel name lists Twitch and Kick VODs in two columns, which is the
fastest way to find a simulcast worth syncing. Click a row to load it into the
pane slot the browser was opened from. `F` opens it on the first free slot.

**PAIR** marks a VOD that has a counterpart on the other platform starting
within 120 s — i.e. the same simulcast session. Since both platforms' ingest
clocks agree to the millisecond (see above), this is a reliable signal rather
than a heuristic, and paired rows line up visually across the two columns.

Channel names are saved to `vodsync.channels` next to the executable, one per
line, and appear as chips in both the browser and each empty pane. The file is
plain text and hand-editable.

### Listing mechanics

Listing is one `yt-dlp --flat-playlist` call per platform, which is fast.
**Twitch's flat listing has no `timestamp`**, so those rows show `date ...`
until a background thread resolves them one at a time, top-down; the list is
usable immediately and dates fill in as they land. Kick returns timestamps in
the flat listing, so its column is complete at once.

A channel that exists on only one platform is not an error — that column just
comes back empty.

Watch out: strings handed to `snprintf` as **arguments** must use a single `%`,
not `%%`. Escaping the yt-dlp `--print` template as `%%(id)s` made every lookup
silently return nothing, because the `%%` was never processed.

## Architecture

```
src/
  main.c                  window, Clay lifecycle, frame loop, selftest
  vs_app.{h,c}            data model + sync engine        (no renderer deps)
  vs_resolve.{h,c}        yt-dlp subprocess, WinHTTP, HLS/PDT parser
  vs_player.{h,c}         libmpv + GL render API -> raylib FBO
  vs_chat.{h,c}           chat log loading, cursor tracking
  vs_session.{h,c}        save/load sessions and bookmarks
  vs_ui.{h,c}             Clay layout, input handling
  vs_browse.{h,c}         channel lookup, VOD listing, saved channels
tools/
  vs_chat_sync.py         drives an external chat exporter, merges the log
  vs_theme.h              colours, spacing, font ids
  clay_renderer_raylib.c  vendored, patched (see below)
  vs_test_align.c         headless resolver/alignment check
tools/
  build.py                MSVC build driver
  vs_chat_prep.py         TwitchDownloader JSON -> flat chat log
```

`vs_app.h` deliberately includes neither raylib nor mpv; GPU pane state lives
behind an opaque `struct vs_pane_gpu *`. That keeps the model and sync engine
compilable headless, which is what `vs_test_align.c` exercises.

Frame order in `main.c` matters: **mpv renders into its FBOs before
`BeginDrawing()`**, so it never lands inside raylib's draw batch.

### Chat is deliberately out-of-process

Twitch has no documented VOD-chat endpoint. Rather than depend on an
undocumented one, `tools/vs_chat_prep.py` converts a TwitchDownloader export
into a flat TSV that the C side parses:

```bash
TwitchDownloaderCLI chatdownload --id <vod id> -o chat.json
```

Then drop the `.json` on the window, or press `C`. The app shells out to the
converter automatically. Chat carries its own wall-clock anchor and its own
offset, so it syncs with no video pane for that VOD — which is the point, since
Twitch VODs often get muted.

---

## Vendored patches — reapply if you update these deps

**`third_party/clay/clay.h`** (0.14) — MSVC's C mode has no C23 `{}`:

- `CLAY__INIT(Clay_SizingAxis) {}` → `{0}`
- `Clay_Dimensions contentSizeCurrent = {}` → `{0}`
- `CLAY_AUTO_ID()` → `CLAY_AUTO_ID({0})` (zero-arg call of a 1-param macro)
- two `CLAY(CLAY_ID("Clay__DebugView…"), { })` → `{0}`

Do **not** switch to `/std:clatest` to avoid these — it makes
`__STDC_VERSION__ >= 202311L`, so the macro expands to `{}` everywhere and MSVC
14.38 rejects all of them.

**`src/clay_renderer_raylib.c`** — four upstream bugs/assumptions:

- Added `#include "clay.h"`: upstream assumes it is compiled into a translation
  unit that already has it.
- **Missing `break` after `OVERLAY_COLOR_END`** — falls through into
  `RECTANGLE`.
- `(Vector2) {}` → `{0}`.
- `Raylib_MeasureText` was `static inline`, so `main.c` could not reference it.
- Replaced the hardwired 3D-model `CUSTOM` case with a
  `Clay_Raylib_CustomHandler` function-pointer dispatch.
- **`Raylib_MeasureText` read the glyph array out of bounds on any byte above
  0x7f** — `int index = text.chars[i] - 32` with a signed `char`, unbounded at
  both ends. Replaced with a `GetCodepointNext` / `GetGlyphIndex` loop. This
  one is not cosmetic: see "Three bugs under the chat list". If you update
  this file, reapply it first.

### Clay gotchas worth remembering

`CLAY()` is a for-loop macro. **Returning out of a `CLAY(){}` body skips the
matching close** and raises
`"There were still open layout elements when EndLayout was called"` — 178 of
them in a single selftest run before this was found. Guard with
`if (cond) { … }` instead, or extract the body into a helper that returns
before opening anything.

Second: an element with **both** `.backgroundColor` and `.custom` emits two
commands, and the background is drawn second. See cause 2 above.

Third: **a clipped element is a scroll container, and Clay measures a scroll
container's children against an unbounded axis.** A `WRAP_WORDS` text inside
one never wraps, because nothing tells it where to. Give such a child an
explicit `CLAY_SIZING_GROW(min, max)` maximum; `clip` on the child does not
substitute, and `clip` on the container does not either.

Fourth: Clay has no text input at all. Fields here are app state plus raylib's
`GetCharPressed`, rendered by Clay with a 1 px `FIXED` element as the caret.
A single `a->editing` int says which field owns the keyboard; `VS_EDIT_CHAT`
and `VS_EDIT_CHANNEL` extend the pane-slot range for the non-pane fields.

---

## Platform API reference

**Twitch**

- `GET https://api.twitch.tv/helix/videos` — Client-ID + app access token, no
  user login for public VODs. Query by `id`, `user_id` or `game_id`. Returns
  `created_at`, `published_at`, `duration`, `stream_id`, `muted_segments`,
  `url`, `thumbnail_url`. **No manifest URL** — metadata only.
- Official playback is the embed player (`player.twitch.tv`), which *is*
  scriptable: `play`, `pause`, `seek`, `getCurrentTime`, `getDuration`, plus
  `READY`/`PLAY`/`PAUSE`/`SEEK`/`ENDED` events. Ads fire inside it with no
  event, which is why this app uses raw HLS instead.

**Kick**

- The official public API ([docs.kick.com](https://docs.kick.com), OAuth 2.1 +
  PKCE) covers Categories, Users, Channels, Livestreams, Chat, Moderation,
  Channel Rewards, KICKs, Ads, Drops and webhook Events. **There is no videos /
  VOD / past-broadcast endpoint at all.**
- The official embed (`player.kick.com/{channel}`) is livestream-only and
  ignores `postMessage` — it honours only `autoplay` and `muted` URL params.
  There is an open request for a control API
  (`KickEngineering/KickDevDocs` issue 445). So a Kick embed cannot be synced,
  which is what forced the native-player architecture.
- Undocumented: `kick.com/api/v2/channels/{slug}/videos` and
  `kick.com/api/v1/video/{uuid}`, both behind Cloudflare. yt-dlp handles this
  via `curl_cffi` impersonation.

Both undocumented paths are ToS-grey and can break without warning. Fine for a
local personal tool; not something to ship publicly.
