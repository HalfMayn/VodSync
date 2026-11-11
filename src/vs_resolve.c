// vodsync - resolution worker: spawns yt-dlp, fetches the HLS manifest, and
// builds an absolute wall-clock timeline from EXT-X-PROGRAM-DATE-TIME.
//
// Both Twitch and Kick emit PROGRAM-DATE-TIME on every segment (Kick runs on
// Amazon IVS, which descends from Twitch's own stack), so one parser serves
// both. PDT is the authoritative start time: yt-dlp's `timestamp` field is
// typically a couple of seconds later than the first segment's PDT.
#include "vs_resolve.h"
#include "vs_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VS_SEP "\x1f"   // unit separator between --print fields

// ---------------------------------------------------------------- platform

vs_platform vs_platform_of(const char *url) {
    if (!url) return VS_PLAT_UNKNOWN;
    if (strstr(url, "twitch.tv")) return VS_PLAT_TWITCH;
    if (strstr(url, "kick.com"))  return VS_PLAT_KICK;
    return VS_PLAT_UNKNOWN;
}

// A stream that is on the air and the recording of that same stream both
// report is_live=True, and they are nothing alike to play. A channel url is a
// thirty-second sliding window that has to be accumulated to be seekable at
// all; a /videos/ url is the whole broadcast so far, already in one seekable
// manifest, which merely keeps growing at the end. The url says which, so ask
// it rather than believing is_live:
//
//   twitch.tv/juliakins                 live stream
//   twitch.tv/videos/2921350139         the same broadcast, in progress
//   kick.com/misterarther               live stream
//   kick.com/misterarther/videos/<id>   the same broadcast, in progress
//
// Treating the second kind as live is what made an in-progress broadcast
// unscrubbable: the live path keeps a window near the edge and discards the
// history behind it, so five hours of perfectly seekable recording came out
// as a few seconds you could not move around in.
bool vs_url_is_vod(const char *url) {
    if (!url) return false;
    return strstr(url, "/videos/") != NULL || strstr(url, "/video/") != NULL;
}

const char *vs_platform_name(vs_platform p) {
    switch (p) {
        case VS_PLAT_TWITCH: return "Twitch";
        case VS_PLAT_KICK:   return "Kick";
        default:             return "?";
    }
}

void vs_ytdlp_extra(char *out, int cap) {
    const char *ck = getenv("VODSYNC_YTDLP_COOKIES");
    if (ck && ck[0]) snprintf(out, cap, " --cookies-from-browser \"%s\"", ck);
    else out[0] = 0;
}

// ------------------------------------------------------- time conversion

// Days since 1970-01-01 for a proleptic Gregorian date (Hinnant's algorithm).
static long long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + (long long)doe - 719468LL;
}

// Parses "2026-11-05T18:39:30.847Z" and +hh:mm / -hh:mm offsets to unix seconds.
double vs_iso8601_to_unix(const char *s) {
    int Y, M, D, h, mi;
    double sec = 0;
    if (sscanf(s, "%d-%d-%dT%d:%d:%lf", &Y, &M, &D, &h, &mi, &sec) != 6) return 0;
    double t = (double)days_from_civil(Y, M, D) * 86400.0 + h * 3600.0 + mi * 60.0 + sec;

    // Walk past the date portion before looking for a zone sign.
    const char *z = s;
    int seen_t = 0;
    while (*z) {
        if (*z == 'T') seen_t = 1;
        else if (seen_t && (*z == 'Z' || *z == '+' || *z == '-')) break;
        z++;
    }
    if (*z == '+' || *z == '-') {
        int oh = 0, om = 0;
        if (sscanf(z + 1, "%d:%d", &oh, &om) >= 1) {
            double off = oh * 3600.0 + om * 60.0;
            t += (*z == '+') ? -off : off;
        }
    }
    return t;
}

// -------------------------------------------------------- manifest parse

void vs_segs_free(vs_seg *segs, int n) {
    if (!segs) return;
    for (int i = 0; i < n; i++) { free(segs[i].url); free(segs[i].map); }
    free(segs);
}

bool vs_parse_manifest(const char *text, vs_seg **segs, int *nsegs,
                       double *pdt_start, double *total, char *err, int errcap) {
    return vs_parse_manifest_ex(text, false, segs, nsegs, pdt_start, total,
                                err, errcap);
}

bool vs_parse_manifest_ex(const char *text, bool keep_urls, vs_seg **segs_out,
                          int *nsegs_out, double *pdt_start, double *total,
                          char *err, int errcap) {
    int cap = 1024, n = 0;
    vs_seg *segs = (vs_seg *)malloc(sizeof(vs_seg) * cap);
    double pos = 0, pending_dur = 0, pending_pdt = 0;
    bool have_pdt = false, pending_disc = false;
    char map[VS_MANI_MAX];
    map[0] = 0;

    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        int llen = eol ? (int)(eol - p) : (int)strlen(p);
        while (llen > 0 && (p[llen - 1] == '\r' || p[llen - 1] == ' ')) llen--;

        if (llen > 0) {
            if (llen == 20 && strncmp(p, "#EXT-X-DISCONTINUITY", 20) == 0) {
                // The whole line, not a prefix: the header's
                // #EXT-X-DISCONTINUITY-SEQUENCE:0 opens with the same 20
                // characters, and taking that for a cut marks segment 0 as a
                // new section and swallows the real cut further down.
                pending_disc = true;
            } else if (llen > 11 && strncmp(p, "#EXT-X-MAP:", 11) == 0) {
                // Whole line, kept verbatim so it can be written back out.
                int c = llen < VS_MANI_MAX - 1 ? llen : VS_MANI_MAX - 1;
                memcpy(map, p, c);
                map[c] = 0;
            } else if (llen > 25 && strncmp(p, "#EXT-X-PROGRAM-DATE-TIME:", 25) == 0) {
                char tmp[64];
                int c = llen - 25;
                if (c > 63) c = 63;
                memcpy(tmp, p + 25, c);
                tmp[c] = 0;
                double t = vs_iso8601_to_unix(tmp);
                if (t > 0) { pending_pdt = t; have_pdt = true; }
            } else if (llen > 8 && strncmp(p, "#EXTINF:", 8) == 0) {
                pending_dur = atof(p + 8);
            } else if (p[0] != '#') {
                if (n == cap) {
                    cap *= 2;
                    segs = (vs_seg *)realloc(segs, sizeof(vs_seg) * cap);
                }
                segs[n].pos = pos;
                segs[n].dur = pending_dur > 0 ? pending_dur : 0;
                segs[n].pdt = pending_pdt;
                // Segment 0 starts the first section rather than interrupting
                // anything, so it is never a cut.
                segs[n].disc = pending_disc && n > 0;
                pending_disc = false;
                segs[n].url = NULL;
                segs[n].map = NULL;
                if (keep_urls) {
                    segs[n].url = (char *)malloc(llen + 1);
                    memcpy(segs[n].url, p, llen);
                    segs[n].url[llen] = 0;
                    if (map[0]) segs[n].map = vs_strdup(map);
                }
                pos += segs[n].dur;
                if (pending_pdt > 0) pending_pdt += segs[n].dur;
                n++;
            }
        }
        if (!eol) break;
        p = eol + 1;
    }

    if (n == 0) {
        vs_segs_free(segs, n);
        snprintf(err, errcap, "manifest contained no segments");
        return false;
    }
    if (!have_pdt || segs[0].pdt <= 0) {
        vs_segs_free(segs, n);
        snprintf(err, errcap, "no PROGRAM-DATE-TIME in manifest; cannot align");
        return false;
    }

    *segs_out  = segs;
    *nsegs_out = n;
    *pdt_start = segs[0].pdt;
    *total     = pos;
    return true;
}

// ------------------------------------------------------- live playlist file
//
// A live channel's playlist is a sliding window about 30 seconds long: by the
// time you have watched a minute, the segments you started on are gone from
// it, and mpv cannot seek anywhere. So the app keeps every segment it has
// seen and writes a playlist of its own, which mpv plays instead of the
// remote one. Two things fall out of that:
//
//   - Position 0 is OUR first segment, whose PROGRAM-DATE-TIME we parsed, so
//     the wall clock maps onto mpv's position exactly - the same arithmetic a
//     VOD gets, and the reason two live panes can be aligned at all.
//   - The seekable window grows for as long as you watch, instead of sliding
//     out from under you.
//
// The file MUST be extended by appending. Writing a new copy and renaming it
// over the old one (the usual way to avoid a torn read) was measured not to
// work: mpv keeps playing the version it first read and stops at its end.
// Appending whole lines in one write is what ffmpeg's HLS reload expects, and
// is what EXT-X-PLAYLIST-TYPE:EVENT means.

static void live_seg_text(const vs_seg *s, char *out, int cap) {
    double sec = (double)(long long)s->pdt;
    int ms = (int)((s->pdt - sec) * 1000.0 + 0.5);
    if (ms > 999) ms = 999;
    if (ms < 0)   ms = 0;
    struct tm gt;
    vs_gmtime(sec, &gt);
    snprintf(out, cap,
             "#EXT-X-PROGRAM-DATE-TIME:%04d-%02d-%02dT%02d:%02d:%02d.%03dZ\n"
             "#EXTINF:%.3f,\n%s\n",
             gt.tm_year + 1900, gt.tm_mon + 1, gt.tm_mday,
             gt.tm_hour, gt.tm_min, gt.tm_sec, ms,
             s->dur > 0 ? s->dur : 2.0, s->url ? s->url : "");
}

// `create` starts a new file with the header; otherwise segments are appended.
//
// One WriteFile call per update, never a buffered stream: mpv re-reads this
// file while we extend it, and a read that lands in the middle of a write
// would hand ffmpeg half a segment URL. Windows makes the new bytes visible
// only once a single write has been copied in, so a reader sees either the
// old file or the whole new tail - and the last line is always complete.
static bool live_write(const char *path, const vs_seg *segs, int from, int to,
                       bool create) {
    int cap = 4096 + (to - from) * (2 * VS_MANI_MAX + 128);
    char *buf = (char *)malloc(cap);
    if (!buf) return false;
    int n = 0;
    if (create)
        n += snprintf(buf + n, cap - n,
                      "#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:6\n"
                      "#EXT-X-PLAYLIST-TYPE:EVENT\n#EXT-X-MEDIA-SEQUENCE:0\n");
    for (int i = from; i < to; i++) {
        if (!segs[i].url) continue;
        // The initialisation segment, written wherever it comes into force:
        // at the top of the file, and again only if it ever changes. On an
        // append, segs[from - 1] is the last segment already in the file, so
        // an unchanged map is not written (and not re-fetched) again.
        const char *prev = (i > 0) ? segs[i - 1].map : NULL;
        if (segs[i].map && (!prev || strcmp(segs[i].map, prev) != 0))
            n += snprintf(buf + n, cap - n, "%s\n", segs[i].map);
        live_seg_text(&segs[i], buf + n, cap - n);
        n += (int)strlen(buf + n);
    }
    if (n == 0) { free(buf); return true; }

    bool ok = vs_file_write(path, buf, n, create);
    free(buf);
    return ok;
}

static void live_path(char *out, int cap) {
    char dir[VS_PATH_MAX];
    vs_temp_dir(dir, sizeof dir);
    // Session time plus a counter: unique between panes and between two
    // copies of the app running at once.
    snprintf(out, cap, "%svodsync_live_%ld_%ld.m3u8", dir,
             (long)vs_now_unix(), vs_next_id());
}

// --------------------------------------------------- sectioned VOD files
//
// Why this exists: a Kick VOD is MPEG-TS, and when the feed drops, the
// playlist gets an EXT-X-DISCONTINUITY and the timestamps after it restart at
// zero. Measured on a 5h47m VOD with one cut 1513s in - a seek to anything
// past the cut is ACCEPTED (the command returns ok, time-pos reports the
// target) and then no frame ever arrives, because the demuxer is waiting on a
// timestamp that nothing in the second timeline carries. 5h22m frozen, which
// is what "the Kick pane will not seek" turned out to be.
//
// Nothing about the playlist's SHAPE fixes it. Rewriting it as
// PLAYLIST-TYPE:VOD and dropping the discontinuity tag were both measured and
// both still freeze, as do hr-seek=no, correct-pts=no and fflags=+genpts.
// What works is splitting the playlist at the cut: a section on its own seeks
// perfectly, because it is then the only timeline ffmpeg has. ffmpeg's concat
// demuxer joins them back into one and rebases the timestamps as it goes,
// which is the thing the HLS reader will not do.
//
// The `duration` lines in the ffconcat are load-bearing: without them concat
// reports a 48-second file and seeking is nonsense. mpv's own EDL does the
// same job, but was measured landing 56s out on the far side of a seam, so
// ffconcat it is.

int vs_sections_count(const vs_seg *segs, int nsegs) {
    if (nsegs <= 0) return 0;
    int n = 1;
    for (int i = 1; i < nsegs; i++)
        if (segs[i].disc) n++;
    return n;
}

void vs_sections_base(char *out, int cap) {
    char dir[VS_PATH_MAX];
    vs_temp_dir(dir, sizeof dir);
    snprintf(out, cap, "%svodsync_sect_%ld_%ld", dir,
             (long)vs_now_unix(), vs_next_id());
}

// One section as an ordinary finished VOD playlist, so ffmpeg treats it as
// seekable and starts it at its own segment 0.
static bool sect_write_one(const char *path, const vs_seg *segs,
                           int from, int to) {
    int cap = 4096 + (to - from) * (2 * VS_MANI_MAX + 128);
    char *buf = (char *)malloc(cap);
    if (!buf) return false;
    int n = snprintf(buf, cap,
                     "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-PLAYLIST-TYPE:VOD\n"
                     "#EXT-X-TARGETDURATION:12\n#EXT-X-MEDIA-SEQUENCE:0\n");
    for (int i = from; i < to; i++) {
        if (!segs[i].url) continue;
        // On the first segment of a section the map in force may have been
        // inherited from an earlier one, so it is written unconditionally
        // there and only on a change after that.
        const char *prev = (i > from) ? segs[i - 1].map : NULL;
        if (segs[i].map && (!prev || strcmp(segs[i].map, prev) != 0))
            n += snprintf(buf + n, cap - n, "%s\n", segs[i].map);
        live_seg_text(&segs[i], buf + n, cap - n);
        n += (int)strlen(buf + n);
    }
    n += snprintf(buf + n, cap - n, "#EXT-X-ENDLIST\n");
    bool ok = vs_file_write(path, buf, n, true);
    free(buf);
    return ok;
}

bool vs_sections_write(const vs_seg *segs, int nsegs, const char *base,
                       char *out, int outcap) {
    if (nsegs <= 0 || !base || !base[0]) return false;
    char cc[16384];
    int  ccn = snprintf(cc, sizeof cc, "ffconcat version 1.0\n");
    char path[VS_PATH_MAX];
    int  from = 0, idx = 0;

    for (int i = 1; i <= nsegs; i++) {
        if (i < nsegs && !segs[i].disc) continue;   // still inside a section
        if (ccn > (int)sizeof cc - 512) return false;
        snprintf(path, sizeof path, "%s.s%d.m3u8", base, idx);
        if (!sect_write_one(path, segs, from, i)) return false;
        double dur = 0;
        for (int k = from; k < i; k++) dur += segs[k].dur;
        // A bare file name, not a path: concat resolves it against the
        // ffconcat's own directory, and both are in the temp directory. It
        // also keeps Windows backslashes out of a file concat has to parse.
        const char *bs = strrchr(path, '\\');
        const char *fs = strrchr(path, '/');
        const char *name = bs > fs ? bs : fs;
        name = name ? name + 1 : path;
        ccn += snprintf(cc + ccn, sizeof cc - ccn,
                        "file %s\nduration %.3f\n", name, dur);
        from = i;
        idx++;
    }
    snprintf(out, outcap, "%s.ffconcat", base);
    return vs_file_write(out, cc, ccn, true);
}

void vs_sections_cleanup(vs_source *s) {
    if (!s->sect_base[0]) return;
    char path[VS_PATH_MAX];
    snprintf(path, sizeof path, "%s.ffconcat", s->sect_base);
    vs_file_delete(path);
    for (int i = 0; i < s->nsect; i++) {
        snprintf(path, sizeof path, "%s.s%d.m3u8", s->sect_base, i);
        vs_file_delete(path);
    }
    s->sect_base[0] = 0;
    s->nsect = 0;
}

void vs_live_cleanup(vs_source *s) {
    if (s->local_m3u8[0]) {
        vs_file_delete(s->local_m3u8);
        s->local_m3u8[0] = 0;
    }
}

// ---------------------------------------------------------------- worker

typedef struct {
    vs_source     *s;
    char           url[VS_URL_MAX];
    vs_flag        done;
    bool           ok;
    char           err[256];
    char           manifest[VS_MANI_MAX];
    char           title[256], channel[96], vid[96];
    double         api_start, total, pdt_start;
    int            height;
    vs_seg        *segs;
    int            nsegs;
    bool           live;
    char           local[300];
    double         live_edge;
    int            nsect;
    char           sect_base[280];
} vs_job;

static vs_job *g_jobs[VS_MAX_SOURCES * 2];
static int     g_njobs = 0;

// Extracts field `idx` of a 0x1f-separated line.
static void field(const char *line, int idx, char *out, int cap) {
    out[0] = 0;
    const char *p = line;
    for (int i = 0; i < idx; i++) {
        p = strchr(p, '\x1f');
        if (!p) return;
        p++;
    }
    const char *e = strchr(p, '\x1f');
    int len = e ? (int)(e - p) : (int)strcspn(p, "\r\n");
    if (len > cap - 1) len = cap - 1;
    memcpy(out, p, len);
    out[len] = 0;
}

static void worker(void *arg) {
    vs_job *j = (vs_job *)arg;

    char extra[256];
    vs_ytdlp_extra(extra, sizeof extra);

    char cmd[VS_URL_MAX + 900];
    snprintf(cmd, sizeof cmd,
        "%s --no-warnings --no-playlist%s "
        "-f \"best[height<=?1080]\" "
        "--print \"%%(id)s" VS_SEP "%%(timestamp)s" VS_SEP "%%(duration)s" VS_SEP
        "%%(uploader)s" VS_SEP "%%(height)s" VS_SEP "%%(url)s" VS_SEP
        "%%(is_live)s" VS_SEP "%%(title)s\" "
        "\"%s\"", vs_ytdlp_cmd(), extra, j->url);

    char *out = NULL;
    int len = 0;
    if (!vs_proc_capture(cmd, &out, &len, 180000, j->err, sizeof j->err)) {
        free(out);
        vs_flag_set(&j->done);
        return;
    }

    // yt-dlp can print notices first; take the last line carrying our separator.
    char *line = NULL;
    char *p = out;
    while (p && *p) {
        char *eol = strchr(p, '\n');
        char *sep = strchr(p, '\x1f');
        if (sep && (!eol || sep < eol)) line = p;
        if (!eol) break;
        *eol = 0;
        p = eol + 1;
    }
    if (!line) {
        snprintf(j->err, sizeof j->err, "could not parse yt-dlp output");
        free(out);
        vs_flag_set(&j->done);
        return;
    }

    char buf[VS_MANI_MAX];
    field(line, 0, j->vid, sizeof j->vid);
    field(line, 1, buf, sizeof buf); j->api_start = atof(buf);
    field(line, 2, buf, sizeof buf); j->total     = atof(buf);
    field(line, 3, j->channel, sizeof j->channel);
    field(line, 4, buf, sizeof buf); j->height    = atoi(buf);
    field(line, 5, j->manifest, sizeof j->manifest);
    field(line, 6, buf, sizeof buf);
    // is_live alone is not the question - see vs_url_is_vod. An in-progress
    // recording answers True to it and still belongs on the VOD path, which
    // already knows how to re-read a manifest that is still growing.
    j->live = (strcmp(buf, "True") == 0) && !vs_url_is_vod(j->url);
    field(line, 7, j->title, sizeof j->title);
    free(out);

    if (strncmp(j->manifest, "http", 4) != 0) {
        snprintf(j->err, sizeof j->err, "no HLS url returned for this VOD");
        vs_flag_set(&j->done);
        return;
    }

    char *mani = NULL;
    int mlen = 0;
    if (!vs_http_get(j->manifest, &mani, &mlen, j->err, sizeof j->err)) {
        vs_flag_set(&j->done);
        return;
    }
    // Segment URLs are kept for every source now, not only live ones: a VOD
    // whose recording was interrupted has to be rewritten as one playlist per
    // continuous section, and that needs the URLs. ~250 KB for a six-hour
    // VOD, which is nothing beside the demuxer cache.
    j->ok = vs_parse_manifest_ex(mani, true, &j->segs, &j->nsegs,
                                 &j->pdt_start, &j->total,
                                 j->err, sizeof j->err);
    free(mani);
    if (j->ok && j->live) {
        // Keep everything the platform just handed us. The window is
        // about thirty seconds of already-published stream, and it is the
        // only history that exists at startup: accumulating from here on
        // grows the recording forward, but nothing recovers what we throw
        // away now. Dropping all but the last few segments is what made a
        // live pane unscrubbable for the first minute - there was nothing
        // behind the playhead to scrub into.
        //
        // What that trim was really for is the START POSITION, since mpv
        // begins at segment 0. That is handled where the pane opens, by
        // seeking to the live edge instead, so the two concerns no longer
        // fight each other.
        int keep = j->nsegs;
        const char *k = getenv("VODSYNC_LIVE_START_SEGS");
        if (k) {
            int v = atoi(k);
            if (v >= 1 && v <= 600) keep = v;
        }
        int from = j->nsegs - keep;
        if (from < 0) from = 0;
        if (from > 0) {
            // The kept segments may have inherited their EXT-X-MAP from one of
            // the segments being dropped, so carry it across before freeing.
            if (!j->segs[from].map)
                for (int i = from; i >= 0; i--)
                    if (j->segs[i].map) {
                        j->segs[from].map = vs_strdup(j->segs[i].map);
                        break;
                    }
            for (int i = 0; i < from; i++) { free(j->segs[i].url); free(j->segs[i].map); }
            memmove(j->segs, j->segs + from, sizeof(vs_seg) * (j->nsegs - from));
            j->nsegs -= from;
        }
        double pos = 0;
        for (int i = 0; i < j->nsegs; i++) { j->segs[i].pos = pos; pos += j->segs[i].dur; }
        j->pdt_start = j->segs[0].pdt;
        j->live_edge = j->segs[j->nsegs - 1].pdt + j->segs[j->nsegs - 1].dur;
        j->total     = j->live_edge - j->pdt_start;

        live_path(j->local, sizeof j->local);
        if (!live_write(j->local, j->segs, 0, j->nsegs, true)) {
            j->ok = false;
            snprintf(j->err, sizeof j->err, "cannot write %s", j->local);
        }
    }
    if (j->ok && !j->live) {
        // A recording the feed dropped out of is several continuous sections
        // wearing one playlist. vs_sections_write explains why that cannot
        // simply be played as it stands.
        j->nsect = vs_sections_count(j->segs, j->nsegs);
        if (j->nsect > 1) {
            char cc[VS_MANI_MAX];
            vs_sections_base(j->sect_base, sizeof j->sect_base);
            if (vs_sections_write(j->segs, j->nsegs, j->sect_base,
                                  cc, sizeof cc)) {
                snprintf(j->manifest, sizeof j->manifest, "%s", cc);
            } else {
                // Fall back to the remote playlist. It plays, and it seeks
                // within the first section, which beats refusing to open it.
                j->sect_base[0] = 0;
                j->nsect = 1;
            }
        }
    }
    vs_flag_set(&j->done);
}

bool vs_resolve_begin(vs_source *s) {
    for (int i = 0; i < g_njobs; i++)
        if (g_jobs[i] && g_jobs[i]->s == s) return false;
    if (g_njobs >= (int)(sizeof g_jobs / sizeof g_jobs[0])) return false;

    vs_job *j = (vs_job *)calloc(1, sizeof(vs_job));
    j->s = s;
    snprintf(j->url, sizeof j->url, "%s", s->page_url);
    g_jobs[g_njobs++] = j;

    s->state = VS_LOAD_RESOLVING;
    snprintf(s->status, sizeof s->status, "resolving %s ...",
             vs_platform_name(s->platform));

    if (!vs_thread_start(worker, j)) {
        s->state = VS_LOAD_FAILED;
        snprintf(s->status, sizeof s->status, "could not start worker thread");
        return false;
    }
    return true;
}

bool vs_resolve_poll(vs_source *s) {
    for (int i = 0; i < g_njobs; i++) {
        vs_job *j = g_jobs[i];
        if (!j || j->s != s) continue;
        if (!vs_flag_get(&j->done)) return false;

        if (j->ok) {
            s->live = j->live;
            if (j->live) {
                // mpv plays our own playlist; the remote one is only re-read.
                snprintf(s->live_url,   sizeof s->live_url,   "%s", j->manifest);
                snprintf(s->local_m3u8, sizeof s->local_m3u8, "%s", j->local);
                snprintf(j->manifest,   sizeof j->manifest,   "%s", j->local);
                s->live_written = j->nsegs;
                s->live_edge    = j->live_edge;
                s->live_lost    = vs_now_unix();
            }
            s->nsect = j->nsect;
            snprintf(s->sect_base, sizeof s->sect_base, "%s", j->sect_base);
            snprintf(s->manifest, sizeof s->manifest, "%s", j->manifest);
            snprintf(s->title,    sizeof s->title,    "%s", j->title);
            snprintf(s->channel,  sizeof s->channel,  "%s", j->channel);
            snprintf(s->vid,      sizeof s->vid,      "%s", j->vid);
            s->api_start = j->api_start;
            s->pdt_start = j->pdt_start;
            s->duration  = j->total;
            s->height    = j->height;
            vs_segs_free(s->segs, s->nsegs);
            s->segs  = j->segs;
            s->nsegs = j->nsegs;
            s->state = VS_LOAD_READY;
            if (s->live) {
                snprintf(s->status, sizeof s->status,
                         "%dp  LIVE  %.0fs buffered", s->height, s->duration);
            } else {
                double skew = (s->api_start > 0) ? s->api_start - s->pdt_start : 0;
                if (s->nsect > 1)
                    snprintf(s->status, sizeof s->status,
                             "%dp  %d seg  %d parts (feed dropped)  "
                             "api skew %+.2fs",
                             s->height, s->nsegs, s->nsect, skew);
                else
                    snprintf(s->status, sizeof s->status,
                             "%dp  %d seg  api skew %+.2fs",
                             s->height, s->nsegs, skew);
            }
        } else {
            vs_segs_free(j->segs, j->nsegs);
            s->state = VS_LOAD_FAILED;
            snprintf(s->status, sizeof s->status, "%s", j->err);
        }
        free(j);
        g_jobs[i] = g_jobs[--g_njobs];
        return true;
    }
    return false;
}

void vs_resolve_cancel(vs_source *s) {
    for (int i = 0; i < g_njobs; i++)
        if (g_jobs[i] && g_jobs[i]->s == s) g_jobs[i]->s = NULL;
}

// --------------------------------------------------- live manifest refresh

#include <time.h>

#define VS_GROWING_WINDOW 300.0   // VOD ends within 5 min of now => still live

bool vs_source_is_growing(const vs_source *s) {
    if (s->state != VS_LOAD_READY || s->pdt_start <= 0) return false;
    double now = vs_now_unix();
    // A live stream grows until the refreshes stop coming back with anything,
    // which is how the app notices the streamer went offline. Segments publish
    // every two seconds, so a gap this long is the stream ending (or the feed
    // being lost), and waiting longer only leaves the playhead pinned against
    // an edge that will never move again.
    if (s->live) return (now - s->live_lost) < 25.0;
    double end = s->pdt_start + s->duration;
    return (now - end) < VS_GROWING_WINDOW;
}

typedef struct {
    vs_source    *s;
    bool          live;
    char          manifest[VS_MANI_MAX];
    vs_flag       done;
    bool          ok;
    vs_seg       *segs;
    int           nsegs;
    double        pdt_start, total;
    char          err[192];
} refresh_job;

static refresh_job *g_refresh[VS_MAX_SOURCES];
static int          g_nrefresh = 0;

static void refresh_worker(void *arg) {
    refresh_job *j = (refresh_job *)arg;
    char *mani = NULL;
    int mlen = 0;
    if (vs_http_get(j->manifest, &mani, &mlen, j->err, sizeof j->err)) {
        j->ok = vs_parse_manifest_ex(mani, true, &j->segs, &j->nsegs,
                                     &j->pdt_start, &j->total,
                                     j->err, sizeof j->err);
        free(mani);
    }
    vs_flag_set(&j->done);
}

static bool refresh_begin(vs_source *s, bool live) {
    const char *url = live ? s->live_url : s->manifest;
    if (s->state != VS_LOAD_READY || !url[0]) return false;
    for (int i = 0; i < g_nrefresh; i++)
        if (g_refresh[i] && g_refresh[i]->s == s) return false;
    if (g_nrefresh >= VS_MAX_SOURCES) return false;

    refresh_job *j = (refresh_job *)calloc(1, sizeof(refresh_job));
    j->s = s;
    j->live = live;
    snprintf(j->manifest, sizeof j->manifest, "%s", url);
    g_refresh[g_nrefresh++] = j;

    if (!vs_thread_start(refresh_worker, j)) {
        free(j);
        g_nrefresh--;
        return false;
    }
    return true;
}

bool vs_refresh_begin(vs_source *s)      { return refresh_begin(s, false); }
bool vs_live_refresh_begin(vs_source *s) { return refresh_begin(s, true); }

// Merges a slid window into the segments we already hold, appends whatever is
// new to the local playlist, and extends the timeline to the new live edge.
// VODSYNC_LIVE_LOG=1 reports every live playlist refresh, which is the only
// way to see a stream whose timeline has quietly stopped growing.
static bool live_log(void) {
    static int on = -1;
    if (on < 0) on = getenv("VODSYNC_LIVE_LOG") ? 1 : 0;
    return on == 1;
}

bool vs_live_refresh_poll(vs_source *s, double *added) {
    if (added) *added = 0;
    for (int i = 0; i < g_nrefresh; i++) {
        refresh_job *j = g_refresh[i];
        if (!j || j->s != s) continue;
        if (!vs_flag_get(&j->done)) return false;

        bool grew = false;
        if (j->ok && j->nsegs > 0) {
            double newest = s->nsegs ? s->segs[s->nsegs - 1].pdt : 0;
            int first_new = -1;
            for (int k = 0; k < j->nsegs; k++)
                if (j->segs[k].pdt > newest + 0.25) { first_new = k; break; }

            if (first_new >= 0) {
                int add = j->nsegs - first_new;
                s->segs = (vs_seg *)realloc(s->segs,
                                            sizeof(vs_seg) * (s->nsegs + add));
                double pos = s->nsegs ? s->segs[s->nsegs - 1].pos +
                                        s->segs[s->nsegs - 1].dur : 0;
                int from = s->nsegs;
                for (int k = 0; k < add; k++) {
                    vs_seg *d = &s->segs[s->nsegs + k];
                    *d = j->segs[first_new + k];
                    d->pos = pos;
                    pos += d->dur;
                    j->segs[first_new + k].url = NULL;   // ownership moved
                    j->segs[first_new + k].map = NULL;
                }
                // Same inheritance as the initial trim: the first segment
                // being appended may rely on a map line further up the
                // remote playlist than the point we are splicing at.
                if (!s->segs[from].map)
                    for (int k = first_new; k >= 0; k--)
                        if (j->segs[k].map) {
                            s->segs[from].map = vs_strdup(j->segs[k].map);
                            break;
                        }
                s->nsegs += add;
                live_write(s->local_m3u8, s->segs, from, s->nsegs, false);
                s->live_written = s->nsegs;
                grew = true;
            }

            double edge = j->segs[j->nsegs - 1].pdt + j->segs[j->nsegs - 1].dur;
            if (live_log())
                fprintf(stderr, "[live] %-6s %2d segs, edge %+.2fs, held %d, "
                                "dur %.1f\n",
                        vs_platform_name(s->platform), j->nsegs,
                        edge - s->live_edge, s->nsegs, s->duration);
            if (edge > s->live_edge) {
                s->live_edge = edge;
                s->live_lost = vs_now_unix();
            }
            // The span the pane can show is measured on the wall clock, not by
            // adding up segment durations: if the stream dropped frames the
            // two disagree, and mpv's own position follows the wall clock.
            double span = s->live_edge - s->pdt_start;
            if (span > s->duration) {
                if (added) *added = span - s->duration;
                s->duration = span;
            }
        }
        if (!j->ok && live_log())
            fprintf(stderr, "[live] %-6s refresh FAILED: %s\n",
                    vs_platform_name(s->platform), j->err);
        vs_segs_free(j->segs, j->nsegs);
        free(j);
        g_refresh[i] = g_refresh[--g_nrefresh];
        return grew;
    }
    return false;
}

bool vs_refresh_poll(vs_source *s, double *added) {
    if (added) *added = 0;
    for (int i = 0; i < g_nrefresh; i++) {
        refresh_job *j = g_refresh[i];
        if (!j || j->s != s) continue;
        if (!vs_flag_get(&j->done)) return false;

        bool grew = false;
        if (j->ok && j->nsegs > 0) {
            // Only accept a manifest that still starts where we think it does;
            // a changed start means the VOD was replaced, not extended.
            if (j->pdt_start > 0 &&
                (j->pdt_start - s->pdt_start) < 1.0 &&
                (s->pdt_start - j->pdt_start) < 1.0 &&
                j->total > s->duration + 0.01) {
                if (added) *added = j->total - s->duration;
                vs_segs_free(s->segs, s->nsegs);
                s->segs     = j->segs;
                s->nsegs    = j->nsegs;
                s->duration = j->total;
                j->segs = NULL;
                grew = true;
                // concat reads its input list once, when it opens. A
                // sectioned VOD that has just got longer therefore describes
                // a timeline mpv cannot see the end of - and seeking into
                // that invisible tail is exactly the freeze all of this is
                // here to prevent. Rewrite the files and have the pane
                // reopen them. A VOD that has only now acquired a second
                // section goes the same way, off the remote playlist and
                // onto our own.
                int ns = vs_sections_count(s->segs, s->nsegs);
                if (ns > 1 || s->nsect > 1) {
                    char cc[VS_MANI_MAX];
                    if (!s->sect_base[0])
                        vs_sections_base(s->sect_base, sizeof s->sect_base);
                    if (vs_sections_write(s->segs, s->nsegs, s->sect_base,
                                          cc, sizeof cc)) {
                        s->nsect = ns;
                        snprintf(s->manifest, sizeof s->manifest, "%s", cc);
                        s->sect_reload = true;
                    }
                }
            }
        }
        vs_segs_free(j->segs, j->nsegs);
        free(j);
        g_refresh[i] = g_refresh[--g_nrefresh];
        return grew;
    }
    return false;
}
