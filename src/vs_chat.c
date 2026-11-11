// vodsync - chat replay log loading and cursor tracking.
#include "vs_chat.h"
#include "vs_emote_sets.h"
#include "vs_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A span array grows alongside the messages. One allocation for the whole
// log rather than one per message: most messages have no emotes at all.
static int push_span(vs_chat *c, unsigned short start, unsigned short len,
                     int emote) {
    if (emote < 0 || emote > 0xFFFF) return 0;
    if (c->nspans == c->spans_cap) {
        c->spans_cap = c->spans_cap ? c->spans_cap * 2 : 1024;
        c->spans = (vs_emote_span *)realloc(
            c->spans, sizeof(vs_emote_span) * c->spans_cap);
    }
    c->spans[c->nspans].start = start;
    c->spans[c->nspans].len   = len;
    c->spans[c->nspans].emote = (unsigned short)emote;
    c->nspans++;
    return 1;
}

// The log says "-" when the platform gave the sender no colour. Picking one
// from the name keeps it stable between sessions and between messages, which
// a random or fixed grey would not - and it is what the platforms' own
// clients do.
static void colour_from_name(const char *user, unsigned char *r,
                             unsigned char *g, unsigned char *b) {
    static const unsigned char pal[][3] = {
        { 255, 105,  97 }, { 255, 179,  71 }, { 253, 253, 150 },
        { 119, 221, 119 }, { 119, 190, 230 }, { 174, 155, 255 },
        { 255, 153, 204 }, { 160, 231, 229 }, { 240, 180, 120 },
        { 180, 215, 140 },
    };
    unsigned long long h = 1469598103934665603ULL;
    for (const char *p = user; p && *p; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c += 32;      // same colour whatever the case
        h ^= (unsigned char)c;
        h *= 1099511628211ULL;
    }
    int i = (int)(h % (sizeof pal / sizeof pal[0]));
    *r = pal[i][0]; *g = pal[i][1]; *b = pal[i][2];
}

// "start:len:src:id", occurrences separated by ';'. The emote's NAME is the
// text it covers, so interning reads it back out of the message rather than
// the log carrying it twice.
static void parse_spans(vs_chat *c, const char *field, const char *text,
                        int textlen, vs_chat_msg *m) {
    m->span_at = c->nspans;
    m->nspan = 0;
    if (!field || !field[0] || (field[0] == '-' && !field[1])) return;

    const char *p = field;
    while (*p && m->nspan < 255) {
        int start = atoi(p);
        const char *q = strchr(p, ':');
        if (!q) break;
        int len = atoi(q + 1);
        const char *r = strchr(q + 1, ':');
        if (!r) break;
        const char *idp = strchr(r + 1, ':');
        if (!idp) break;
        char tag[8];
        int taglen = (int)(idp - (r + 1));
        if (taglen < 0 || taglen > 6) break;
        memcpy(tag, r + 1, taglen);
        tag[taglen] = 0;
        idp++;
        char id[VS_EMOTE_ID_MAX];
        int k = 0;
        while (idp[k] && idp[k] != ';' && k < (int)sizeof id - 1) {
            id[k] = idp[k];
            k++;
        }
        id[k] = 0;

        vs_emote_src src = VS_EM_TWITCH;
        if (strcmp(tag, "kk") == 0) src = VS_EM_KICK;
        else if (strcmp(tag, "7tv") == 0) src = VS_EM_7TV;
        else if (strcmp(tag, "bttv") == 0) src = VS_EM_BTTV;

        // The name is the run of text this span covers. Bounds are checked
        // against the text we actually stored, not against what the log
        // claims, because a span past the end would otherwise read whatever
        // follows the buffer.
        char name[VS_EMOTE_NAME_MAX];
        name[0] = 0;
        if (start >= 0 && len > 0 && start + len <= textlen &&
            len < (int)sizeof name) {
            memcpy(name, text + start, len);
            name[len] = 0;
            int e = vs_emote_intern(src, id, name);
            if (e >= 0 && start <= 0xFFFF && len <= 0xFFFF &&
                push_span(c, (unsigned short)start, (unsigned short)len, e))
                m->nspan++;
        }

        const char *semi = strchr(idp, ';');
        if (!semi) break;
        p = semi + 1;
    }
}

// "#source <tag> <platform> <channel> <vodid> [label]"
static void parse_source(vs_chat *c, const char *rest) {
    if (c->nsources >= VS_CHAT_SOURCES) return;
    vs_chat_source *s = &c->sources[c->nsources];
    memset(s, 0, sizeof *s);
    char plat[32] = { 0 };
    // Fields are single tokens by construction (the writer replaces an empty
    // one with "-"), so this is a plain scan. The user id is last and may be
    // absent in a log written before the format carried it, so only the
    // first two fields are required.
    int got = sscanf(rest, "%7s %31s %63s %63s %31s",
                     s->tag, plat, s->channel, s->vid, s->user_id);
    if (got < 2) return;
    if (got < 5 || strcmp(s->user_id, "-") == 0) s->user_id[0] = 0;
    if (strcmp(plat, "twitch") == 0)    s->platform = VS_PLAT_TWITCH;
    else if (strcmp(plat, "kick") == 0) s->platform = VS_PLAT_KICK;
    else                                s->platform = VS_PLAT_UNKNOWN;
    c->nsources++;
}

static int source_index(const vs_chat *c, const char *tag) {
    for (int i = 0; i < c->nsources; i++)
        if (strcmp(c->sources[i].tag, tag) == 0) return i;
    return VS_SRC_NONE;
}

void vs_chat_free(vs_chat *c) {
    free(c->msgs);
    c->msgs = NULL;
    free(c->spans);
    c->spans = NULL;
    c->nspans = c->spans_cap = 0;
    c->nsources = 0;
    c->n = c->cap = 0;
    c->cursor = 0;
    c->state = VS_LOAD_EMPTY;
}

void vs_chat_set_anchor(vs_chat *c, double pdt_start) {
    c->pdt_start = pdt_start;
}

static void push(vs_chat *c, const vs_chat_msg *m) {
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 4096;
        c->msgs = (vs_chat_msg *)realloc(c->msgs, sizeof(vs_chat_msg) * c->cap);
    }
    c->msgs[c->n++] = *m;
}

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

// Finds a helper script. Two layouts have to work: the shipped one, where
// tools/ sits beside the binary, and the development one, where the binary
// is in build/ and tools/ is a level up.
//
// Only the second was handled, which was invisible until the app was run
// from a bundle: python exits 2 for a script it cannot open, and 2 is also
// the exporter's "not installed" code, so a missing script was reported as
// a missing exporter - the wrong problem, in the one place a user would
// act on it.
static void tool_path(const char *name, char *out, int cap) {
    char exe[VS_PATH_MAX];
    vs_exe_dir(exe, sizeof exe);
    snprintf(out, cap, "%s/tools/%s", exe, name);
    if (file_exists(out)) return;
    snprintf(out, cap, "%s/../tools/%s", exe, name);
}

// Runs the bundled converter for TwitchDownloader JSON -> our flat tsv.
static bool convert_json(const char *in, char *out, int outcap, char *err, int errcap) {
    char tool[VS_PATH_MAX];
    tool_path("vs_chat_prep.py", tool, sizeof tool);
    if (!file_exists(tool)) {
        snprintf(err, errcap, "helper not found: %s", tool);
        return false;
    }

    snprintf(out, outcap, "%s.vodsync.tsv", in);

    char cmd[VS_PATH_MAX * 3];
    snprintf(cmd, sizeof cmd, "%s \"%s\" \"%s\" \"%s\"",
             vs_python_cmd(), tool, in, out);

    char *sout = NULL;
    int slen = 0, code = 1;
    char perr[160];
    if (!vs_proc_capture_code(cmd, &sout, &slen, &code, 120000,
                              perr, sizeof perr) && !sout) {
        snprintf(err, errcap, "could not run vs_chat_prep.py");
        return false;
    }
    free(sout);
    if (code != 0) {
        snprintf(err, errcap, "chat conversion failed (exit %d)", code);
        return false;
    }
    return true;
}

static unsigned hex2(const char *p) {
    unsigned v = 0;
    for (int i = 0; i < 2; i++) {
        char ch = p[i];
        v <<= 4;
        if (ch >= '0' && ch <= '9') v |= (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') v |= (unsigned)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') v |= (unsigned)(ch - 'A' + 10);
    }
    return v;
}

bool vs_chat_load(vs_chat *c, const char *path) {
    char use[VS_URL_MAX];
    snprintf(use, sizeof use, "%s", path);
    c->state = VS_LOAD_RESOLVING;

    const char *ext = strrchr(path, '.');
    if (ext && vs_stricmp(ext, ".json") == 0) {
        char err[192];
        if (!convert_json(path, use, sizeof use, err, sizeof err)) {
            snprintf(c->status, sizeof c->status, "%s", err);
            c->state = VS_LOAD_FAILED;
            return false;
        }
    }

    FILE *f = fopen(use, "rb");
    if (!f) {
        snprintf(c->status, sizeof c->status, "cannot open %s", use);
        c->state = VS_LOAD_FAILED;
        return false;
    }

    vs_chat_free(c);
    c->state = VS_LOAD_RESOLVING;
    snprintf(c->path, sizeof c->path, "%s", path);

    char line[2048];
    int bad = 0, version = 1;
    while (fgets(line, sizeof line, f)) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (len == 0) continue;

        if (line[0] == '#') {
            if (strncmp(line, "#vodsync-chat ", 14) == 0) {
                // Always written, never read until now. Reading it is what
                // lets format 2 add columns while every older log keeps
                // loading byte for byte.
                int v = atoi(line + 14);
                if (v > 0) version = v;
            } else if (strncmp(line, "#source ", 8) == 0) {
                parse_source(c, line + 8);
            } else if (strncmp(line, "#start ", 7) == 0) {
                double t = atof(line + 7);
                if (t > 0 && c->pdt_start <= 0) c->pdt_start = t;
            } else if (strncmp(line, "#label ", 7) == 0 && !c->label[0]) {
                snprintf(c->label, sizeof c->label, "%s", line + 7);
            }
            continue;
        }

        // Version 1:  t \t RRGGBB \t user \t text
        // Version 2:  t \t src \t kind \t id \t colour \t user \t emotes \t text
        //
        // Text is the last field in both, and the split stops once the
        // earlier fields are out, so a message may contain a tab.
        int want = (version >= 2) ? 8 : 4;
        char *f[8];
        int nf = 1;
        f[0] = line;
        for (char *p = line; *p && nf < want; p++) {
            if (*p == '\t') { *p = 0; f[nf++] = p + 1; }
        }
        if (nf < want) { bad++; continue; }

        vs_chat_msg m;
        memset(&m, 0, sizeof m);
        m.src = VS_SRC_NONE;
        m.t = atof(f[0]);

        const char *col, *user, *text, *emotes = NULL;
        if (version >= 2) {
            m.src = (unsigned char)source_index(c, f[1]);
            m.kind = (f[2][0] == 'r') ? VS_MSG_REPLY
                   : (f[2][0] == 's') ? VS_MSG_SYSTEM : VS_MSG_NORMAL;
            col = f[4]; user = f[5]; emotes = f[6]; text = f[7];
        } else {
            col = f[1]; user = f[2]; text = f[3];
        }

        snprintf(m.user, sizeof m.user, "%s", user);
        int tlen = (int)snprintf(m.text, sizeof m.text, "%s", text);
        if (tlen > (int)sizeof m.text - 1) tlen = (int)sizeof m.text - 1;

        if (strlen(col) >= 6 && col[0] != '-') {
            m.r = (unsigned char)hex2(col + 0);
            m.g = (unsigned char)hex2(col + 2);
            m.b = (unsigned char)hex2(col + 4);
        } else {
            colour_from_name(m.user, &m.r, &m.g, &m.b);
        }

        // Spans are read against the text as STORED, so one pointing past a
        // truncated message is dropped rather than read out of bounds.
        parse_spans(c, emotes, m.text, tlen, &m);
        push(c, &m);
    }
    fclose(f);

    if (c->n == 0) {
        snprintf(c->status, sizeof c->status, "no messages found in chat log");
        c->state = VS_LOAD_FAILED;
        return false;
    }
    if (!c->label[0]) snprintf(c->label, sizeof c->label, "chat");
    c->cursor = 0;
    c->state  = VS_LOAD_READY;

    // Overlay emotes are matched by NAME against a channel's current sets,
    // so they are fetched now rather than frozen into the log at export
    // time. Once per channel, however many logs mention it.
    for (int i = 0; i < c->nsources; i++) {
        const char *plat =
            c->sources[i].platform == VS_PLAT_TWITCH ? "twitch" :
            c->sources[i].platform == VS_PLAT_KICK   ? "kick"   : NULL;
        if (plat) vs_emote_sets_request(plat, c->sources[i].user_id);
    }
    // `bad` used to be counted and never shown, so a log the parser could
    // not read looked like a log with fewer messages in it.
    char drop[64];
    drop[0] = 0;
    if (bad > 0) snprintf(drop, sizeof drop, ", %d line%s unreadable",
                          bad, bad == 1 ? "" : "s");
    snprintf(c->status, sizeof c->status, "%d messages, %.0f min, v%d%s%s",
             c->n, c->msgs[c->n - 1].t / 60.0, version, drop,
             c->pdt_start > 0 ? "" : " (no anchor yet)");
    return true;
}

double vs_chat_msg_wall(const vs_chat *c, int i) {
    if (i < 0 || i >= c->n) return 0;
    return c->pdt_start + c->msgs[i].t - c->offset;
}

void vs_chat_reseek(vs_chat *c, double wall) {
    if (c->state != VS_LOAD_READY || c->n == 0) return;
    double target = wall - c->pdt_start + c->offset;
    int lo = 0, hi = c->n - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (c->msgs[mid].t <= target) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    c->cursor = best + 1;   // one past the last visible message
}

void vs_chat_advance(vs_chat *c, double wall) {
    if (c->state != VS_LOAD_READY || c->n == 0) return;
    double target = wall - c->pdt_start + c->offset;
    if (c->cursor > 0 && c->msgs[c->cursor - 1].t > target + 0.001) {
        vs_chat_reseek(c, wall);     // went backwards
        return;
    }
    while (c->cursor < c->n && c->msgs[c->cursor].t <= target) c->cursor++;
}

void vs_chat_window(const vs_chat *c, int want, int *first, int *count) {
    int end = c->cursor;
    if (end > c->n) end = c->n;
    int start = end - want;
    if (start < 0) start = 0;
    *first = start;
    *count = end - start;
}

// ------------------------------------------------------- periodic export

typedef struct {
    vs_chat      *c;
    int           src;        // configured source index, or -1 for the
                              // single-VOD flow this file started with
    vs_flag       done;
    bool          ok;
    int           code;
    double        max_offset;
    int           total, added;
    char          out_path[VS_URL_MAX];
    char          cmd[VS_URL_MAX * 2];
    char          msg[256];
} sync_job;

// One per configured source, so a slow export never blocks a fast one.
// This is the only export there is: see vs_chat_source_bind.
static sync_job *g_src_sync[VS_CHAT_SOURCES];

static void sync_worker(void *arg) {
    sync_job *j = (sync_job *)arg;

    char *out = NULL;
    int len = 0, code = 1;
    char perr[160];
    // The export can run for minutes on a long VOD, so the ceiling is
    // generous; the old code waited on the pipe with no limit at all.
    vs_proc_capture_code(j->cmd, &out, &len, &code, 600000, perr, sizeof perr);
    if (!out) {
        snprintf(j->msg, sizeof j->msg, "%s", perr);
        j->code = code;
        vs_flag_set(&j->done);
        return;
    }
    const char *buf = out;
    j->code = code;

    const char *ok = strstr(buf, "OK ");
    if (code == 0 && ok &&
        sscanf(ok + 3, "%lf %d %d", &j->max_offset, &j->total, &j->added) == 3) {
        j->ok = true;
        snprintf(j->msg, sizeof j->msg, "%d messages (+%d)", j->total, j->added);
    } else if (code == 2) {
        snprintf(j->msg, sizeof j->msg, "no chat exporter installed");
    } else {
        const char *tail = len > 180 ? buf + len - 180 : buf;
        snprintf(j->msg, sizeof j->msg, "export failed: %s", tail);
    }
    free(out);
    vs_flag_set(&j->done);
}

bool vs_chat_sync_busy(const vs_chat *c) {
    for (int i = 0; i < VS_CHAT_SOURCES; i++)
        if (g_src_sync[i] && g_src_sync[i]->c == c) return true;
    return false;
}

bool vs_chat_sync_poll(vs_chat *c) {
    sync_job *j = NULL;
    for (int k = 0; k < VS_CHAT_SOURCES; k++) {
        sync_job *q = g_src_sync[k];
        if (q && q->c == c && vs_flag_get(&q->done)) {
            j = q;
            g_src_sync[k] = NULL;
            break;
        }
    }
    if (!j) return false;
    bool ok = j->ok;

    // Every export belongs to a configured source: record what happened to
    // it, then rebuild the merged array from all of them.
    if (j->src >= 0 && j->src < c->nsources) {
        vs_chat_source *s = &c->sources[j->src];
        s->sync_wanted = false;
        s->sync_state = ok ? VS_LOAD_READY : VS_LOAD_FAILED;
        snprintf(s->sync_status, sizeof s->sync_status, "%s", j->msg);
        if (ok) s->exported_to = j->max_offset;
    }
    if (ok) vs_chat_merge(c);
    snprintf(c->sync_status, sizeof c->sync_status, "%s", j->msg);
    c->sync_state = ok ? VS_LOAD_READY : VS_LOAD_FAILED;
    free(j);
    return true;
}

// ===================================================== several chats at once

int vs_chat_source_count(const vs_chat *c) { return c->nsources; }

static void source_path(const vs_chat_source *s, char *out, int cap) {
    char dir[VS_PATH_MAX];
    vs_state_dir(dir, sizeof dir);
    // Named for what it holds rather than for when it was made, so adding a
    // source back finds the log it already has instead of fetching it again.
    const char *key = s->vid[0] ? s->vid : s->channel;
    char safe[80];
    int k = 0;
    for (; key[k] && k < (int)sizeof safe - 1; k++) {
        char ch = key[k];
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        safe[k] = ok ? ch : '_';
    }
    safe[k] = 0;
    snprintf(out, cap, "%s%cchat_%s_%s.tsv", dir, vs_path_sep(),
             s->tag[0] ? s->tag : "x", safe[0] ? safe : "vod");
}

int vs_chat_source_add(vs_chat *c, vs_platform plat, const char *channel,
                       const char *user_id, const char *vod_url,
                       double anchor, double span) {
    if (!vod_url || !vod_url[0]) return -1;
    for (int i = 0; i < c->nsources; i++)
        if (strcmp(c->sources[i].vod_url, vod_url) == 0) return -1;  // already
    if (c->nsources >= VS_CHAT_SOURCES) return -1;

    vs_chat_source *s = &c->sources[c->nsources];
    memset(s, 0, sizeof *s);
    s->platform = plat;
    snprintf(s->tag, sizeof s->tag, "%s", plat == VS_PLAT_KICK ? "kk" : "tw");
    snprintf(s->channel, sizeof s->channel, "%s", channel ? channel : "");
    snprintf(s->user_id, sizeof s->user_id, "%s", user_id ? user_id : "");
    snprintf(s->vod_url, sizeof s->vod_url, "%s", vod_url);
    // The VOD's own id is the last path element on both platforms.
    const char *slash = strrchr(vod_url, '/');
    snprintf(s->vid, sizeof s->vid, "%s", slash ? slash + 1 : "");
    char *frag = strchr(s->vid, '#');       // Kick urls carry a fragment
    if (frag) *frag = 0;
    s->anchor = anchor;
    s->span = span;
    s->avatar = -1;
    s->sync_state = VS_LOAD_EMPTY;
    source_path(s, s->path, sizeof s->path);
    c->nsources++;
    return c->nsources - 1;
}

int vs_chat_source_add_file(vs_chat *c, const char *path) {
    if (!path || !path[0] || c->nsources >= VS_CHAT_SOURCES) return -1;
    vs_chat_source *s = &c->sources[c->nsources];
    memset(s, 0, sizeof *s);
    s->platform = VS_PLAT_UNKNOWN;    // merge fills this in from the file
    s->avatar = -1;
    s->sync_state = VS_LOAD_READY;    // it exists; nothing to export
    snprintf(s->path, sizeof s->path, "%s", path);
    c->nsources++;
    return c->nsources - 1;
}

int vs_chat_source_auto(const vs_chat *c) {
    for (int i = 0; i < c->nsources; i++)
        if (c->sources[i].auto_bound) return i;
    return -1;
}

int vs_chat_source_bind(vs_chat *c, vs_platform plat, const char *channel,
                        const char *user_id, const char *vod_url,
                        double anchor, double span) {
    if (!vod_url || !vod_url[0]) return -1;

    // Already here, however it got here: claim it rather than add a second
    // tile for the same VOD, and fill in anything it was missing.
    for (int i = 0; i < c->nsources; i++)
        if (strcmp(c->sources[i].vod_url, vod_url) == 0) {
            c->sources[i].auto_bound = true;
            if (anchor > 0 && c->sources[i].anchor <= 0)
                c->sources[i].anchor = anchor;
            if (span > 0 && c->sources[i].span <= 0)
                c->sources[i].span = span;
            return i;
        }

    int old = vs_chat_source_auto(c);
    if (old >= 0) {
        // Removing a source shifts every index after it, and a running
        // export holds its source's index - so wait rather than renumber
        // underneath one. The caller comes back next tick.
        if (vs_chat_sync_busy(c)) return -1;
        vs_chat_source_remove(c, old);
        c->manual = false;            // that removal was ours, not the user's
    }

    int k = vs_chat_source_add(c, plat, channel, user_id, vod_url,
                               anchor, span);
    if (k >= 0) {
        c->sources[k].auto_bound = true;
        // The log is named for the VOD, so one may already be here from an
        // earlier run. Read it now: waiting for an export means the tile
        // shows nothing while its messages sit on disk.
        if (c->sources[k].path[0] && file_exists(c->sources[k].path)) {
            c->sources[k].sync_state = VS_LOAD_READY;
            vs_chat_merge(c);
        }
    }
    return k;
}

void vs_chat_source_remove(vs_chat *c, int i) {
    if (i < 0 || i >= c->nsources) return;
    // Taking away the tile that follows the panes is a decision, not a
    // glitch: without this the binder would put it straight back next tick.
    if (c->sources[i].auto_bound) c->manual = true;
    // The exported log stays on disk: re-adding the source then costs
    // nothing, and Reset is what clears state wholesale.
    for (int k = i; k < c->nsources - 1; k++) c->sources[k] = c->sources[k + 1];
    c->nsources--;
    memset(&c->sources[c->nsources], 0, sizeof c->sources[0]);
    vs_chat_merge(c);
}

static int cmp_msg(const void *a, const void *b) {
    double ta = ((const vs_chat_msg *)a)->t;
    double tb = ((const vs_chat_msg *)b)->t;
    return (ta < tb) ? -1 : (ta > tb) ? 1 : 0;
}

void vs_chat_source_nudge(vs_chat *c, int i, double delta) {
    if (i < 0 || i >= c->nsources || delta == 0.0) return;
    c->sources[i].offset += delta;
    // Applied to the stored times, then re-sorted: see the note at the top
    // of this section for why it cannot be done when the time is read.
    for (int m = 0; m < c->n; m++)
        if (c->msgs[m].src == i) c->msgs[m].t -= delta;
    qsort(c->msgs, (size_t)c->n, sizeof(vs_chat_msg), cmp_msg);
}

bool vs_chat_merge(vs_chat *c) {
    if (c->nsources <= 0) return false;

    // Only the messages go; the configured sources are what is being merged
    // FROM and must survive it.
    free(c->msgs);  c->msgs = NULL;  c->n = c->cap = 0;
    free(c->spans); c->spans = NULL; c->nspans = c->spans_cap = 0;
    c->cursor = 0;

    int loaded = 0;
    double base = 0;
    for (int k = 0; k < c->nsources; k++) {
        vs_chat_source *src = &c->sources[k];
        src->nmsgs = 0;
        if (!src->path[0]) continue;

        vs_chat tmp;
        memset(&tmp, 0, sizeof tmp);
        if (!vs_chat_load(&tmp, src->path)) {
            vs_chat_free(&tmp);
            continue;               // not exported yet; not an error here
        }
        if (tmp.pdt_start > 0) src->anchor = tmp.pdt_start;
        // The tag follows the platform wherever the source came from. It is
        // part of the log's file name and of the avatar's cache key, so an
        // empty one points at the wrong file rather than at nothing.
        if (!src->tag[0] && src->platform != VS_PLAT_UNKNOWN)
            snprintf(src->tag, sizeof src->tag, "%s",
                     src->platform == VS_PLAT_KICK ? "kk" : "tw");
        // The file knows what it is. A source added from a URL alone has no
        // channel or user id until its export exists; once it does, the
        // #source line it wrote carries both.
        if (tmp.nsources > 0) {
            const vs_chat_source *hdr = &tmp.sources[0];
            if (!src->channel[0] && hdr->channel[0])
                snprintf(src->channel, sizeof src->channel, "%s", hdr->channel);
            if (!src->user_id[0] && hdr->user_id[0])
                snprintf(src->user_id, sizeof src->user_id, "%s", hdr->user_id);
            if (!src->vid[0] && hdr->vid[0])
                snprintf(src->vid, sizeof src->vid, "%s", hdr->vid);
            if (src->platform == VS_PLAT_UNKNOWN && hdr->platform) {
                src->platform = hdr->platform;
                snprintf(src->tag, sizeof src->tag, "%s", hdr->tag);
            }
        }

        // The first source that loads sets the axis; the rest shift onto it.
        double shift = 0;
        if (loaded == 0) {
            base = tmp.pdt_start;
            c->pdt_start = base;
        } else if (tmp.pdt_start > 0 && base > 0) {
            shift = tmp.pdt_start - base;
        }
        shift -= src->offset;       // the nudge, baked in

        int span_base = c->nspans;
        for (int m = 0; m < tmp.n; m++) {
            vs_chat_msg msg = tmp.msgs[m];
            msg.t += shift;
            msg.src = (unsigned char)k;
            // This source's spans are appended after the ones already held,
            // so each message's range moves with them.
            if (msg.nspan) msg.span_at += span_base;
            push(c, &msg);
        }
        if (tmp.nspans > 0) {
            int want = c->nspans + tmp.nspans;
            if (want > c->spans_cap) {
                c->spans_cap = want < 1024 ? 1024 : want;
                c->spans = (vs_emote_span *)realloc(
                    c->spans, sizeof(vs_emote_span) * c->spans_cap);
            }
            memcpy(c->spans + c->nspans, tmp.spans,
                   sizeof(vs_emote_span) * tmp.nspans);
            c->nspans += tmp.nspans;
        }
        src->nmsgs = tmp.n;
        loaded++;
        vs_chat_free(&tmp);
    }

    if (c->n == 0) {
        c->state = VS_LOAD_EMPTY;
        snprintf(c->status, sizeof c->status, "%d source%s, nothing exported yet",
                 c->nsources, c->nsources == 1 ? "" : "s");
        return false;
    }

    qsort(c->msgs, (size_t)c->n, sizeof(vs_chat_msg), cmp_msg);
    c->state = VS_LOAD_READY;

    if (c->nsources == 1) {
        snprintf(c->label, sizeof c->label, "%s chat",
                 c->sources[0].channel[0] ? c->sources[0].channel : "chat");
    } else {
        snprintf(c->label, sizeof c->label, "%s +%d",
                 c->sources[0].channel[0] ? c->sources[0].channel : "chat",
                 c->nsources - 1);
    }
    snprintf(c->status, sizeof c->status, "%d messages from %d source%s",
             c->n, loaded, loaded == 1 ? "" : "s");
    return true;
}


// --------------------------------------------- exporting one configured source

bool vs_chat_sync_source(vs_chat *c, int i, double from_offset) {
    if (i < 0 || i >= c->nsources || i >= VS_CHAT_SOURCES) return false;
    if (g_src_sync[i]) return false;           // this one is already going
    vs_chat_source *s = &c->sources[i];
    if (!s->vod_url[0]) {
        s->sync_state = VS_LOAD_FAILED;
        snprintf(s->sync_status, sizeof s->sync_status, "no VOD for this source");
        return false;
    }

    char script[VS_PATH_MAX];
    tool_path("vs_chat_sync.py", script, sizeof script);
    if (!file_exists(script)) {
        s->sync_state = VS_LOAD_FAILED;
        snprintf(s->sync_status, sizeof s->sync_status,
                 "chat helper missing: %s", script);
        return false;
    }
    if (!s->path[0]) source_path(s, s->path, sizeof s->path);

    sync_job *j = (sync_job *)calloc(1, sizeof(sync_job));
    j->c = c;
    j->src = i;
    snprintf(j->out_path, sizeof j->out_path, "%s", s->path);

    double since = from_offset > 1 ? from_offset - 1 : 0;
    const char *stub = getenv("VODSYNC_CHAT_STUB") ? " --stub" : "";

    if (s->platform == VS_PLAT_KICK) {
        // Kick's exporter walks a time range rather than fetching a VOD by
        // id, so it needs both ends. Without an anchor its messages - which
        // are stamped with absolute time - have nothing to be offsets from.
        if (s->anchor <= 0) {
            free(j);
            s->sync_state = VS_LOAD_FAILED;
            snprintf(s->sync_status, sizeof s->sync_status,
                     "no start time known for this Kick VOD yet");
            return false;
        }
        snprintf(j->cmd, sizeof j->cmd,
                 "%s \"%s\" --platform kick --channel \"%s\" --vod \"%s\" "
                 "--out \"%s\" --anchor %.3f --span %.0f --since %.0f%s",
                 vs_python_cmd(), script, s->channel, s->vid, j->out_path,
                 s->anchor, s->span > 0 ? s->span : 24 * 3600.0, since, stub);
    } else {
        snprintf(j->cmd, sizeof j->cmd,
                 "%s \"%s\" --vod \"%s\" --out \"%s\" --since %.0f%s",
                 vs_python_cmd(), script, s->vod_url, j->out_path, since, stub);
        if (s->anchor > 0) {
            char anchor[64];
            snprintf(anchor, sizeof anchor, " --anchor %.3f", s->anchor);
            strncat(j->cmd, anchor, sizeof j->cmd - strlen(j->cmd) - 1);
        }
    }

    g_src_sync[i] = j;
    s->sync_wanted = false;
    s->sync_state = VS_LOAD_RESOLVING;
    snprintf(s->sync_status, sizeof s->sync_status,
             from_offset > 1 ? "updating ..." : "exporting ...");
    c->sync_state = VS_LOAD_RESOLVING;
    snprintf(c->sync_status, sizeof c->sync_status, "%s %s ...",
             from_offset > 1 ? "updating" : "exporting",
             s->channel[0] ? s->channel : "chat");

    if (!vs_thread_start(sync_worker, j)) {
        free(j);
        g_src_sync[i] = NULL;
        s->sync_state = VS_LOAD_FAILED;
        snprintf(s->sync_status, sizeof s->sync_status, "could not start export");
        return false;
    }
    return true;
}

bool vs_chat_sync_all(vs_chat *c) {
    if (c->nsources <= 0) return false;
    // All at once. The quick ones then appear while the slow one runs,
    // instead of every chat waiting on the slowest.
    bool any = false;
    for (int i = 0; i < c->nsources; i++)
        if (vs_chat_sync_source(c, i, c->sources[i].exported_to)) any = true;
    return any;
}
