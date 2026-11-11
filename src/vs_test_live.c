// vodsync - live-stream probe.
//
// Answers the questions that decide how live streams can be played at all:
//
//   1. Can mpv play a playlist we write ourselves, holding the live segment
//      URLs we fetched? That is what pins mpv's position 0 to a PDT we know
//      exactly, instead of to whatever window mpv happened to fetch.
//   2. Does it keep going when we append newly published segments to that
//      file, or does it stop at the end it first saw?
//   3. Is it seekable backwards over what we have accumulated?
//   4. How far behind the live edge does playback actually sit?
//
// Usage: vs_test_live.exe <media-playlist-url> [seconds]
#include "vs_resolve.h"
#include <mpv/client.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define MAXSEG 4096

typedef struct {
    double pdt, dur;
    char  *url;
} seg;

static seg  g_seg[MAXSEG];
static int  g_nseg;
static char g_path[MAX_PATH];

static double now_unix(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) |
                           ft.dwLowDateTime;
    return (double)t / 1e7 - 11644473600.0;
}

// Appends every segment newer than the newest we hold. Returns how many.
static int merge(const char *text) {
    double newest = g_nseg ? g_seg[g_nseg - 1].pdt : 0;
    double pdt = 0, dur = 0;
    int added = 0;

    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        int n = eol ? (int)(eol - p) : (int)strlen(p);
        while (n > 0 && (p[n - 1] == '\r' || p[n - 1] == ' ')) n--;

        if (n > 25 && strncmp(p, "#EXT-X-PROGRAM-DATE-TIME:", 25) == 0) {
            char tmp[64];
            int c = n - 25 > 63 ? 63 : n - 25;
            memcpy(tmp, p + 25, c);
            tmp[c] = 0;
            pdt = vs_iso8601_to_unix(tmp);
        } else if (n > 8 && strncmp(p, "#EXTINF:", 8) == 0) {
            dur = atof(p + 8);
        } else if (n > 0 && p[0] != '#') {
            // EXT-X-PREFETCH segments (Kick's low-latency hint) carry no
            // PDT or EXTINF, and arrive as #-prefixed lines, so they are
            // skipped here along with every other tag.
            if (pdt > newest + 0.25 && g_nseg < MAXSEG) {
                char *u = (char *)malloc(n + 1);
                memcpy(u, p, n);
                u[n] = 0;
                g_seg[g_nseg].pdt = pdt;
                g_seg[g_nseg].dur = dur > 0 ? dur : 2.0;
                g_seg[g_nseg].url = u;
                g_nseg++;
                newest = pdt;
                added++;
            }
            pdt = 0;
        }
        if (!eol) break;
        p = eol + 1;
    }
    return added;
}

static bool g_event_tag = true;   // write #EXT-X-PLAYLIST-TYPE:EVENT
static bool g_append    = false;  // append in place instead of rewrite+rename
static int  g_written   = 0;      // segments already in the file

static void seg_lines(char *out, int cap, int from, int to) {
    int n = 0;
    for (int i = from; i < to; i++) {
        time_t sec = (time_t)g_seg[i].pdt;
        int ms = (int)((g_seg[i].pdt - (double)sec) * 1000.0);
        struct tm tm;
        gmtime_s(&tm, &sec);
        n += snprintf(out + n, cap - n,
                "#EXT-X-PROGRAM-DATE-TIME:%04d-%02d-%02dT%02d:%02d:%02d.%03dZ\n"
                "#EXTINF:%.3f,\n%s\n",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, ms,
                g_seg[i].dur, g_seg[i].url);
    }
}

// Writes the accumulated segments as a VOD-shaped playlist. Rewrites go to a
// sibling file and are renamed, so a reader never sees a half-written one;
// appends extend the file in place, which is what EVENT playlists do live.
static bool write_playlist(void) {
    char *body = (char *)malloc(1 << 22);
    body[0] = 0;

    if (g_append && g_written > 0) {
        seg_lines(body, 1 << 22, g_written, g_nseg);
        FILE *f = fopen(g_path, "ab");
        if (!f) { free(body); return false; }
        fputs(body, f);
        fclose(f);
        g_written = g_nseg;
        free(body);
        return true;
    }

    int n = snprintf(body, 1 << 22,
            "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:6\n%s"
            "#EXT-X-MEDIA-SEQUENCE:0\n",
            g_event_tag ? "#EXT-X-PLAYLIST-TYPE:EVENT\n" : "");
    seg_lines(body + n, (1 << 22) - n, 0, g_nseg);

    char tmp[MAX_PATH];
    snprintf(tmp, sizeof tmp, "%s.part", g_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(body); return false; }
    fputs(body, f);
    fclose(f);
    free(body);
    g_written = g_nseg;
    return MoveFileExA(tmp, g_path, MOVEFILE_REPLACE_EXISTING) != 0;
}

static double dprop(mpv_handle *m, const char *name) {
    double v = 0;
    if (mpv_get_property(m, name, MPV_FORMAT_DOUBLE, &v) < 0) return -1;
    return v;
}

static int iprop(mpv_handle *m, const char *name) {
    int v = 0;
    if (mpv_get_property(m, name, MPV_FORMAT_FLAG, &v) < 0) return -1;
    return v;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: vs_test_live <media-playlist-url> [seconds] "
               "[--append] [--no-event]\n");
        return 2;
    }
    const char *url  = argv[1];
    double       secs = argc > 2 ? atof(argv[2]) : 60.0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--append")   == 0) g_append = true;
        if (strcmp(argv[i], "--no-event") == 0) g_event_tag = false;
    }
    printf("mode: %s, playlist-type %s\n",
           g_append ? "append in place" : "rewrite + rename",
           g_event_tag ? "EVENT" : "(none)");

    char err[256];
    char *text = NULL;
    int   len = 0;
    double t_fetch = now_unix();
    if (!vs_http_get(url, &text, &len, err, sizeof err)) {
        printf("FAIL fetch: %s\n", err);
        return 1;
    }
    merge(text);
    free(text);
    if (g_nseg == 0) { printf("FAIL no segments\n"); return 1; }

    printf("fetched %d segments at %.3f\n", g_nseg, t_fetch);
    printf("  window start %.3f  edge %.3f  (edge is now%+.2fs)\n",
           g_seg[0].pdt, g_seg[g_nseg - 1].pdt + g_seg[g_nseg - 1].dur,
           g_seg[g_nseg - 1].pdt + g_seg[g_nseg - 1].dur - t_fetch);

    // Start four segments behind the edge: enough to buffer, close enough to
    // be live. Everything before that is dropped, so segment 0 of the local
    // playlist has a PDT we know exactly.
    int from = g_nseg - 4;
    if (from < 0) from = 0;
    if (from > 0) {
        for (int i = 0; i < from; i++) free(g_seg[i].url);
        memmove(g_seg, g_seg + from, sizeof(seg) * (g_nseg - from));
        g_nseg -= from;
    }
    double anchor = g_seg[0].pdt;     // wall clock of local position 0

    char tdir[MAX_PATH];
    GetTempPathA(MAX_PATH, tdir);
    snprintf(g_path, sizeof g_path, "%svodsync_live_probe.m3u8", tdir);
    if (!write_playlist()) { printf("FAIL cannot write %s\n", g_path); return 1; }
    printf("local playlist %s  anchor %.3f  (%d segs)\n", g_path, anchor, g_nseg);

    mpv_handle *m = mpv_create();
    if (!m) { printf("FAIL mpv_create (libmpv-2.dll missing?)\n"); return 1; }
    mpv_set_option_string(m, "vo", "null");
    mpv_set_option_string(m, "terminal", "no");
    mpv_set_option_string(m, "msg-level", "all=no");
    mpv_set_option_string(m, "audio", "no");
    mpv_set_option_string(m, "hwdec", "no");
    mpv_set_option_string(m, "keep-open", "yes");
    mpv_set_option_string(m, "cache", "yes");
    mpv_set_option_string(m, "cache-on-disk", "no");
    mpv_set_option_string(m, "demuxer-max-bytes", "96MiB");
    mpv_set_option_string(m, "demuxer-max-back-bytes", "192MiB");
    mpv_set_option_string(m, "start", "0");
    // Without this, a playlist with no ENDLIST is treated as live and ffmpeg
    // starts three segments from the end instead of at our segment 0.
    mpv_set_option_string(m, "demuxer-lavf-o", "live_start_index=0");
    mpv_set_option_string(m, "user-agent",
                          "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
    mpv_set_option_string(m, "referrer", strstr(url, "ttvnw.net")
                          ? "https://www.twitch.tv/" : "https://kick.com/");
    if (mpv_initialize(m) < 0) { printf("FAIL mpv_initialize\n"); return 1; }

    const char *cmd[] = { "loadfile", g_path, NULL };
    mpv_command(m, cmd);

    double t0 = now_unix(), last_refresh = 0, last_print = 0;
    bool   seek_tried = false, seek_ok = false;
    double seek_from = 0, seek_target = 0, seek_landed = -1;
    int    refreshes = 0, appended = 0;
    double max_pos = 0;

    while (now_unix() - t0 < secs) {
        mpv_event *e = mpv_wait_event(m, 0.05);
        if (e->event_id == MPV_EVENT_END_FILE) {
            mpv_event_end_file *ef = (mpv_event_end_file *)e->data;
            printf("[%5.1fs] END_FILE reason=%d error=%s\n",
                   now_unix() - t0, ef->reason,
                   ef->error ? mpv_error_string(ef->error) : "-");
        } else if (e->event_id == MPV_EVENT_FILE_LOADED) {
            char *fmt = mpv_get_property_string(m, "file-format");
            printf("[%5.1fs] FILE_LOADED  file-format=%s\n",
                   now_unix() - t0, fmt ? fmt : "?");
            mpv_free(fmt);
        }

        double t = now_unix();

        // Top up the playlist the way the app would.
        if (t - last_refresh > 2.0) {
            last_refresh = t;
            char *txt = NULL;
            int   n = 0;
            if (vs_http_get(url, &txt, &n, err, sizeof err)) {
                int add = merge(txt);
                free(txt);
                refreshes++;
                if (add > 0) {
                    appended += add;
                    if (!write_playlist())
                        printf("[%5.1fs] playlist write FAILED (win32 %lu)\n",
                               t - t0, GetLastError());
                }
            } else {
                printf("[%5.1fs] refresh failed: %s\n", t - t0, err);
            }
        }

        if (t - last_print > 2.0) {
            last_print = t;
            double pos = dprop(m, "time-pos");
            if (pos > max_pos) max_pos = pos;
            struct _stat64 st;
            st.st_size = 0;
            _stat64(g_path, &st);
            printf("[%5.1fs] pos %8.2f  wall %.2f  behind-live %6.2fs  "
                   "seekable %d  cache-end %7.2f  dur %8.2f  eof %d  "
                   "segs %d  file %lldB\n",
                   t - t0, pos, anchor + pos,
                   (g_seg[g_nseg - 1].pdt + g_seg[g_nseg - 1].dur) - (anchor + pos),
                   iprop(m, "seekable"), dprop(m, "demuxer-cache-time"),
                   dprop(m, "duration"), iprop(m, "eof-reached"),
                   g_nseg, (long long)st.st_size);
        }

        // Once there is something behind us, try to seek back into it.
        if (!seek_tried && now_unix() - t0 > 24.0) {
            seek_tried = true;
            seek_from = dprop(m, "time-pos");
            seek_target = seek_from - 12.0;
            if (seek_target < 0) seek_target = 0;
            char buf[32];
            snprintf(buf, sizeof buf, "%.3f", seek_target);
            const char *sk[] = { "seek", buf, "absolute+exact", NULL };
            int rc = mpv_command(m, sk);
            printf("[%5.1fs] seek %.2f -> %.2f : rc=%d %s\n",
                   now_unix() - t0, seek_from, seek_target, rc,
                   rc < 0 ? mpv_error_string(rc) : "ok");
            seek_ok = rc >= 0;
        }
        if (seek_tried && seek_landed < 0 && now_unix() - t0 > 27.0)
            seek_landed = dprop(m, "time-pos");
    }

    printf("\n--- summary ---\n");
    printf("refreshes        %d\n", refreshes);
    printf("segments added   %d  (total %d)\n", appended, g_nseg);
    printf("max position     %.2f\n", max_pos);
    printf("played past the first window: %s\n",
           max_pos > 8.5 ? "YES - appends are picked up" : "NO");
    printf("backward seek    %s", seek_ok ? "accepted" : "refused");
    if (seek_landed >= 0)
        printf("  landed at %.2f (wanted %.2f)", seek_landed, seek_target);
    printf("\n");

    mpv_terminate_destroy(m);
    DeleteFileA(g_path);
    return 0;
}
