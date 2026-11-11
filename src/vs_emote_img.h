// vodsync - emote images: fetching, caching, and the texture to draw.
//
// Kept apart from vs_emote.c on purpose. That file is the table - identity
// only, no network, no GPU - which is what lets it be tested headlessly.
// This one needs raylib and the network, so it is where that cost is paid.
//
// Nothing here draws. It answers one question - "is there a texture for this
// emote yet?" - and starts the work if there is not, so a caller can ask
// every frame and get a picture as soon as one exists.
#ifndef VS_EMOTE_IMG_H
#define VS_EMOTE_IMG_H
#include "vs_emote.h"
#include "raylib.h"

// Two sizes are kept per emote: the one drawn inline in chat, and a larger
// one for the hover. Asking for a big view of a small texture looks soft,
// and fetching every emote at the large size to avoid that would hold far
// more memory than the inline use needs.
typedef enum {
    VS_EMSZ_INLINE = 0,
    VS_EMSZ_LARGE,
    VS_EMSZ_COUNT
} vs_emote_size;

// An animated emote decodes to one texture holding every frame stacked
// vertically, which is what raylib's GIF loader produces. Drawing a frame is
// therefore a source rectangle rather than a texture swap.
typedef struct {
    Texture2D tex;
    int       fw, fh;        // one frame
    int       frames;
    float     frame_secs;    // how long each frame shows
} vs_emote_img;

// The image for an emote at one of the two sizes, or NULL if there is not
// one yet. A miss queues the fetch, so the caller simply asks again next
// frame rather than managing the request itself.
const vs_emote_img *vs_emote_img_get(int emote, vs_emote_size size);

// Which part of the strip to draw at app time `t`. Static emotes return the
// whole texture, so callers need no special case.
Rectangle vs_emote_img_frame(const vs_emote_img *im, double t);

// Takes whatever the workers have finished and turns it into textures. Must
// run on the thread that owns the GL context, once a frame.
void vs_emote_img_pump(void);

// Drops every texture and stops the workers. For --reset and shutdown.
void vs_emote_img_shutdown(void);

// ---- images that are not emotes ----
//
// Same fetch, cache, decode and upload; a different way of naming them. The
// key is both the cache file name and the identity, so asking twice for one
// key returns the same image however many callers there are.
//
// Returns a handle, or -1 if there is no room. A handle is valid until
// shutdown; vs_img_get returns NULL until the image arrives, so a caller
// simply asks again next frame.
int vs_img_url(const char *key, const char *url);
const vs_emote_img *vs_img_get(int handle);

// ready/pending/failed counts and the decoded bytes held, for the status
// line and for tests to assert against. `cached` is how many of the ready
// ones came off disk rather than the network, which is the only way to tell
// a working cache from one that silently refetches on every launch.
void vs_emote_img_stats(int *ready, int *pending, int *failed, long *bytes,
                        int *cached);

#endif
