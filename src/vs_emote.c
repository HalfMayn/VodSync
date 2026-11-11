// vodsync - the emote table. See vs_emote.h for why there are two ways in.
//
// This file holds identity only: which emotes exist, where each came from,
// and what it is called. Nothing here touches the network or the GPU, so it
// can be exercised headlessly - which matters, because the interesting bugs
// are in the matching rules rather than in the drawing.
#include "vs_emote.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    vs_emote_src   src;
    char           id[VS_EMOTE_ID_MAX];
    char           name[VS_EMOTE_NAME_MAX];
    vs_emote_state state;
    bool           animated;
} vs_emote;

static vs_emote *g_em;
static int       g_n, g_cap;

// Two open-addressed maps over the same array, because the two lookups ask
// different questions: "this source's id" when reading a log, and "this
// word" when rendering. Both store index+1, so 0 means empty.
static int *g_by_id,   g_id_cap;
static int *g_by_name, g_name_cap;

static unsigned long long fnv(const char *s, int len) {
    unsigned long long h = 1469598103934665603ULL;
    for (int i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// ASCII-insensitive, which is what emote names are. Not locale-aware on
// purpose: a Turkish locale must not change whether "KEKW" matches.
static int ieq(const char *a, const char *b, int blen) {
    int i = 0;
    for (; i < blen; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x) return 0;
    }
    return a[i] == 0;
}

static unsigned long long ihash(const char *s, int len) {
    unsigned long long h = 1469598103934665603ULL;
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        h ^= (unsigned char)c;
        h *= 1099511628211ULL;
    }
    return h;
}

static bool name_is_key(vs_emote_src s) {
    return s == VS_EM_7TV || s == VS_EM_BTTV;
}

// ---------------------------------------------------------------- the maps

static void id_put(int idx);
static void name_put(int idx);

static void rehash_id(int want) {
    int cap = 64;
    while (cap < want * 3) cap *= 2;
    free(g_by_id);
    g_by_id = (int *)calloc(cap, sizeof(int));
    g_id_cap = cap;
    for (int i = 0; i < g_n; i++) id_put(i);
}

static void rehash_name(int want) {
    int cap = 64;
    while (cap < want * 3) cap *= 2;
    free(g_by_name);
    g_by_name = (int *)calloc(cap, sizeof(int));
    g_name_cap = cap;
    for (int i = 0; i < g_n; i++)
        if (name_is_key(g_em[i].src) && g_em[i].name[0]) name_put(i);
}

static void id_put(int idx) {
    unsigned long long h = fnv(g_em[idx].id, (int)strlen(g_em[idx].id));
    h = h * 31 + (unsigned)g_em[idx].src;
    int m = g_id_cap - 1;
    int p = (int)(h & m);
    while (g_by_id[p]) p = (p + 1) & m;
    g_by_id[p] = idx + 1;
}

static void name_put(int idx) {
    unsigned long long h = ihash(g_em[idx].name, (int)strlen(g_em[idx].name));
    int m = g_name_cap - 1;
    int p = (int)(h & m);
    while (g_by_name[p]) p = (p + 1) & m;
    g_by_name[p] = idx + 1;
}

static int id_get(vs_emote_src src, const char *id) {
    if (!g_by_id) return -1;
    unsigned long long h = fnv(id, (int)strlen(id));
    h = h * 31 + (unsigned)src;
    int m = g_id_cap - 1;
    int p = (int)(h & m);
    while (g_by_id[p]) {
        int i = g_by_id[p] - 1;
        if (g_em[i].src == src && strcmp(g_em[i].id, id) == 0) return i;
        p = (p + 1) & m;
    }
    return -1;
}

// ------------------------------------------------------------------- public

int vs_emote_intern(vs_emote_src src, const char *id, const char *name) {
    if (src < 0 || src >= VS_EM_SRC_COUNT || !id || !id[0]) return -1;

    int found = id_get(src, id);
    if (found >= 0) {
        // A later sighting may know the name when the first did not - a 7TV
        // set gives both, a bare id does not.
        if (name && name[0] && !g_em[found].name[0]) {
            snprintf(g_em[found].name, sizeof g_em[found].name, "%s", name);
            if (name_is_key(src)) {
                if (!g_by_name) rehash_name(g_n + 1);
                else name_put(found);
            }
        }
        return found;
    }

    if (g_n == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 256;
        g_em = (vs_emote *)realloc(g_em, sizeof(vs_emote) * g_cap);
        rehash_id(g_cap);
        rehash_name(g_cap);
    }
    if (!g_by_id) rehash_id(64);
    if (!g_by_name) rehash_name(64);

    int i = g_n++;
    memset(&g_em[i], 0, sizeof g_em[i]);
    g_em[i].src = src;
    g_em[i].state = VS_EM_UNKNOWN;
    snprintf(g_em[i].id, sizeof g_em[i].id, "%s", id);
    if (name && name[0])
        snprintf(g_em[i].name, sizeof g_em[i].name, "%s", name);
    id_put(i);
    if (name_is_key(src) && g_em[i].name[0]) name_put(i);
    return i;
}

int vs_emote_find_name(const char *name, int len) {
    if (!name || !g_by_name) return -1;
    if (len <= 0) len = (int)strlen(name);
    if (len <= 0 || len >= VS_EMOTE_NAME_MAX) return -1;
    unsigned long long h = ihash(name, len);
    int m = g_name_cap - 1;
    int p = (int)(h & m);
    while (g_by_name[p]) {
        int i = g_by_name[p] - 1;
        // name_is_key is checked again rather than trusted: only overlay
        // emotes are ever put in this map, and a platform emote reaching it
        // would mean any matching word started drawing a picture.
        if (name_is_key(g_em[i].src) && ieq(g_em[i].name, name, len))
            return i;
        p = (p + 1) & m;
    }
    return -1;
}

vs_emote_src vs_emote_source(int i) {
    return (i >= 0 && i < g_n) ? g_em[i].src : VS_EM_TWITCH;
}

const char *vs_emote_id(int i) {
    return (i >= 0 && i < g_n) ? g_em[i].id : "";
}

const char *vs_emote_name(int i) {
    return (i >= 0 && i < g_n) ? g_em[i].name : "";
}

int vs_emote_count(void) { return g_n; }

void vs_emote_reset(void) {
    free(g_em);     g_em = NULL;     g_n = g_cap = 0;
    free(g_by_id);  g_by_id = NULL;  g_id_cap = 0;
    free(g_by_name); g_by_name = NULL; g_name_cap = 0;
}


// ===================================================== a very small JSON reader
//
// Enough to walk the two shapes these two APIs return, and nothing more: no
// tree, no validation, no numbers that are not asked for. The only part that
// is genuinely easy to get wrong is skipping a value without being fooled by
// a bracket inside a string, so that is the part written carefully.
//
// Deliberately not a dependency. The alternative was adding a JSON library
// to a project that has none, to read two fields out of two endpoints.

typedef struct { const char *s; int n, i; } js;

static void js_ws(js *j) {
    while (j->i < j->n) {
        char c = j->s[j->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') j->i++;
        else break;
    }
}

static void utf8_put(char *out, int cap, int *k, unsigned cp) {
    if (cp < 0x80) {
        if (*k + 1 < cap) out[(*k)++] = (char)cp;
    } else if (cp < 0x800) {
        if (*k + 2 < cap) {
            out[(*k)++] = (char)(0xC0 | (cp >> 6));
            out[(*k)++] = (char)(0x80 | (cp & 0x3F));
        }
    } else {
        if (*k + 3 < cap) {
            out[(*k)++] = (char)(0xE0 | (cp >> 12));
            out[(*k)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[(*k)++] = (char)(0x80 | (cp & 0x3F));
        }
    }
}

// At a '"'. Copies the string with escapes resolved. Always consumes the
// whole string even when `out` fills up, so the caller's position stays
// valid - a truncated name is survivable, a desynced parser is not.
static bool js_str(js *j, char *out, int cap) {
    js_ws(j);
    if (j->i >= j->n || j->s[j->i] != '"') return false;
    j->i++;
    int k = 0;
    while (j->i < j->n) {
        char c = j->s[j->i++];
        if (c == '"') break;
        if (c != '\\') {
            if (k + 1 < cap) out[k++] = c;
            continue;
        }
        if (j->i >= j->n) break;
        char e = j->s[j->i++];
        switch (e) {
            case 'n': if (k + 1 < cap) out[k++] = '\n'; break;
            case 't': if (k + 1 < cap) out[k++] = '\t'; break;
            case 'r': if (k + 1 < cap) out[k++] = '\r'; break;
            case 'b': if (k + 1 < cap) out[k++] = '\b'; break;
            case 'f': if (k + 1 < cap) out[k++] = '\f'; break;
            case 'u': {
                unsigned cp = 0;
                for (int d = 0; d < 4 && j->i < j->n; d++) {
                    char h = j->s[j->i++];
                    cp <<= 4;
                    if (h >= '0' && h <= '9')      cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                }
                // A surrogate half on its own becomes U+FFFD rather than a
                // malformed byte sequence.
                if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
                utf8_put(out, cap, &k, cp);
                break;
            }
            default: if (k + 1 < cap) out[k++] = e; break;   // " \ / and the rest
        }
    }
    if (cap > 0) out[k < cap ? k : cap - 1] = 0;
    return true;
}

// Skips exactly one value, whatever it is.
static bool js_skip(js *j) {
    js_ws(j);
    if (j->i >= j->n) return false;
    char c = j->s[j->i];
    if (c == '"') {
        char dump[8];
        return js_str(j, dump, (int)sizeof dump);
    }
    if (c == '{' || c == '[') {
        int depth = 0;
        bool instr = false;
        while (j->i < j->n) {
            char d = j->s[j->i++];
            if (instr) {
                if (d == '\\') j->i++;          // skip the escaped character
                else if (d == '"') instr = false;
                continue;
            }
            if (d == '"') instr = true;
            else if (d == '{' || d == '[') depth++;
            else if (d == '}' || d == ']') {
                depth--;
                if (depth == 0) return true;
            }
        }
        return false;
    }
    // A bare literal: number, true, false, null.
    while (j->i < j->n) {
        char d = j->s[j->i];
        if (d == ',' || d == '}' || d == ']' ||
            d == ' ' || d == '\n' || d == '\r' || d == '\t') break;
        j->i++;
    }
    return true;
}

static bool js_bool(js *j, bool *out) {
    js_ws(j);
    if (j->i < j->n && j->s[j->i] == 't') { *out = true;  return js_skip(j); }
    if (j->i < j->n && j->s[j->i] == 'f') { *out = false; return js_skip(j); }
    *out = false;
    return js_skip(j);
}

// Positions the reader just after the '[' of the array at `key`, searching
// from the current position. Both of 7TV's shapes contain exactly one
// "emotes" array - the global set has it at the top level, a user response
// nests it under "emote_set" - so finding the key by name is enough and
// saves walking the whole document structurally.
static bool js_find_array(js *j, const char *key) {
    int klen = (int)strlen(key);
    while (j->i < j->n) {
        if (j->s[j->i] == '"' && j->i + klen + 2 < j->n &&
            strncmp(j->s + j->i + 1, key, (size_t)klen) == 0 &&
            j->s[j->i + 1 + klen] == '"') {
            j->i += klen + 2;
            js_ws(j);
            if (j->i < j->n && j->s[j->i] == ':') {
                j->i++;
                js_ws(j);
                if (j->i < j->n && j->s[j->i] == '[') { j->i++; return true; }
            }
            continue;
        }
        j->i++;
    }
    return false;
}

// ======================================================================= 7TV

int vs_emote_add_7tv(const char *json, int len) {
    if (!json) return -1;
    if (len <= 0) len = (int)strlen(json);
    js j = { json, len, 0 };
    if (!js_find_array(&j, "emotes")) return -1;

    int added = 0;
    for (;;) {
        js_ws(&j);
        if (j.i >= j.n) break;
        if (j.s[j.i] == ']') break;
        if (j.s[j.i] == ',') { j.i++; continue; }
        if (j.s[j.i] != '{') break;
        j.i++;                                  // into the emote object

        char id[VS_EMOTE_ID_MAX] = { 0 };
        char name[VS_EMOTE_NAME_MAX] = { 0 };
        bool animated = false;

        // Fields of the emote itself. Walked by key rather than by position,
        // so the order the API happens to use does not matter - and the
        // nested "data" object carries an id and a name of its own, which
        // taking the first match in the text would have picked up instead.
        for (;;) {
            js_ws(&j);
            if (j.i >= j.n || j.s[j.i] == '}') { j.i++; break; }
            if (j.s[j.i] == ',') { j.i++; continue; }
            char key[32];
            if (!js_str(&j, key, sizeof key)) break;
            js_ws(&j);
            if (j.i < j.n && j.s[j.i] == ':') j.i++;
            if (strcmp(key, "id") == 0) {
                js_str(&j, id, sizeof id);
            } else if (strcmp(key, "name") == 0) {
                // The alias in THIS set, which is what chat types.
                js_str(&j, name, sizeof name);
            } else if (strcmp(key, "data") == 0) {
                js_ws(&j);
                if (j.i < j.n && j.s[j.i] == '{') {
                    j.i++;
                    for (;;) {
                        js_ws(&j);
                        if (j.i >= j.n || j.s[j.i] == '}') { j.i++; break; }
                        if (j.s[j.i] == ',') { j.i++; continue; }
                        char dk[32];
                        if (!js_str(&j, dk, sizeof dk)) break;
                        js_ws(&j);
                        if (j.i < j.n && j.s[j.i] == ':') j.i++;
                        if (strcmp(dk, "animated") == 0) js_bool(&j, &animated);
                        else js_skip(&j);
                    }
                } else {
                    js_skip(&j);
                }
            } else {
                js_skip(&j);
            }
        }

        if (id[0] && name[0]) {
            int e = vs_emote_intern(VS_EM_7TV, id, name);
            if (e >= 0) {
                vs_emote_set_animated(e, animated);
                added++;
            }
        }
    }
    return added;
}

// ====================================================================== BTTV

static int bttv_array(js *j) {
    int added = 0;
    for (;;) {
        js_ws(j);
        if (j->i >= j->n) break;
        if (j->s[j->i] == ']') { j->i++; break; }
        if (j->s[j->i] == ',') { j->i++; continue; }
        if (j->s[j->i] != '{') break;
        j->i++;

        char id[VS_EMOTE_ID_MAX] = { 0 };
        char code[VS_EMOTE_NAME_MAX] = { 0 };
        char type[16] = { 0 };
        bool animated = false;

        for (;;) {
            js_ws(j);
            if (j->i >= j->n || j->s[j->i] == '}') { j->i++; break; }
            if (j->s[j->i] == ',') { j->i++; continue; }
            char key[32];
            if (!js_str(j, key, sizeof key)) break;
            js_ws(j);
            if (j->i < j->n && j->s[j->i] == ':') j->i++;
            if (strcmp(key, "id") == 0)             js_str(j, id, sizeof id);
            else if (strcmp(key, "code") == 0)      js_str(j, code, sizeof code);
            else if (strcmp(key, "imageType") == 0) js_str(j, type, sizeof type);
            else if (strcmp(key, "animated") == 0)  js_bool(j, &animated);
            else                                    js_skip(j);
        }

        if (id[0] && code[0]) {
            int e = vs_emote_intern(VS_EM_BTTV, id, code);
            if (e >= 0) {
                // imageType is the authority; `animated` is not on every
                // entry, and a gif is animated whether or not it says so.
                vs_emote_set_animated(e, animated || strcmp(type, "gif") == 0);
                added++;
            }
        }
    }
    return added;
}

int vs_emote_add_bttv(const char *json, int len) {
    if (!json) return -1;
    if (len <= 0) len = (int)strlen(json);

    js probe = { json, len, 0 };
    js_ws(&probe);
    if (probe.i < probe.n && probe.s[probe.i] == '[') {
        probe.i++;
        return bttv_array(&probe);              // the global list
    }

    // A user response: two arrays, and a channel may have either or both.
    int total = 0;
    bool any = false;
    for (int pass = 0; pass < 2; pass++) {
        js j = { json, len, 0 };
        if (js_find_array(&j, pass == 0 ? "channelEmotes" : "sharedEmotes")) {
            total += bttv_array(&j);
            any = true;
        }
    }
    return any ? total : -1;
}

// ================================================================= CDN layout

bool vs_emote_animated(int i) {
    return (i >= 0 && i < g_n) ? g_em[i].animated : false;
}

void vs_emote_set_animated(int i, bool animated) {
    if (i >= 0 && i < g_n) g_em[i].animated = animated;
}

bool vs_emote_url(int i, int scale, char *out, int cap) {
    if (i < 0 || i >= g_n || !out || cap <= 0) return false;
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    const char *id = g_em[i].id;

    // Four layouts, and the file extensions below are only hints: the loader
    // sniffs the magic bytes instead of trusting them, because BTTV serves
    // no extension at all and an "animated" flag can be wrong.
    switch (g_em[i].src) {
        case VS_EM_TWITCH:
            if (scale > 3) scale = 3;           // 1.0, 2.0, 3.0 only
            snprintf(out, cap,
                     "https://static-cdn.jtvnw.net/emoticons/v2/%s"
                     "/default/light/%d.0", id, scale);
            return true;
        case VS_EM_KICK:
            // One size, and no choice of format.
            snprintf(out, cap, "https://files.kick.com/emotes/%s/fullsize", id);
            return true;
        case VS_EM_7TV:
            snprintf(out, cap, "https://cdn.7tv.app/emote/%s/%dx.%s",
                     id, scale, g_em[i].animated ? "gif" : "png");
            return true;
        case VS_EM_BTTV:
            if (scale == 4) scale = 3;          // 1x, 2x, 3x
            snprintf(out, cap, "https://cdn.betterttv.net/emote/%s/%dx",
                     id, scale);
            return true;
        default:
            return false;
    }
}
