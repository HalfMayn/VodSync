// vodsync - channel browser implementation.
//
// Listing is cheap (yt-dlp --flat-playlist). Twitch's flat listing does not
// include a timestamp, so those are filled in afterwards by a background
// thread that resolves entries top-down; the list is usable immediately and
// dates appear as they land.
#include "vs_browse.h"
#include "vs_resolve.h"
#include "vs_platform.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VS_SEP "\x1f"

// ----------------------------------------------------------------- helpers

// Titles are full of emoji the UI font has no glyphs for. Keep printable
// ASCII and collapse the rest to spaces.
static void sanitize(const char *in, char *out, int cap) {
    int o = 0;
    bool last_space = true;
    for (const unsigned char *p = (const unsigned char *)in; *p && o < cap - 1; p++) {
        unsigned char ch = *p;
        if (ch >= 32 && ch < 127) {
            out[o++] = (char)ch;
            last_space = (ch == ' ');
        } else if (!last_space && o < cap - 1) {
            out[o++] = ' ';
            last_space = true;
        }
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = 0;
}

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

// The browser only cares whether it got output; the platform call carries
// an error string the other callers use.
static bool run_capture(const char *cmdline, char **out, int timeout_ms) {
    char err[160];
    int len = 0;
    *out = NULL;
    if (vs_proc_capture(cmdline, out, &len, timeout_ms, err, sizeof err))
        return true;
    free(*out);          // output captured before a non-zero exit
    *out = NULL;
    return false;
}

// ------------------------------------------------------------- list worker

typedef struct {
    vs_browser   *b;
    char          channel[VS_CHAN_MAX];
    vs_flag done;
    vs_vod        tw[VS_MAX_VODS]; int ntw;
    vs_vod        kk[VS_MAX_VODS]; int nkk;
    char          status[192];
} list_job;

static int parse_listing(const char *out, vs_platform plat, vs_vod *dst, int cap) {
    int n = 0;
    const char *p = out;
    while (*p && n < cap) {
        const char *eol = strchr(p, '\n');
        int llen = eol ? (int)(eol - p) : (int)strlen(p);
        if (llen > 0 && strchr(p, '\x1f') && (!eol || strchr(p, '\x1f') < eol)) {
            char line[2048];
            int c = llen < 2047 ? llen : 2047;
            memcpy(line, p, c);
            line[c] = 0;

            vs_vod v;
            memset(&v, 0, sizeof v);
            v.platform = plat;
            char buf[VS_URL_MAX];
            field(line, 0, v.id, sizeof v.id);
            field(line, 1, buf, sizeof buf);
            v.ts  = (strcmp(buf, "NA") == 0) ? 0 : atof(buf);
            field(line, 2, buf, sizeof buf);
            v.dur = (strcmp(buf, "NA") == 0) ? 0 : atof(buf);
            field(line, 3, v.url, sizeof v.url);
            field(line, 4, buf, sizeof buf);
            sanitize(buf, v.title, sizeof v.title);

            // Kick urls arrive with a #__youtubedl_smuggle fragment.
            char *hash = strchr(v.url, '#');
            if (hash) *hash = 0;

            if (v.id[0] && strncmp(v.url, "http", 4) == 0) {
                v.ts_wanted = (v.ts == 0);
                dst[n++] = v;
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    return n;
}

static void list_worker(void *arg) {
    list_job *j = (list_job *)arg;
    // Single %, not %%: this string is passed to snprintf as an argument,
    // not as part of the format, so it is never scanned for conversions.
    const char *fmt =
        "%(id)s" VS_SEP "%(timestamp)s" VS_SEP "%(duration)s" VS_SEP
        "%(url)s" VS_SEP "%(title)s";

    char extra[256];
    vs_ytdlp_extra(extra, sizeof extra);

    char cmd[VS_URL_MAX];
    char *out = NULL;

    // filter=all, not filter=archives: archives alone omits highlights and
    // uploads, which for an active channel is a large share of the VODs.
    snprintf(cmd, sizeof cmd,
        "%s --flat-playlist --playlist-end %d --no-warnings%s "
        "--print \"%s\" \"https://www.twitch.tv/%s/videos?filter=all\"",
        vs_ytdlp_cmd(), VS_MAX_VODS, extra, fmt, j->channel);
    if (run_capture(cmd, &out, 120000)) {
        j->ntw = parse_listing(out, VS_PLAT_TWITCH, j->tw, VS_MAX_VODS);
        free(out);
        out = NULL;
    }

    snprintf(cmd, sizeof cmd,
        "%s --flat-playlist --playlist-end %d --no-warnings%s "
        "--print \"%s\" \"https://kick.com/%s/videos\"",
        vs_ytdlp_cmd(), VS_MAX_VODS, extra, fmt, j->channel);
    if (run_capture(cmd, &out, 120000)) {
        j->nkk = parse_listing(out, VS_PLAT_KICK, j->kk, VS_MAX_VODS);
        free(out);
    }

    if (j->ntw == 0 && j->nkk == 0)
        snprintf(j->status, sizeof j->status,
                 "no VODs found for \"%s\" on either platform", j->channel);
    else
        snprintf(j->status, sizeof j->status,
                 "%d Twitch, %d Kick", j->ntw, j->nkk);

    vs_flag_set(&j->done);
}

// --------------------------------------------------------- backfill worker

typedef struct {
    vs_browser   *b;
    vs_flag done;
    vs_flag cancel;
    char          urls[VS_MAX_VODS][VS_URL_MAX];
    double        ts[VS_MAX_VODS];
    vs_flag filled;     // how many entries have a result
    int           n;
} fill_job;

static void fill_worker(void *arg) {
    fill_job *j = (fill_job *)arg;
    char extra2[256];
    vs_ytdlp_extra(extra2, sizeof extra2);
    for (int i = 0; i < j->n; i++) {
        if (vs_flag_get(&j->cancel)) break;
        char cmd[VS_URL_MAX + 200];
        snprintf(cmd, sizeof cmd,
            "%s --no-warnings --no-playlist%s "
            "--print \"%%(timestamp)s\" \"%s\"", vs_ytdlp_cmd(), extra2,
            j->urls[i]);
        char *out = NULL;
        if (run_capture(cmd, &out, 90000)) {
            double t = atof(out);
            j->ts[i] = (t > 1000000000.0) ? t : 0;
            free(out);
        }
        vs_flag_set_value(&j->filled, i + 1);
    }
    vs_flag_set(&j->done);
}

// ------------------------------------------------------------------- state

static list_job *g_list;
static fill_job *g_fill;

void vs_browse_open(vs_browser *b, int slot) {
    b->open = true;
    b->slot = slot;
    // Default here rather than at each call site: three places open this
    // for a pane, and all of them should mean a pane.
    b->chat_target = false;
}

void vs_browse_close(vs_browser *b) {
    b->open = false;
    if (g_fill) vs_flag_set(&g_fill->cancel);
}

int vs_browse_count(const vs_browser *b) { return b->ntw + b->nkk; }

// Accepts a pasted channel url as well as a bare name.
//
// The old version looked for ".com/" and took what followed, which works
// for kick.com and not at all for twitch.tv: pasting a Twitch channel url
// saved a channel called "https:". Strip a scheme, then a host - anything
// before the first slash that looks like one, meaning it contains a dot -
// and keep the first path segment.
bool vs_channel_normalize(const char *in, char *out, int cap) {
    if (!in || !in[0] || cap < 2) return false;

    const char *p = in;
    while (*p == ' ' || *p == '\t') p++;
    const char *scheme = strstr(p, "://");
    if (scheme) p = scheme + 3;
    if (strncmp(p, "www.", 4) == 0) p += 4;

    const char *slash = strchr(p, '/');
    if (slash) {
        bool host_like = false;
        for (const char *q = p; q < slash; q++) if (*q == '.') host_like = true;
        if (host_like) p = slash + 1;
    }

    int o = 0;
    for (; *p && o < cap - 1; p++) {
        if (*p == '/' || *p == '?' || *p == '#') break;
        if (*p == ' ' || *p == '\t') continue;
        out[o++] = (char)tolower((unsigned char)*p);
    }
    out[o] = 0;
    return out[0] != 0;
}

bool vs_browse_begin(vs_browser *b, const char *channel) {
    if (!channel || !channel[0]) return false;
    if (b->state == VS_LOAD_RESOLVING) return false;

    char name[VS_CHAN_MAX];
    if (!vs_channel_normalize(channel, name, sizeof name)) return false;

    snprintf(b->query, sizeof b->query, "%s", name);
    b->ntw = b->nkk = 0;
    b->backfill_done = 0;
    b->tw_live = b->kk_live = false;
    vs_live_check_begin(name);
    b->state = VS_LOAD_RESOLVING;
    snprintf(b->status, sizeof b->status, "looking up %s ...", name);

    if (g_list) { free(g_list); g_list = NULL; }
    if (g_fill) { vs_flag_set(&g_fill->cancel); g_fill = NULL; }

    list_job *j = (list_job *)calloc(1, sizeof(list_job));
    j->b = b;
    snprintf(j->channel, sizeof j->channel, "%s", name);
    g_list = j;

    bool started = vs_thread_start(list_worker, j);
    if (!started) {
        b->state = VS_LOAD_FAILED;
        snprintf(b->status, sizeof b->status, "could not start lookup thread");
        free(j);
        g_list = NULL;
        return false;
    }
    return true;
}

static void start_backfill(vs_browser *b) {
    int n = 0;
    fill_job *j = (fill_job *)calloc(1, sizeof(fill_job));
    j->b = b;
    for (int i = 0; i < b->ntw && n < VS_MAX_VODS; i++) {
        if (!b->tw[i].ts_wanted) continue;
        snprintf(j->urls[n], VS_URL_MAX, "%s", b->tw[i].url);
        n++;
    }
    j->n = n;
    if (n == 0) { free(j); return; }
    g_fill = j;
    bool started = vs_thread_start(fill_worker, j);
    if (!started) { free(j); g_fill = NULL; return; }
}

bool vs_browse_poll(vs_browser *b) {
    bool changed = false;

    {
        bool tw = false, kk = false;
        if (vs_live_check_poll(&tw, &kk)) {
            b->tw_live = tw;
            b->kk_live = kk;
            changed = true;
        }
    }

    if (g_list && vs_flag_get(&g_list->done)) {
        list_job *j = g_list;
        g_list = NULL;
        if (j->b == b) {
            memcpy(b->tw, j->tw, sizeof b->tw);
            memcpy(b->kk, j->kk, sizeof b->kk);
            b->ntw = j->ntw;
            b->nkk = j->nkk;
            b->state = (j->ntw || j->nkk) ? VS_LOAD_READY : VS_LOAD_FAILED;
            snprintf(b->status, sizeof b->status, "%s", j->status);
            if (b->state == VS_LOAD_READY) start_backfill(b);
            changed = true;
        }
        free(j);
    }

    if (g_fill) {
        long filled = vs_flag_value(&g_fill->filled);
        if (filled > b->backfill_done) {
            // Map results back onto the Twitch entries that wanted them.
            int k = 0;
            for (int i = 0; i < b->ntw; i++) {
                if (!b->tw[i].ts_wanted) continue;
                if (k < filled && b->tw[i].ts == 0 && g_fill->ts[k] > 0)
                    b->tw[i].ts = g_fill->ts[k];
                k++;
            }
            b->backfill_done = (int)filled;
            changed = true;
        }
        if (vs_flag_get(&g_fill->done)) {
            free(g_fill);
            g_fill = NULL;
        }
    }
    return changed;
}

// --------------------------------------------------------- saved channels

static void channels_path(char *out, int cap) {
    char dir[VS_PATH_MAX];
    vs_state_dir(dir, sizeof dir);
    snprintf(out, cap, "%s/vodsync.channels", dir);
}

// ------------------------------------------------------- live right now?
//
// yt-dlp exits with "The channel is not currently live" when it is not, and
// prints is_live otherwise. Two cheap calls, on their own thread: the browser
// paints the LIVE NOW row whenever the answer arrives.

typedef struct {
    char          channel[VS_CHAN_MAX];
    vs_flag done;
    bool          tw, kk;
} live_job;

static live_job *g_livejob = NULL;

static bool channel_is_live(const char *host, const char *channel) {
    char extra[256];
    vs_ytdlp_extra(extra, sizeof extra);
    char cmd[VS_URL_MAX];
    snprintf(cmd, sizeof cmd,
        "%s --no-warnings --no-playlist%s --print \"%s\" \"%s/%s\"",
        vs_ytdlp_cmd(), extra, "%(is_live)s", host, channel);
    char *out = NULL;
    bool live = false;
    if (run_capture(cmd, &out, 90000)) {
        live = out && strstr(out, "True") != NULL;
        free(out);
    }
    return live;
}

static void live_worker(void *arg) {
    live_job *j = (live_job *)arg;
    j->tw = channel_is_live("https://www.twitch.tv", j->channel);
    j->kk = channel_is_live("https://kick.com", j->channel);
    vs_flag_set(&j->done);
}

bool vs_live_check_begin(const char *channel) {
    if (g_livejob || !channel || !channel[0]) return false;
    live_job *j = (live_job *)calloc(1, sizeof(live_job));
    snprintf(j->channel, sizeof j->channel, "%s", channel);
    g_livejob = j;
    bool started = vs_thread_start(live_worker, j);
    if (!started) { free(j); g_livejob = NULL; return false; }
    return true;
}

bool vs_live_check_poll(bool *tw, bool *kk) {
    live_job *j = g_livejob;
    if (!j) return false;
    if (!vs_flag_get(&j->done)) return false;
    if (tw) *tw = j->tw;
    if (kk) *kk = j->kk;
    free(j);
    g_livejob = NULL;
    return true;
}

// ------------------------------------------------- newest VOD of a channel
//
// Used for the chat log of a live stream: chat is exported from the VOD that
// is being recorded while the stream runs, not from the channel URL.

typedef struct {
    char          channel[VS_CHAN_MAX];
    char          url[VS_URL_MAX];
    vs_flag done;
} vod_job;

static vod_job *g_vodjob = NULL;

static void vod_worker(void *arg) {
    vod_job *j = (vod_job *)arg;
    char extra[256];
    vs_ytdlp_extra(extra, sizeof extra);
    char cmd[VS_URL_MAX];
    snprintf(cmd, sizeof cmd,
        "%s --flat-playlist --playlist-end 1 --no-warnings%s "
        "--print \"%s\" \"https://www.twitch.tv/%s/videos?filter=all\"",
        vs_ytdlp_cmd(), extra, "%(id)s", j->channel);
    char *out = NULL;
    if (run_capture(cmd, &out, 120000)) {
        char id[64];
        int n = 0;
        for (const char *p = out; *p && n < 63; p++) {
            if (*p >= '0' && *p <= '9') id[n++] = *p;
            else if (n > 0) break;
        }
        id[n] = 0;
        if (n > 0)
            snprintf(j->url, sizeof j->url, "https://www.twitch.tv/videos/%s", id);
        free(out);
    }
    vs_flag_set(&j->done);
}

bool vs_live_vod_begin(const char *channel) {
    if (g_vodjob || !channel || !channel[0]) return false;
    vod_job *j = (vod_job *)calloc(1, sizeof(vod_job));
    snprintf(j->channel, sizeof j->channel, "%s", channel);
    g_vodjob = j;
    bool started = vs_thread_start(vod_worker, j);
    if (!started) { free(j); g_vodjob = NULL; return false; }
    return true;
}

bool vs_live_vod_poll(char *url_out, int cap) {
    vod_job *j = g_vodjob;
    if (!j) return false;
    if (!vs_flag_get(&j->done)) return false;
    snprintf(url_out, cap, "%s", j->url);
    free(j);
    g_vodjob = NULL;
    return true;
}

void vs_channels_load(vs_channels *c) {
    c->n = 0;
    char path[VS_PATH_MAX];
    channels_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[VS_CHAN_MAX + 8];
    while (fgets(line, sizeof line, f) && c->n < VS_MAX_SAVED) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                           line[len-1] == ' ')) line[--len] = 0;
        if (len == 0 || line[0] == '#') continue;
        snprintf(c->names[c->n], VS_CHAN_MAX, "%s", line);
        c->n++;
    }
    fclose(f);
}

void vs_channels_save(const vs_channels *c) {
    char path[VS_PATH_MAX];
    channels_path(path, sizeof path);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "# vodsync saved channels\n");
    for (int i = 0; i < c->n; i++) fprintf(f, "%s\n", c->names[i]);
    fclose(f);
}

bool vs_channels_add(vs_channels *c, const char *name) {
    if (!name || !name[0]) return false;
    for (int i = 0; i < c->n; i++)
        if (vs_stricmp(c->names[i], name) == 0) return false;   // already saved
    if (c->n >= VS_MAX_SAVED) return false;
    snprintf(c->names[c->n], VS_CHAN_MAX, "%s", name);
    c->n++;
    vs_channels_save(c);
    return true;
}

void vs_channels_remove(vs_channels *c, int i) {
    if (i < 0 || i >= c->n) return;
    for (int k = i; k < c->n - 1; k++)
        memcpy(c->names[k], c->names[k + 1], VS_CHAN_MAX);
    c->n--;
    vs_channels_save(c);
}

void vs_channels_clear(vs_channels *c) {
    c->n = 0;
    vs_channels_save(c);        // an empty file, not an absent one
}
