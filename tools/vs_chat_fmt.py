#!/usr/bin/env python3
"""vodsync chat log format - the one definition of it.

Both vs_chat_prep.py (convert a file you already have) and vs_chat_sync.py
(fetch and top up) write logs, and the app reads them, so the format lives
here rather than in two places that can drift.


FORMAT 2
--------

    #vodsync-chat 2
    #start <unix seconds>                       anchor for every offset below
    #source <tag> <platform> <channel> <vodid> <user_id>   one per chat
    #label <text>
    <offset>\\t<src>\\t<type>\\t<id>\\t<colour>\\t<user>\\t<emotes>\\t<text>

`text` is LAST because the app's parser takes the rest of the line for it,
which is what lets a message contain a tab without escaping.

Fields:

  offset   seconds from #start, 3 decimals. Negative is allowed: a merged log
           is anchored once, and a chat can start before that instant.
  src      a tag declared by a #source line ("tw", "kk"). Which chat this
           line came from. Needed for more than labelling - see below.
  type     m message, r reply, s system/notice. Reserved now so replies and
           sub notices do not need a third format.
  id       8 hex digits, a stable hash of the platform's own message id. Only
           the exporter uses it, to de-duplicate a top-up exactly; the app
           never loads it. Before this, a top-up keyed on
           (time, user, text) and silently dropped a message someone sent
           twice in the same second - and Kick's timestamps are only
           second-resolution, so that was not a hypothetical.
  colour   RRGGBB, or "-" for none (the app then picks one from the name).
  user     display name.
  emotes   "-", or occurrences separated by ";", each "start:len:src:id".
           start and len are BYTE offsets into text, because the app indexes
           bytes. src here is the emote's own platform, which is not always
           the line's: a Twitch emote can only appear in Twitch chat, but the
           two platforms number theirs independently, so the id needs saying
           whose it is.
  text     what was typed, with nothing substituted.


WHY EMOTES ARE SPANS AND NOT MARKERS IN THE TEXT

Kick hands us emotes in-band, as "[emote:5118082:mraJedi]" inside the
message. The obvious move is to keep that shape and let the app parse it,
and it is the wrong one:

  - it inflates text. A 20-character token replaces an 8-character name, so a
    message of nothing but emotes triples in length. The app reads lines into
    a 2048-byte buffer with fgets and a message into a 512-byte field, and an
    over-long line is SPLIT - the front half parses as a truncated message
    and the tail, having no tabs, is counted as malformed and dropped. Spans
    keep text at the length someone actually typed, so neither limit moves.
  - it would make a literal "[emote:1:x]" typed in chat render as an emote.
  - and the app would need a tokeniser per platform anyway.

So Kick's tokens are normalised OUT on the way in, and Twitch's fragment ids
are normalised IN to the same spans. One representation, from either source.


WHY 7TV IS NOT IN HERE

7TV emotes are matched by NAME against a channel's current 7TV set, which the
app fetches at run time. Resolving them at export would freeze them into the
log, so a log exported today would show yesterday's set forever. Keeping them
out means the file holds only what the platform itself told us, which is the
part that cannot be recovered later.
"""
import hashlib
import re

VERSION = 2

SRC_TWITCH = "tw"
SRC_KICK = "kk"

PLATFORM_OF = {SRC_TWITCH: "twitch", SRC_KICK: "kick"}

TYPE_MESSAGE = "m"
TYPE_REPLY = "r"
TYPE_SYSTEM = "s"

MAX_TEXT = 500          # Twitch's own limit; Kick is close enough
MAX_USER = 46
MAX_EMOTES = 24         # per message, which no real message approaches

# [emote:<id>:<name>] - Kick's in-band form.
KICK_EMOTE = re.compile(r"\[emote:(\d+):([^\]]*)\]")


# ------------------------------------------------------------------ scrubbing

def clean(s):
    """One line, no tabs, no control characters.

    A newline or a tab in a message would desync every field after it, so
    this is load-bearing rather than cosmetic.
    """
    s = str(s or "")
    out = []
    for ch in s:
        o = ord(ch)
        if o in (9, 10, 13):
            out.append(" ")
        elif o < 32 or o == 127:
            continue
        else:
            out.append(ch)
    return " ".join("".join(out).split())


def norm_color(c):
    """RRGGBB, or "-" when there is none.

    "-" rather than a made-up grey: the app can colour from the name instead,
    and a log that says "no colour" is not the same as one that says grey.
    """
    c = str(c or "").lstrip("#").strip()
    if len(c) == 6:
        try:
            int(c, 16)
            return c.upper()
        except ValueError:
            pass
    return "-"


def msg_id(native):
    """8 hex digits from the platform's own id. Fixed width, stable between
    runs, and short enough that carrying it costs ~400 KB on a 50k-message
    log instead of the 1.8 MB a Kick uuid per line would."""
    if not native:
        return "00000000"
    return hashlib.sha1(str(native).encode("utf-8")).hexdigest()[:8]


# -------------------------------------------------------------------- emotes

def emotes_encode(spans):
    """spans: iterable of (start, length, src, id) over text's BYTES."""
    spans = [s for s in spans if s[1] > 0][:MAX_EMOTES]
    if not spans:
        return "-"
    return ";".join(f"{a}:{n}:{s}:{i}" for a, n, s, i in spans)


def emotes_decode(field):
    if not field or field == "-":
        return []
    out = []
    for part in field.split(";"):
        bits = part.split(":")
        if len(bits) != 4:
            continue
        try:
            out.append((int(bits[0]), int(bits[1]), bits[2], bits[3]))
        except ValueError:
            continue
    return out


def _clip(text, spans):
    """Clamp text to MAX_TEXT characters and drop spans that fall off the end.

    Byte offsets are recomputed after clipping, since clipping is by
    character and the offsets are in bytes.
    """
    if len(text) > MAX_TEXT:
        text = text[:MAX_TEXT]
    nbytes = len(text.encode("utf-8"))
    kept = [s for s in spans if s[0] + s[1] <= nbytes]
    return text, kept


# ----------------------------------------------------------- Twitch messages

def twitch_row(c, src=SRC_TWITCH):
    """One row from a TwitchDownloaderCLI comment, or None to skip it.

    The body is rebuilt from `fragments` rather than taken from `body`,
    because the fragments are what say where the emotes are - each one is
    either plain text or an emote, in order. message.emoticons carries
    begin/end too, but as character offsets into a string we would then have
    to trust, so fragments are the safer of the two.
    """
    off = c.get("content_offset_seconds")
    if off is None:
        off = (c.get("message") or {}).get("content_offset_seconds")
    if off is None:
        return None
    msg = c.get("message") or {}

    # Build the body from the raw fragments, measuring spans against the raw
    # string, and clean the result ONCE at the end. Cleaning each fragment
    # separately would be wrong: clean() collapses runs of whitespace and
    # strips the ends, and the fragments carry the spacing between words, so
    # per-fragment cleaning moves every byte after the first collapsed space
    # and leaves the spans pointing at the wrong characters.
    frags = msg.get("fragments")
    spans = []
    if frags:
        parts = []
        nbytes = 0
        for fr in frags:
            piece = str(fr.get("text") or "")
            if not piece:
                continue
            em = fr.get("emoticon") or {}
            eid = em.get("emoticon_id") or em.get("id")
            if eid:
                spans.append((nbytes, len(piece.encode("utf-8")),
                              SRC_TWITCH, str(eid)))
            parts.append(piece)
            nbytes += len(piece.encode("utf-8"))
        text = "".join(parts)
    else:
        text = str(msg.get("body") or "")

    # Same conservative rule as Kick: if cleaning moved anything, the spans
    # can no longer be trusted, so they go rather than point at the wrong
    # characters. In practice chat rarely needs cleaning at all.
    cleaned = clean(text)
    if cleaned != text:
        spans = []
    text = cleaned
    if not text:
        return None
    text, spans = _clip(text, spans)

    who = ((c.get("commenter") or {}).get("display_name")
           or (c.get("commenter") or {}).get("name") or "?")
    return (float(off), src, TYPE_MESSAGE, msg_id(c.get("_id") or c.get("id")),
            norm_color(msg.get("user_color")), clean(who)[:MAX_USER],
            emotes_encode(spans), text)


# ------------------------------------------------------------- Kick messages

def kick_row(m, anchor, src=SRC_KICK):
    """One row from a Kick /messages entry, or None to skip it.

    Kick timestamps are absolute, so the offset is created_at - anchor. That
    is simpler than Twitch, where the export counts from the VOD's own start.
    """
    from datetime import datetime, timezone
    ts = m.get("created_at")
    if not ts:
        return None
    try:
        dt = datetime.fromisoformat(str(ts).replace("Z", "+00:00"))
    except ValueError:
        return None
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    off = dt.timestamp() - anchor

    raw = str(m.get("content") or "")
    # Normalise the in-band tokens out, recording where each emote lands in
    # the text that is left.
    spans = []
    parts = []
    nbytes = 0
    pos = 0
    for mo in KICK_EMOTE.finditer(raw):
        before = raw[pos:mo.start()]
        parts.append(before)
        nbytes += len(before.encode("utf-8"))
        name = mo.group(2) or ""
        if name:
            spans.append((nbytes, len(name.encode("utf-8")),
                          SRC_KICK, mo.group(1)))
            parts.append(name)
            nbytes += len(name.encode("utf-8"))
        pos = mo.end()
    parts.append(raw[pos:])
    text = "".join(parts)

    # clean() may move bytes around (collapsing runs of spaces), so when it
    # changes anything the spans can no longer be trusted and are dropped
    # rather than left pointing at the wrong characters.
    cleaned = clean(text)
    if cleaned != text:
        spans = []
        text = cleaned
    if not text:
        return None
    text, spans = _clip(text, spans)

    sender = m.get("sender") or {}
    ident = sender.get("identity") or {}
    who = sender.get("username") or sender.get("slug") or "?"
    kind = TYPE_REPLY if m.get("type") == "reply" else TYPE_MESSAGE
    if m.get("type") not in ("message", "reply"):
        kind = TYPE_SYSTEM
    return (float(off), src, kind, msg_id(m.get("id")),
            norm_color(ident.get("color")), clean(who)[:MAX_USER],
            emotes_encode(spans), text)


# ------------------------------------------------------------------ file i/o

def source_line(tag, platform, channel, vodid, user_id=""):
    """#source <tag> <platform> <channel> <vodid> <user_id>

    user_id is the platform's own numeric id for the channel. It is here
    because 7TV and BTTV both address a channel by it rather than by name,
    and the app cannot recover it later without an authenticated API call -
    while the exporter has it for free in the response it already fetched.
    """
    return "#source " + " ".join([
        tag, platform, channel or "-", str(vodid or "-"), str(user_id or "-")])


def write_log(path, rows, start=0.0, label="", sources=()):
    """Rows are the 8-tuples the *_row builders return, any order.

    Written to a sibling .tmp and renamed over the target, because the app
    may be reading this file while a top-up rewrites it and a half-written
    log would load as a truncated one.
    """
    import pathlib as _pl
    path = _pl.Path(path)
    rows = sorted(rows, key=lambda r: r[0])
    tmp = path.with_suffix(path.suffix + ".tmp")
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"#vodsync-chat {VERSION}\n")
        if start:
            f.write(f"#start {start:.3f}\n")
        for s in sources:
            f.write(s.rstrip("\n") + "\n")
        f.write(f"#label {label or 'chat'}\n")
        for off, src, kind, mid, col, user, emotes, text in rows:
            f.write(f"{off:.3f}\t{src}\t{kind}\t{mid}\t{col}\t{user}\t"
                    f"{emotes}\t{text}\n")
    tmp.replace(path)
    return len(rows)


def read_log(path):
    """Returns (version, header_lines, rows) with rows as 8-tuples.

    A version 1 log reads as version 1 rows widened to the same shape, so a
    log exported before any of this still loads and still tops up. Its lines
    carry no source, no id and no emotes, so they get the Twitch tag (v1
    could only ever be Twitch), a hash of what the line contains, and none.
    """
    import pathlib
    p = pathlib.Path(path)
    version = 1
    header, rows = [], []
    if not p.exists():
        return version, header, rows
    with p.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n\r")
            if not line:
                continue
            if line.startswith("#"):
                header.append(line)
                if line.startswith("#vodsync-chat "):
                    try:
                        version = int(line.split()[1])
                    except (IndexError, ValueError):
                        pass
                continue
            if version >= 2:
                parts = line.split("\t", 7)
                if len(parts) != 8:
                    continue
                try:
                    rows.append((float(parts[0]), parts[1], parts[2], parts[3],
                                 parts[4], parts[5], parts[6], parts[7]))
                except ValueError:
                    continue
            else:
                parts = line.split("\t", 3)
                if len(parts) != 4:
                    continue
                try:
                    off = float(parts[0])
                except ValueError:
                    continue
                rows.append((off, SRC_TWITCH, TYPE_MESSAGE,
                             msg_id(f"v1:{parts[0]}:{parts[2]}:{parts[3]}"),
                             norm_color(parts[1]), parts[2], "-", parts[3]))
    return version, header, rows


def merge(old_rows, new_rows):
    """Exact de-duplication on the message id, falling back to the old
    (time, user, text) key only for rows that carry no usable id.

    Returns (merged, added).
    """
    seen_ids = {r[3] for r in old_rows if r[3] and r[3] != "00000000"}
    seen_weak = {(round(r[0], 2), r[5], r[7]) for r in old_rows}
    merged = list(old_rows)
    added = 0
    for r in new_rows:
        mid = r[3]
        if mid and mid != "00000000":
            if mid in seen_ids:
                continue
            seen_ids.add(mid)
        else:
            key = (round(r[0], 2), r[5], r[7])
            if key in seen_weak:
                continue
            seen_weak.add(key)
        merged.append(r)
        added += 1
    merged.sort(key=lambda r: r[0])
    return merged, added
