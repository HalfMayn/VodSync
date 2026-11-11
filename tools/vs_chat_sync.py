#!/usr/bin/env python3
"""Export (or top up) a VOD's chat into vodsync's flat chat log.

    python tools/vs_chat_sync.py --vod <url-or-id> --out chat.tsv [--since N]
    python tools/vs_chat_sync.py --platform kick --channel <slug> \\
                                 --anchor <unix> --span <secs> --out chat.tsv

`--since N` fetches only from N seconds into the VOD onwards and merges the
result into an existing log, which is what makes periodic updates cheap while a
stream is still running. Without it the whole chat is fetched.

Prints one status line on success:

    OK <max_offset_seconds> <total_rows> <new_rows>

Exit codes: 0 ok, 2 no exporter available, 1 anything else.

**This script does not talk to Twitch itself.** It drives an external chat
exporter that you install. Twitch publishes no documented VOD-chat endpoint,
so rather than bake a scraper for an undocumented one into this project, the
fragile part stays in a tool you choose and can update independently.

**Kick is the exception, and it is a deliberate one.** There is no tool to
drive: chat-downloader does not support Kick, and nothing else does either.
So this script talks to kick.com/api/v2 itself, which is undocumented and
will break when Kick changes it. It is confined to fetch_kick() below so
there is one place to fix. What makes it worth owning: the endpoint needs no
auth, a 5h47m VOD costs about 46 requests and 25 seconds (measured), the
timestamps are absolute so alignment is a subtraction, and Kick hands over
emote ids and names in-band, so Kick emotes need no second API at all.

Supported exporters, tried in this order:

  1. TwitchDownloaderCLI  (recommended)
       https://github.com/lay295/TwitchDownloader
       Put it on PATH, or set VODSYNC_CHAT_TOOL to its full path.
  2. chat-downloader      (pip install chat-downloader)

Use --tool to force a specific one, or --stub for a self-test that generates
synthetic messages without any network access.
"""
import argparse
import json
import os
import pathlib
import random
import zlib
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone

# Sibling module: python puts a script's own directory on sys.path, so this
# works wherever the script is invoked from. It must be installed beside this
# file - see tools/installer/vodsync.iss.
import vs_chat_fmt as fmt


# --------------------------------------------------------------- utilities

def iso_to_unix(s):
    if not s:
        return 0.0
    s = str(s).strip().replace("Z", "+00:00")
    try:
        dt = datetime.fromisoformat(s)
    except ValueError:
        return 0.0
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.timestamp()


def norm_color(c):
    c = str(c or "").lstrip("#").strip()
    if len(c) == 6:
        try:
            int(c, 16)
            return c.lower()
        except ValueError:
            pass
    return "c8c8d2"


def clean(text):
    return " ".join(str(text or "").split())


def vod_id(vod):
    """Accept a bare id, a /videos/<id> url, or anything ending in digits."""
    v = str(vod).strip().rstrip("/")
    if "/" in v:
        v = v.rsplit("/", 1)[-1]
    return v.lstrip("v").split("?")[0]


# ------------------------------------------------------------- exporters

def find_tool(explicit):
    if explicit:
        return explicit if (pathlib.Path(explicit).exists() or
                            shutil.which(explicit)) else None
    env = os.environ.get("VODSYNC_CHAT_TOOL")
    if env and (pathlib.Path(env).exists() or shutil.which(env)):
        return env
    for name in ("TwitchDownloaderCLI", "TwitchDownloaderCLI.exe"):
        found = shutil.which(name)
        if found:
            return found
    # Fall back to a copy vendored in the repo, so running the exe works with
    # no PATH entry and no environment variable set.
    here = pathlib.Path(__file__).resolve().parent          # tools/
    # Both builds can sit in the same folder - the Windows one keeps its
    # .exe - so ask for the right one first rather than whichever exists.
    names = (("TwitchDownloaderCLI.exe", "TwitchDownloaderCLI")
             if os.name == "nt" else
             ("TwitchDownloaderCLI", "TwitchDownloaderCLI.exe"))
    for d in (here.parent / "third_party" / "twitchdownloader",
              here.parent / "build",
              here.parent):
        for name in names:
            cand = d / name
            if cand.exists() and (os.name == "nt" or os.access(cand, os.X_OK)):
                return str(cand)
    return None


def run_twitchdownloader(tool, vid, since, tmpdir):
    out = pathlib.Path(tmpdir) / "chat.json"
    cmd = [tool, "chatdownload", "--id", vid, "-o", str(out),
           "--compression", "None"]
    if since and since > 0:
        cmd += ["-b", str(int(since))]
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if r.returncode != 0 or not out.exists():
        tail = (r.stderr or r.stdout or "")[-400:]
        raise RuntimeError(f"TwitchDownloaderCLI failed: {tail}")
    with out.open("r", encoding="utf-8", errors="replace") as f:
        return json.load(f)


def run_chat_downloader(vid, since):
    try:
        from chat_downloader import ChatDownloader
    except ImportError:
        raise RuntimeError("chat-downloader is not installed")
    url = f"https://www.twitch.tv/videos/{vid}"
    kwargs = {"start_time": int(since)} if since and since > 0 else {}
    chat = ChatDownloader().get_chat(url, **kwargs)
    comments = []
    for m in chat:
        comments.append({
            "content_offset_seconds": m.get("time_in_seconds"),
            "commenter": {"display_name": (m.get("author") or {}).get("name")},
            "message": {"body": m.get("message"),
                        "user_color": (m.get("author") or {}).get("colour")},
        })
    return {"comments": comments, "video": {}}


def run_stub(vid, since, span=600):
    """Synthetic chat, for exercising the pipeline with no network."""
    # zlib.crc32, not hash(): str hashing is salted per process, so
    # hash() would make repeated runs produce different messages.
    rng = random.Random(zlib.crc32((vid + str(int(since or 0))).encode()))
    users = ["ripsipiirakka92", "hievryone77", "Ace_76", "og_delta_",
             "xMacii", "OnePunchSid", "bruinski15", "Chronomeo127"]
    start = int(since or 0)
    comments = []
    t = start
    while t < start + span:
        t += rng.uniform(0.4, 3.0)
        comments.append({
            "content_offset_seconds": round(t, 3),
            "commenter": {"display_name": rng.choice(users)},
            "message": {"body": rng.choice([
                "LFG", "that was clean", "o7", "KEKW", "he is cooking",
                "no shot", "first try?", "chat is this real",
            ]), "user_color": rng.choice(
                ["#1E90FF", "#FF69B4", "#00FF7F", "#FFD700", "#FF4500"])},
        })
    return {"comments": comments, "video": {}}


# ------------------------------------------------------------ log merging

# --------------------------------------------------------------------- Kick
#
# Everything undocumented about Kick lives in this section.
#
# The shape, measured rather than assumed:
#   GET /api/v2/channels/<slug>            -> {"id": .., "user_id": ..}
#   GET /api/v2/channels/<id>/messages?cursor=<microseconds>
#       -> {"data": {"messages": [...25 of them...], "cursor": "<older>"}}
# It pages BACKWARDS, 25 at a time, and each page covered ~455s of chat on
# the channel this was built against. The slug form of /messages answers 401;
# only the numeric id works. A browser User-Agent is required - without one
# Cloudflare answers instead of Kick.

KICK_UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
           "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36")


def kick_get(url, timeout=30):
    req = urllib.request.Request(url, headers={
        "User-Agent": KICK_UA, "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def kick_channel(slug):
    """The numeric chat id for a channel slug, plus the Kick user id (which
    is what 7TV keys a Kick channel by - not the chat id)."""
    d = kick_get(f"https://kick.com/api/v2/channels/{slug}")
    cid = d.get("id")
    if not cid:
        raise RuntimeError(f"kick: no channel id for {slug!r}")
    return int(cid), d.get("user_id")


# A page is 25 entries, always. limit, per_page and size are all ignored -
# tested, not assumed - so the only lever on wall-clock time is how many
# walks run at once.
KICK_PAGE = 25
KICK_MAX_PAGES = 20000          # whole-job ceiling, not per segment


class _Progress:
    """Pages done, printed to STDERR.

    stderr because the app parses stdout for the single "OK ..." line, and a
    progress line there would break that contract. Without this the Kick path
    printed nothing at all until it finished, so a job that takes minutes
    looked indistinguishable from a hung one.
    """

    def __init__(self, label):
        self.lock = threading.Lock()
        self.pages = 0
        self.msgs = 0
        self.waits = 0      # times a rate limit made us back off
        self.gaps = 0       # pages given up on, i.e. messages NOT fetched
        self.label = label
        self.last = 0.0

    def waited(self):
        with self.lock:
            self.waits += 1

    def gap(self):
        with self.lock:
            self.gaps += 1

    def add(self, pages, msgs):
        with self.lock:
            self.pages += pages
            self.msgs += msgs
            now = time.time()
            if now - self.last > 2.0:
                self.last = now
                extra = ""
                if self.waits:
                    extra += f", {self.waits} backoffs"
                if self.gaps:
                    extra += f", {self.gaps} GAPS"
                print(f"  {self.label}: {self.pages} pages, "
                      f"{self.msgs} messages{extra}",
                      file=sys.stderr, flush=True)


def _walk_back(chat_id, seg_start, seg_end, budget, prog):
    """One segment, walked from its end back past its start.

    Returns (messages, pages). Stops on a page that predates seg_start, on a
    cursor that stops moving, or on an empty page - a stream that was offline
    leaves gaps in the history, and none of the three is an error.
    """
    cursor = str(int(seg_end * 1_000_000))
    out = []
    pages = 0
    gaps = 0
    while pages < budget:
        url = (f"https://kick.com/api/v2/channels/{chat_id}"
               f"/messages?cursor={cursor}")
        d = None
        for attempt in range(6):
            try:
                d = kick_get(url)
                break
            except urllib.error.HTTPError as e:
                if e.code in (429, 503):
                    # "Slow down" is answered by slowing down and carrying
                    # on. Giving up here is how half a VOD went missing:
                    # requests bunch up where the chat is densest, so the
                    # pages lost were the ones that mattered most.
                    prog.waited()
                    time.sleep(1.0 * (2 ** attempt))
                    continue
                raise RuntimeError(f"kick: HTTP {e.code} on /messages")
            except urllib.error.URLError as e:
                if attempt < 5:
                    prog.waited()
                    time.sleep(1.0 * (2 ** attempt))
                    continue
                raise RuntimeError(f"kick: {e.reason}")
        if d is None:
            # Still refused after six tries. Record it as a hole rather than
            # ending the walk as though the segment were finished.
            prog.gap()
            gaps += 1
            break
        pages += 1
        data = d.get("data") or {}
        msgs = data.get("messages") or []
        if not msgs:
            break
        out.extend(msgs)
        prog.add(1, len(msgs))
        nxt = data.get("cursor")
        if not nxt or nxt == cursor:
            break
        cursor = str(nxt)
        # Stop once this page reaches at or before the start of the range.
        # It has to be asked of the messages: the cursor is an opaque
        # positional token, and an earlier version of this line compared
        # int(cursor)/1e6 to seg_start as though it were microseconds, which
        # ended the walk early and returned 19,500 of 26,506 messages.
        times = [t for t in (iso_to_unix(m.get("created_at")) for m in msgs)
                 if t]
        if times and min(times) <= seg_start:
            break
        time.sleep(0.03)
    return out, pages, gaps


def fetch_kick(slug, start_unix, end_unix, workers=8):
    """Every message between two instants, oldest first, de-duplicated.

    Overlap between adjacent segments is expected and harmless: each walk
    overshoots its own start by up to one page, and the message id makes
    removing the duplicates exact rather than a guess.
    """
    chat_id, _user_id = kick_channel(slug)
    span = end_unix - start_unix
    if span <= 0:
        return [], 0

    # ONE walk, from one seed, start to finish. This is not a performance
    # choice, it is the only correct one.
    #
    # `cursor` is an opaque positional token. Only a value the API itself
    # handed back addresses anything; one fabricated from a wall-clock
    # instant returns a sparse, unrelated slice instead of failing. So the
    # VOD cannot be split into time ranges and walked in parallel - that was
    # tried, and it returned 12,534 of a VOD's 26,506 messages, identically
    # every run, with no rate limiting involved. A fabricated mid-VOD seed
    # was measured walking 32 hours of chat in 80 pages and leaving the VOD
    # altogether, and a seed inside a 640-message-a-minute burst returned
    # pages spanning 500-900 seconds.
    #
    # The one seed below is also fabricated, and that is fine: it only has to
    # land somewhere at or after the end of the range, after which every
    # cursor is the API's own. Everything before the VOD's end is reached by
    # following the chain.
    #
    # `workers` is kept as an argument so the app's command line does not
    # have to change, and ignored.
    _ = workers
    n = 1
    budget = KICK_MAX_PAGES
    prog = _Progress("kick chat")

    raw = []
    pages = 0
    raw, pages, gaps = _walk_back(chat_id, start_unix, end_unix, budget, prog)

    # De-duplicate on the platform's own id, then keep only what the VOD
    # covers, oldest first.
    by_id = {}
    for m in raw:
        key = m.get("id") or (m.get("created_at"), m.get("content"))
        if key not in by_id:
            by_id[key] = m
    keep = []
    for m in by_id.values():
        t = iso_to_unix(m.get("created_at"))
        if t and start_unix - 1.0 <= t <= end_unix + 1.0:
            keep.append(m)
    keep.sort(key=lambda m: iso_to_unix(m.get("created_at")))
    note = ""
    if gaps:
        note = (f"  WARNING: {gaps} page(s) could not be fetched after "
                f"repeated backoff; this log has holes in it")
    print(f"  kick chat: {pages} pages, {len(raw)} fetched, "
          f"{len(keep)} kept after de-duplication, {prog.waits} backoffs",
          file=sys.stderr, flush=True)
    if note:
        print(note, file=sys.stderr, flush=True)
    return keep, pages


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--platform", default="twitch",
                    choices=("twitch", "kick"))
    ap.add_argument("--vod", default=None,
                    help="twitch: the VOD to export. kick: recorded in the "
                         "#source line only, since chat is fetched by time")
    ap.add_argument("--channel", default=None,
                    help="kick: the channel slug, which is how its chat is "
                         "addressed")
    ap.add_argument("--out", required=True)
    ap.add_argument("--since", type=float, default=0)
    ap.add_argument("--span", type=float, default=0,
                    help="kick: seconds of VOD after --anchor to cover")
    ap.add_argument("--workers", type=int, default=1,
                    help="accepted and ignored: Kick's cursor cannot be "
                         "fabricated, so its chat can only be walked as one "
                         "chain (see fetch_kick)")
    ap.add_argument("--tool", default=None)
    ap.add_argument("--anchor", type=float, default=0,
                    help="unix time of VOD offset 0, written as #start")
    ap.add_argument("--label", default=None)
    ap.add_argument("--stub", action="store_true",
                    help="generate synthetic chat, no network")
    a = ap.parse_args()
    if a.platform == "twitch" and not a.vod:
        print("--vod is required for twitch", file=sys.stderr)
        return 1
    if a.platform == "kick" and not a.channel:
        print("--channel is required for kick", file=sys.stderr)
        return 1

    out = pathlib.Path(a.out)
    vid = vod_id(a.vod) if a.vod else ""
    ver, header, old_rows = fmt.read_log(out)

    # The anchor decides where every offset in the file sits, so its
    # precedence matters and has not changed: the app passes one derived from
    # the manifest's PROGRAM-DATE-TIME, which is the accurate value; an
    # anchor already in the log comes next, so a top-up never downgrades one
    # that was set correctly; Twitch's own created_at runs a couple of
    # seconds late and is the last resort.
    anchor = a.anchor
    if not anchor:
        for h in header:
            if h.startswith("#start "):
                try:
                    anchor = float(h.split(" ", 1)[1])
                except ValueError:
                    pass
                break

    data = None
    pages = 0
    try:
        if a.platform == "kick":
            # Kick messages are stamped with absolute time, so without an
            # anchor there is nothing to measure an offset from. Refuse
            # rather than write a log whose offsets mean nothing.
            if not anchor:
                print("--anchor is required for kick (unix time of VOD "
                      "offset 0)", file=sys.stderr)
                return 1
            span = a.span if a.span > 0 else 24 * 3600.0
            msgs, pages = fetch_kick(a.channel,
                                     anchor + max(0.0, a.since),
                                     anchor + span,
                                     workers=a.workers)
            new_rows = [r for r in (fmt.kick_row(m, anchor) for m in msgs) if r]
        elif a.stub:
            data = run_stub(vid, a.since)
            new_rows = None
        else:
            tool = find_tool(a.tool)
            if tool:
                with tempfile.TemporaryDirectory() as td:
                    data = run_twitchdownloader(tool, vid, a.since, td)
            else:
                data = run_chat_downloader(vid, a.since)
            new_rows = None
    except RuntimeError as e:
        msg = str(e)
        if "not installed" in msg or "chat-downloader" in msg:
            print("no chat exporter found.\n"
                  "  Install TwitchDownloaderCLI and put it on PATH:\n"
                  "    https://github.com/lay295/TwitchDownloader\n"
                  "  or set VODSYNC_CHAT_TOOL to its full path,\n"
                  "  or: pip install chat-downloader", file=sys.stderr)
            return 2
        print(msg, file=sys.stderr)
        return 1
    except Exception as e:
        print(f"{type(e).__name__}: {e}", file=sys.stderr)
        return 1

    if new_rows is None:
        comments = data.get("comments")
        if comments is None and isinstance(data, list):
            comments = data
        new_rows = [r for r in (fmt.twitch_row(c) for c in (comments or []))
                    if r]
        if not anchor:
            anchor = iso_to_unix((data.get("video") or {}).get("created_at"))

    merged, added = fmt.merge(old_rows, new_rows)

    if a.platform == "kick":
        tag, plat, chan = fmt.SRC_KICK, "kick", a.channel
        # Looked up again rather than threaded out of the fetch: it is one
        # cheap request, and it keeps fetch_kick returning just messages.
        try:
            _cid, uid = kick_channel(a.channel)
        except Exception:
            uid = ""
        user_id = uid or ""
        default_label = f"kick {a.channel} chat"
    else:
        streamer = (data or {}).get("streamer", {}) or {}
        tag, plat, chan = fmt.SRC_TWITCH, "twitch", streamer.get("login") or ""
        user_id = streamer.get("id") or ""
        default_label = f"twitch {vid} chat"

    label = a.label or next((h[7:] for h in header if h.startswith("#label ")),
                            default_label)

    # Keep any #source lines already in the log and add ours if it is new, so
    # a log that several sources have written to keeps declaring them all.
    sources = [h for h in header if h.startswith("#source ")]
    mine = fmt.source_line(tag, plat, chan, vid, user_id)
    if not any(s.split()[1:2] == [tag] for s in sources):
        sources.append(mine)

    fmt.write_log(out, merged, start=anchor or 0.0, label=label,
                  sources=sources)

    max_off = merged[-1][0] if merged else 0.0
    extra = f" pages={pages}" if pages else ""
    print(f"OK {max_off:.3f} {len(merged)} {added}{extra}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
