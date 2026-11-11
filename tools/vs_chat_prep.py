#!/usr/bin/env python3
"""Convert a Twitch chat export into vodsync's flat chat log.

    python tools/vs_chat_prep.py <input.json> <output.tsv>

Accepts TwitchDownloader's chat JSON (the usual way to get VOD chat), which
looks roughly like:

    { "video": { "created_at": "2026-11-05T18:39:33Z", ... },
      "comments": [ { "content_offset_seconds": 12.3,
                      "commenter": { "display_name": "someone" },
                      "message": { "body": "hello", "user_color": "#1E90FF" } } ] }

Output is vodsync's chat log format, which tools/vs_chat_fmt.py defines and
this script no longer spells out for itself - it wrote the format by hand
once, and that is how a converted file and a fetched one drift apart.

Why a file and not a live fetch: Twitch has no documented VOD-chat endpoint.
Keeping ingestion in a script you run yourself means the app never depends on
an undocumented API, and you can regenerate the log whenever the format moves.
Get the JSON with TwitchDownloaderCLI:

    TwitchDownloaderCLI chatdownload --id <vod id> -o chat.json
"""
import json
import pathlib
import sys
from datetime import datetime, timezone

# Sibling module: python puts a script's own directory on sys.path. It must be
# installed beside this file - see tools/installer/vodsync.iss.
import vs_chat_fmt as fmt


def iso_to_unix(s):
    if not s:
        return 0.0
    s = s.strip().replace("Z", "+00:00")
    try:
        dt = datetime.fromisoformat(s)
    except ValueError:
        return 0.0
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.timestamp()


def norm_color(c):
    if not c:
        return "c8c8d2"
    c = str(c).lstrip("#").strip()
    if len(c) == 6:
        try:
            int(c, 16)
            return c.lower()
        except ValueError:
            pass
    return "c8c8d2"


def clean(text):
    # Tabs and newlines are our delimiters; collapse any whitespace run.
    return " ".join(str(text or "").split())


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src = pathlib.Path(sys.argv[1])
    dst = pathlib.Path(sys.argv[2])
    if not src.exists():
        print(f"input not found: {src}", file=sys.stderr)
        return 1

    with src.open("r", encoding="utf-8", errors="replace") as f:
        data = json.load(f)

    comments = data.get("comments")
    if comments is None and isinstance(data, list):
        comments = data
    if comments is None:
        print("no 'comments' array in input", file=sys.stderr)
        return 1

    video = data.get("video") or {}
    start = iso_to_unix(video.get("created_at"))
    streamer = (data.get("streamer") or {}).get("name") or video.get("user_name") or ""

    # Same row builder the exporter uses, so a converted file and a fetched
    # one are the same format down to the emote spans.
    rows = [r for r in (fmt.twitch_row(c) for c in comments) if r]
    rows.sort(key=lambda r: r[0])

    vid = str((data.get("video") or {}).get("id") or "")
    fmt.write_log(dst, rows, start=start or 0.0,
                  label=f"{streamer or src.stem} chat",
                  sources=[fmt.source_line(
                      fmt.SRC_TWITCH, "twitch", streamer, vid,
                      ((data.get("streamer") or {}).get("id") or ""))])

    span = rows[-1][0] / 60.0 if rows else 0
    print(f"wrote {len(rows)} messages ({span:.0f} min) -> {dst}")
    if not start:
        print("note: input had no video.created_at, so the log has no wall-clock "
              "anchor. Load the matching VOD as a chat source in the app, or "
              "nudge the chat offset by hand.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
