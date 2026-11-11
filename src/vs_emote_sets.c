// vodsync - fetching the overlay emote sets. See vs_emote_sets.h.
#include "vs_emote_sets.h"
#include "vs_emote.h"
#include "vs_platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define VS_SET_SLOTS   8
#define VS_SET_CHANNELS 4       // as many as a merged log can declare

enum { SJ_EMPTY = 0, SJ_PENDING, SJ_DONE };
enum { SK_7TV = 0, SK_BTTV };

typedef struct {
    vs_flag  state;
    int      kind;              // which parser the body belongs to
    char     key[48];           // "<platform>:<id>", empty for a global set
    char     url[256];
    char    *body;
    int      nbody;
} set_job;

// One avatar per channel asked for. Small and fixed: a merged chat panel
// holds a handful of sources, not a directory of them.
typedef struct {
    char key[48];
    char url[256];
} avatar_ent;
static avatar_ent g_avatars[VS_SET_CHANNELS * 2];
static int        g_navatars;

static set_job g_jobs[VS_SET_SLOTS];
static vs_flag g_quit;
static bool    g_worker_up;
static bool    g_globals_asked;
static char    g_asked[VS_SET_CHANNELS][48];
static int     g_nasked;
static int     g_fetched, g_failed, g_pending, g_emotes;

// One worker: there are four documents per channel at most, and they are
// fetched once. Several would need a compare-and-swap to claim a slot, which
// the platform layer does not offer.
static void worker(void *arg) {
    (void)arg;
    for (;;) {
        if (vs_flag_get(&g_quit)) return;
        bool did = false;
        for (int i = 0; i < VS_SET_SLOTS; i++) {
            set_job *j = &g_jobs[i];
            if (vs_flag_value(&j->state) != SJ_PENDING) continue;
            char *body = NULL;
            int n = 0;
            char err[160];
            if (vs_http_get(j->url, &body, &n, err, sizeof err) && n > 0) {
                j->body = body;
                j->nbody = n;
            } else {
                free(body);
                j->body = NULL;
                j->nbody = 0;
            }
            vs_flag_set_value(&j->state, SJ_DONE);   // last
            did = true;
        }
        if (!did) vs_sleep_ms(25);
    }
}

// The first `"<field>": "<value>"` at or after `from`. Not a JSON parser:
// it is a scan, and a scan over a nested document finds whichever copy of
// the name comes first. That is the whole reason `from` exists - see
// avatar_of() below.
static bool json_str_field_from(const char *body, int n, int from,
                                const char *field, char *out, int cap) {
    int flen = (int)strlen(field);
    for (int i = from < 0 ? 0 : from; i + flen + 4 < n; i++) {
        if (body[i] != '"' || strncmp(body + i + 1, field, (size_t)flen) != 0)
            continue;
        if (body[i + 1 + flen] != '"') continue;
        int k = i + 2 + flen;
        while (k < n && (body[k] == ' ' || body[k] == ':')) k++;
        if (k >= n || body[k] != '"') continue;
        k++;
        int w = 0;
        while (k < n && body[k] != '"' && w < cap - 1) {
            // Only \/ appears in these urls; anything else escaped means
            // this is not the simple string field assumed, so give up
            // rather than write something half-unescaped.
            if (body[k] == '\\') {
                if (k + 1 < n && body[k + 1] == '/') { k++; continue; }
                return false;
            }
            out[w++] = body[k++];
        }
        out[w] = 0;
        return w > 0;
    }
    return false;
}

// Where the given key's object starts, or -1.
static int json_key_at(const char *body, int n, const char *key) {
    int klen = (int)strlen(key);
    for (int i = 0; i + klen + 2 < n; i++)
        if (body[i] == '"' && strncmp(body + i + 1, key, (size_t)klen) == 0 &&
            body[i + 1 + klen] == '"')
            return i;
    return -1;
}

// The channel's own avatar.
//
// 7TV nests an emote_set before the user, the set has an owner, and the
// owner has an avatar_url of its own - so the first avatar_url in the
// document belongs to whoever made the emote set, not to this channel.
// Reading it from inside "user" is the difference between the right face
// and a stranger's.
static bool avatar_of(const char *body, int n, char *out, int cap) {
    int user_at = json_key_at(body, n, "user");
    if (user_at >= 0 &&
        json_str_field_from(body, n, user_at, "avatar_url", out, cap))
        return true;
    // BetterTTV: one avatar, at the top level.
    return json_str_field_from(body, n, 0, "avatar", out, cap);
}

static void avatar_store(const char *key, const char *url) {
    if (!key[0] || !url[0]) return;
    for (int i = 0; i < g_navatars; i++)
        if (strcmp(g_avatars[i].key, key) == 0) return;     // first one wins
    if (g_navatars >= (int)(sizeof g_avatars / sizeof g_avatars[0])) return;
    snprintf(g_avatars[g_navatars].key, sizeof g_avatars[0].key, "%s", key);
    snprintf(g_avatars[g_navatars].url, sizeof g_avatars[0].url, "%s", url);
    g_navatars++;
}

const char *vs_emote_sets_avatar(const char *platform, const char *user_id) {
    if (!platform || !user_id || !user_id[0]) return "";
    char key[48];
    snprintf(key, sizeof key, "%s:%s", platform, user_id);
    for (int i = 0; i < g_navatars; i++)
        if (strcmp(g_avatars[i].key, key) == 0) return g_avatars[i].url;
    return "";
}

static void queue(int kind, const char *url, const char *key) {
    for (int i = 0; i < VS_SET_SLOTS; i++) {
        set_job *j = &g_jobs[i];
        if (vs_flag_value(&j->state) != SJ_EMPTY) continue;
        j->kind = kind;
        j->body = NULL;
        j->nbody = 0;
        snprintf(j->key, sizeof j->key, "%s", key ? key : "");
        snprintf(j->url, sizeof j->url, "%s", url);
        if (!g_worker_up) {
            g_worker_up = true;
            vs_thread_start(worker, NULL);
        }
        g_pending++;
        vs_flag_set_value(&j->state, SJ_PENDING);    // last
        return;
    }
    // Every slot busy. Dropped rather than retried: these are four one-off
    // documents per channel, and a log declaring more sources than there are
    // slots is not a case worth queueing for.
}

void vs_emote_sets_request(const char *platform, const char *user_id) {
    if (!platform || !user_id || !user_id[0]) return;

    if (!g_globals_asked) {
        g_globals_asked = true;
        queue(SK_7TV,  "https://7tv.io/v3/emote-sets/global", NULL);
        queue(SK_BTTV, "https://api.betterttv.net/3/cached/emotes/global", NULL);
    }

    char key[48];
    snprintf(key, sizeof key, "%s:%s", platform, user_id);
    for (int i = 0; i < g_nasked; i++)
        if (strcmp(g_asked[i], key) == 0) return;      // already done
    if (g_nasked < VS_SET_CHANNELS)
        snprintf(g_asked[g_nasked++], sizeof g_asked[0], "%s", key);

    char url[256];
    // 7TV keys a channel by the platform's user id and supports both.
    snprintf(url, sizeof url, "https://7tv.io/v3/users/%s/%s",
             platform, user_id);
    queue(SK_7TV, url, key);

    // BetterTTV's channel endpoint only knows twitch (and youtube); there is
    // no Kick provider, so a Kick channel gets BTTV's global set and nothing
    // more. Asking anyway would just be a 404 to explain later.
    if (strcmp(platform, "twitch") == 0) {
        snprintf(url, sizeof url,
                 "https://api.betterttv.net/3/cached/users/twitch/%s",
                 user_id);
        queue(SK_BTTV, url, key);
    }
}

void vs_emote_sets_pump(void) {
    for (int i = 0; i < VS_SET_SLOTS; i++) {
        set_job *j = &g_jobs[i];
        if (vs_flag_value(&j->state) != SJ_DONE) continue;
        if (j->body && j->nbody > 0 && j->key[0]) {
            // Picked up on the way past: 7TV calls it avatar_url, BTTV
            // calls it avatar, and whichever document arrives first wins.
            char av[256];
            if (avatar_of(j->body, j->nbody, av, sizeof av))
                avatar_store(j->key, av);
        }
        if (j->body && j->nbody > 0) {
            int added = (j->kind == SK_7TV)
                        ? vs_emote_add_7tv(j->body, j->nbody)
                        : vs_emote_add_bttv(j->body, j->nbody);
            if (added >= 0) {
                g_fetched++;
                g_emotes += added;
            } else {
                // Reached, parsed, and made no sense - worth telling apart
                // from never having arrived.
                g_failed++;
            }
        } else {
            g_failed++;
        }
        free(j->body);
        j->body = NULL;
        j->nbody = 0;
        if (g_pending > 0) g_pending--;
        vs_flag_set_value(&j->state, SJ_EMPTY);      // last
    }
}

void vs_emote_sets_status(int *fetched, int *pending, int *failed,
                          int *emotes) {
    if (fetched) *fetched = g_fetched;
    if (pending) *pending = g_pending;
    if (failed)  *failed = g_failed;
    if (emotes)  *emotes = g_emotes;
}
