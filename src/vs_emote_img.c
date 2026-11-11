// vodsync - emote images: fetch, cache, decode, upload.
//
// Threading follows the shape the rest of the app uses, because the platform
// layer offers an interlocked set and nothing else - no mutex, no
// compare-and-swap. So:
//
//   * a job slot is handed between threads by a single state word, and only
//     one thread may write it at each step (UI: empty -> pending, worker:
//     pending -> done, UI: done -> empty);
//   * each worker owns a DISJOINT set of slots (slot % workers == me), which
//     is what makes several workers safe without a compare-and-swap to claim
//     one. Sharing a queue would need exactly that.
//
// The job carries its own URL and cache path, filled in by the UI thread.
// That is not laziness about passing an index: vs_emote_url() reads the
// emote table, the table is realloc'd by interning on the UI thread, and a
// worker reading it mid-grow would be a use-after-free.
//
// Decoding happens on the UI thread rather than in the worker. The expensive
// part is the network, which is off-thread; decoding a 64-pixel emote is
// sub-millisecond, and keeping raylib's image functions on one thread avoids
// having to reason about whether they are safe on another.
#include "vs_emote_img.h"
#include "vs_platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define VS_EM_WORKERS   3
#define VS_EM_SLOTS     9            // 3 each, so every slot has one owner
#define VS_EM_PER_PUMP  4            // uploads per frame, to avoid a hitch

// Decoded bytes held on the GPU. Emotes are small, but an animated one is a
// whole strip: 64x64 with 40 frames is 650 KB. When this is reached new
// emotes stop being fetched rather than old ones being evicted - a caller
// may be holding a pointer to a texture, and refusing to grow is honest
// where pulling a texture out from under the renderer is not.
#define VS_EM_BYTE_CAP  (96L * 1024 * 1024)

enum { JOB_EMPTY = 0, JOB_PENDING, JOB_DONE };

// Which list a finished job writes back into.
enum { JOB_FOR_EMOTE = 0, JOB_FOR_URL };

typedef struct {
    vs_flag        state;
    long           gen;
    int            kind;       // JOB_FOR_*
    int            rec;        // url record index, when kind is JOB_FOR_URL
    int            emote;
    int            size;
    char           url[512];
    char           cache[VS_PATH_MAX];
    unsigned char *bytes;      // the worker's result; the UI thread frees it
    int            nbytes;
    bool           from_cache;
} em_job;

enum { REC_NONE = 0, REC_WAITING, REC_READY, REC_FAILED };

typedef struct {
    vs_emote_img  img;
    unsigned char state;
} em_rec;

static em_job  g_jobs[VS_EM_SLOTS];
static vs_flag g_quit;
static bool    g_workers_up;
static long    g_gen = 1;

static em_rec *g_rec;
static int     g_rec_cap;          // records, i.e. emotes * VS_EMSZ_COUNT

// Images named by the caller rather than by emote. Avatars, so far.
#define VS_URLIMG_MAX 16
typedef struct {
    char    key[64];
    char    url[256];       // kept so an unqueued request can be retried
    em_rec  rec;
} url_img;
static url_img g_url[VS_URLIMG_MAX];
static int     g_url_n;
static long    g_bytes;
static int     g_ready, g_failed, g_waiting, g_cached;

// ------------------------------------------------------------- the workers

static void worker(void *arg) {
    int me = (int)(long long)arg;
    for (;;) {
        if (vs_flag_get(&g_quit)) return;
        bool did = false;
        for (int i = me; i < VS_EM_SLOTS; i += VS_EM_WORKERS) {
            em_job *j = &g_jobs[i];
            if (vs_flag_value(&j->state) != JOB_PENDING) continue;

            unsigned char *buf = NULL;
            int n = 0;
            bool cached = false;
            if (j->cache[0] && vs_file_read(j->cache, &buf, &n) && n > 0) {
                cached = true;
            } else {
                free(buf);
                buf = NULL;
                n = 0;
                char *body = NULL;
                int blen = 0;
                char err[160];
                if (vs_http_get(j->url, &body, &blen, err, sizeof err) &&
                    blen > 0) {
                    buf = (unsigned char *)body;
                    n = blen;
                    // Written even though it may be re-read next launch
                    // rather than this one: a five-hour chat log reaches for
                    // the same few hundred emotes every time it is opened.
                    if (j->cache[0]) vs_file_write(j->cache, buf, n, true);
                } else {
                    free(body);
                }
            }

            j->bytes = buf;
            j->nbytes = n;
            j->from_cache = cached;
            vs_flag_set_value(&j->state, JOB_DONE);   // last, as always
            did = true;
        }
        if (!did) vs_sleep_ms(15);
    }
}

static void workers_start(void) {
    if (g_workers_up) return;
    g_workers_up = true;
    for (int i = 0; i < VS_EM_WORKERS; i++)
        vs_thread_start(worker, (void *)(long long)i);
}

// --------------------------------------------------------------- the cache

// <state dir>/emotes/<source><id>. No extension: what the CDNs serve is
// decided by sniffing the bytes, so recording a guessed one in the name
// would only be something else to get wrong.
static bool cache_path(int emote, vs_emote_size size, char *out, int cap) {
    static char dir[VS_PATH_MAX];
    static int tried;
    if (!tried) {
        tried = 1;
        char base[VS_PATH_MAX];
        vs_state_dir(base, sizeof base);
        snprintf(dir, sizeof dir, "%s%cemotes", base, vs_path_sep());
        if (!vs_dir_create(dir)) dir[0] = 0;
    }
    if (!dir[0]) return false;
    const char *src = "x";
    switch (vs_emote_source(emote)) {
        case VS_EM_TWITCH: src = "tw"; break;
        case VS_EM_KICK:   src = "kk"; break;
        case VS_EM_7TV:    src = "7v"; break;
        case VS_EM_BTTV:   src = "bt"; break;
        default: break;
    }
    // The id is used in a file name, so anything that is not plainly safe is
    // replaced. Ids are alphanumeric in practice; this is here so that a
    // platform changing its mind cannot write outside the cache directory.
    char id[VS_EMOTE_ID_MAX];
    const char *p = vs_emote_id(emote);
    int k = 0;
    for (; p[k] && k < (int)sizeof id - 1; k++) {
        char c = p[k];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        id[k] = ok ? c : '_';
    }
    id[k] = 0;
    if (!id[0]) return false;
    snprintf(out, cap, "%s%c%s%s_%d", dir, vs_path_sep(), src, id,
             (int)size);
    return true;
}

// ------------------------------------------------------------- the records

static void rec_reserve(int emote) {
    int want = (emote + 1) * VS_EMSZ_COUNT;
    if (want <= g_rec_cap) return;
    int cap = g_rec_cap ? g_rec_cap : 256;
    while (cap < want) cap *= 2;
    g_rec = (em_rec *)realloc(g_rec, sizeof(em_rec) * cap);
    memset(g_rec + g_rec_cap, 0, sizeof(em_rec) * (cap - g_rec_cap));
    g_rec_cap = cap;
}

static int queue_job(int emote, vs_emote_size size) {
    if (g_bytes >= VS_EM_BYTE_CAP) return 0;
    char url[512];
    // Inline wants a little more than it draws, so it stays sharp as the
    // text scale goes up; the hover wants the biggest the source offers.
    int scale = (size == VS_EMSZ_LARGE) ? 4 : 2;
    if (!vs_emote_url(emote, scale, url, sizeof url)) return 0;

    for (int i = 0; i < VS_EM_SLOTS; i++) {
        em_job *j = &g_jobs[i];
        if (vs_flag_value(&j->state) != JOB_EMPTY) continue;
        j->kind = JOB_FOR_EMOTE;
        j->rec = -1;
        j->emote = emote;
        j->size = (int)size;
        j->gen = g_gen;
        j->bytes = NULL;
        j->nbytes = 0;
        j->from_cache = false;
        snprintf(j->url, sizeof j->url, "%s", url);
        if (!cache_path(emote, size, j->cache, sizeof j->cache))
            j->cache[0] = 0;
        workers_start();
        vs_flag_set_value(&j->state, JOB_PENDING);    // last
        return 1;
    }
    return 0;      // every slot busy; the caller will ask again next frame
}

const vs_emote_img *vs_emote_img_get(int emote, vs_emote_size size) {
    if (emote < 0 || emote >= vs_emote_count()) return NULL;
    if (size < 0 || size >= VS_EMSZ_COUNT) return NULL;
    rec_reserve(emote);
    em_rec *r = &g_rec[emote * VS_EMSZ_COUNT + size];
    if (r->state == REC_READY) return &r->img;
    if (r->state == REC_NONE) {
        if (queue_job(emote, size)) {
            r->state = REC_WAITING;
            g_waiting++;
        }
    }
    return NULL;
}

// ------------------------------------------------------------- the decoding

// What the bytes actually are. BTTV serves no extension at all, a 7TV
// "animated" flag can disagree with the file, and Kick serves gif and png
// from the same path - so the bytes decide, not the name.
static const char *sniff(const unsigned char *b, int n) {
    if (n >= 6 && memcmp(b, "GIF8", 4) == 0)                   return ".gif";
    if (n >= 8 && b[0] == 0x89 && memcmp(b + 1, "PNG", 3) == 0) return ".png";
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8)                return ".jpg";
    if (n >= 2 && b[0] == 'B'  && b[1] == 'M')                 return ".bmp";
    // RIFF....WEBP - which raylib cannot decode. Recognised so it is
    // reported as unsupported rather than as a corrupt file.
    if (n >= 12 && memcmp(b, "RIFF", 4) == 0 &&
        memcmp(b + 8, "WEBP", 4) == 0)                          return NULL;
    return NULL;
}

// raylib's GIF loader returns a frame COUNT and no delays, so the delay is
// read here from the first Graphic Control Extension: 0x21 0xF9 0x04, a
// packed byte, then the delay in hundredths of a second.
//
// Only the first is read. A GIF may give every frame its own delay, and an
// emote that does would animate at the wrong speed in places; that is a
// visible approximation rather than a hidden one, and the alternative is a
// per-frame table raylib gives no way to apply.
static float gif_frame_secs(const unsigned char *b, int n) {
    for (int i = 0; i + 6 < n; i++) {
        if (b[i] == 0x21 && b[i + 1] == 0xF9 && b[i + 2] == 0x04) {
            int cs = b[i + 4] | (b[i + 5] << 8);
            // 0 and 1 mean "as fast as possible", which every browser
            // clamps to 100ms; matching that keeps emotes looking right.
            if (cs < 2) cs = 10;
            return (float)cs / 100.0f;
        }
    }
    return 1.0f / 15.0f;
}

static bool decode_upload(em_job *j, vs_emote_img *out) {
    const char *ext = sniff(j->bytes, j->nbytes);
    if (!ext) return false;

    Image im;
    int frames = 1;
    if (strcmp(ext, ".gif") == 0) {
        im = LoadImageAnimFromMemory(ext, j->bytes, j->nbytes, &frames);
        if (frames < 1) frames = 1;
    } else {
        im = LoadImageFromMemory(ext, j->bytes, j->nbytes);
    }
    if (!im.data || im.width <= 0 || im.height <= 0) {
        if (im.data) UnloadImage(im);
        return false;
    }

    // width and height describe ONE frame; the other frames follow it in
    // .data. Treating height as the whole strip and dividing by the count
    // squashed every animated emote - an 11-frame 320x319 became 320x29 and
    // drew as a banner across the column.
    int fw = im.width, fh = im.height;
    if (fh <= 0 || fw <= 0) { UnloadImage(im); return false; }

    // What the two sizes are actually drawn at, with headroom for the
    // text scale going up. Anything larger is scaled down here rather than
    // held at the source's own resolution.
    int maxh = (j->size == (int)VS_EMSZ_LARGE) ? 160 : 72;
    if (fh > maxh) {
        int nh = maxh;
        int nw = (int)((double)fw * (double)nh / (double)fh + 0.5);
        if (nw < 1) nw = 1;
        // Resize the whole strip in one go: every frame scales the same
        // way and they stay stacked, so nothing downstream changes.
        im.height = fh * frames;
        ImageResize(&im, nw, nh * frames);
        fw = nw;
        fh = nh;
        im.height = fh;
    }

    long cost = (long)fw * fh * 4 * frames;
    if (g_bytes + cost > VS_EM_BYTE_CAP) {
        UnloadImage(im);
        return false;
    }

    // The frames are already contiguous, so the strip needs no copying:
    // claim the full height for the upload and the texture is the strip
    // that vs_emote_img_frame indexes into.
    im.height = fh * frames;
    out->tex = LoadTextureFromImage(im);
    im.height = fh;
    UnloadImage(im);
    if (out->tex.id == 0) return false;
    // Emotes are drawn smaller than they are fetched, so bilinear rather
    // than the default point sampling: at point, a 64px emote at 20px is a
    // mess of dropped pixels.
    SetTextureFilter(out->tex, TEXTURE_FILTER_BILINEAR);

    out->fw = fw;
    out->fh = fh;
    out->frames = frames;
    out->frame_secs = (frames > 1)
                      ? gif_frame_secs(j->bytes, j->nbytes) : 0.0f;
    g_bytes += cost;
    return true;
}

void vs_emote_img_pump(void) {
    int done = 0;
    for (int i = 0; i < VS_EM_SLOTS && done < VS_EM_PER_PUMP; i++) {
        em_job *j = &g_jobs[i];
        if (vs_flag_value(&j->state) != JOB_DONE) continue;

        // A reset renumbered the table while this was in flight, so the
        // emote index it carries no longer means anything.
        bool stale = (j->gen != g_gen);
        if (!stale && j->kind == JOB_FOR_URL) {
            em_rec *r = (j->rec >= 0 && j->rec < g_url_n)
                        ? &g_url[j->rec].rec : NULL;
            if (r) {
                if (r->state == REC_WAITING) g_waiting--;
                if (j->bytes && j->nbytes > 0 && decode_upload(j, &r->img)) {
                    r->state = REC_READY;
                    g_ready++;
                    if (j->from_cache) g_cached++;
                } else {
                    r->state = REC_FAILED;
                    g_failed++;
                }
            }
            free(j->bytes);
            j->bytes = NULL;
            j->nbytes = 0;
            vs_flag_set_value(&j->state, JOB_EMPTY);
            done++;
            continue;
        }
        if (!stale) {
            rec_reserve(j->emote);
            em_rec *r = &g_rec[j->emote * VS_EMSZ_COUNT + j->size];
            if (r->state == REC_WAITING) g_waiting--;
            if (j->bytes && j->nbytes > 0 && decode_upload(j, &r->img)) {
                r->state = REC_READY;
                g_ready++;
                if (j->from_cache) g_cached++;
            } else {
                r->state = REC_FAILED;
                g_failed++;
            }
        }
        free(j->bytes);
        j->bytes = NULL;
        j->nbytes = 0;
        vs_flag_set_value(&j->state, JOB_EMPTY);      // last
        done++;
    }
}

Rectangle vs_emote_img_frame(const vs_emote_img *im, double t) {
    Rectangle r = { 0, 0, 0, 0 };
    if (!im) return r;
    r.width = (float)im->fw;
    r.height = (float)im->fh;
    if (im->frames > 1 && im->frame_secs > 0.0f) {
        int f = (int)(t / (double)im->frame_secs) % im->frames;
        if (f < 0) f += im->frames;
        r.y = (float)(f * im->fh);
    }
    return r;
}

void vs_emote_img_stats(int *ready, int *pending, int *failed, long *bytes,
                        int *cached) {
    if (ready)   *ready = g_ready;
    if (pending) *pending = g_waiting;
    if (failed)  *failed = g_failed;
    if (bytes)   *bytes = g_bytes;
    if (cached)  *cached = g_cached;
}

void vs_emote_img_shutdown(void) {
    // Bumped first: anything already in flight is discarded by pump rather
    // than written into records that are about to be freed.
    g_gen++;
    for (int i = 0; i < g_rec_cap; i++) {
        if (g_rec[i].state == REC_READY && g_rec[i].img.tex.id)
            UnloadTexture(g_rec[i].img.tex);
    }
    for (int i = 0; i < g_url_n; i++)
        if (g_url[i].rec.state == REC_READY && g_url[i].rec.img.tex.id)
            UnloadTexture(g_url[i].rec.img.tex);
    memset(g_url, 0, sizeof g_url);
    g_url_n = 0;
    free(g_rec);
    g_rec = NULL;
    g_rec_cap = 0;
    g_bytes = 0;
    g_ready = g_failed = g_waiting = g_cached = 0;
}


// ------------------------------------------------- images named by the caller

// Tries to put a record's fetch on a slot. False when they are all busy,
// which is not a failure - the caller asks again next frame.
static bool url_try_queue(int h) {
    if (h < 0 || h >= g_url_n) return false;
    if (g_url[h].rec.state != REC_NONE) return true;       // already going
    if (g_bytes >= VS_EM_BYTE_CAP) return false;
    const char *url = g_url[h].url;
    const char *key = g_url[h].key;
    if (!url[0]) return false;

    for (int i = 0; i < VS_EM_SLOTS; i++) {
        em_job *j = &g_jobs[i];
        if (vs_flag_value(&j->state) != JOB_EMPTY) continue;
        j->kind = JOB_FOR_URL;
        j->rec = h;
        j->emote = -1;
        j->size = 0;
        j->gen = g_gen;
        j->bytes = NULL;
        j->nbytes = 0;
        j->from_cache = false;
        snprintf(j->url, sizeof j->url, "%s", url);
        {
            // Same cache directory as the emotes, under the caller's key.
            static char dir[VS_PATH_MAX];
            static int tried;
            if (!tried) {
                tried = 1;
                char base[VS_PATH_MAX];
                vs_state_dir(base, sizeof base);
                snprintf(dir, sizeof dir, "%s%cemotes", base, vs_path_sep());
                if (!vs_dir_create(dir)) dir[0] = 0;
            }
            if (dir[0]) snprintf(j->cache, sizeof j->cache, "%s%c%s",
                                 dir, vs_path_sep(), key);
            else j->cache[0] = 0;
        }
        workers_start();
        g_url[h].rec.state = REC_WAITING;
        g_waiting++;
        vs_flag_set_value(&j->state, JOB_PENDING);
        return true;
    }
    return false;          // every slot busy; try again next frame
}

int vs_img_url(const char *key, const char *url) {
    if (!key || !key[0] || !url || !url[0]) return -1;
    for (int i = 0; i < g_url_n; i++)
        if (strcmp(g_url[i].key, key) == 0) return i;      // already known
    if (g_url_n >= VS_URLIMG_MAX) return -1;

    int h = g_url_n++;
    memset(&g_url[h], 0, sizeof g_url[h]);
    snprintf(g_url[h].key, sizeof g_url[h].key, "%s", key);
    snprintf(g_url[h].url, sizeof g_url[h].url, "%s", url);
    url_try_queue(h);
    return h;
}

const vs_emote_img *vs_img_get(int handle) {
    if (handle < 0 || handle >= g_url_n) return NULL;
    url_img *u = &g_url[handle];
    if (u->rec.state == REC_READY) return &u->rec.img;
    // Still never queued, because the slots were busy when it was asked
    // for. Asking is the retry.
    if (u->rec.state == REC_NONE) url_try_queue(handle);
    return NULL;
}
