// vodsync - downloading a VOD to disk, and syncing one played back from there.
//
// This is an addition, not a replacement. Streaming a VOD from its manifest
// is untouched; a download is a second way to get at the same recording, for
// when you would rather not depend on the network or on Twitch's CDN urls
// staying valid.
//
// The one hard problem here is the anchor. Every pane in this app is aligned
// by wall-clock time: vs_pos_for_wall maps a moment to a position using
// pdt_start and, for a recording that stopped and restarted, the whole
// per-segment pdt table. Those come from EXT-X-PROGRAM-DATE-TIME in the HLS
// manifest, and a downloaded media file carries none of it - the parser is
// explicit that a manifest without PROGRAM-DATE-TIME "cannot align". So a
// download that saved only the video would produce a file this app can play
// and cannot sync, which is the one thing it is for.
//
// Hence the sidecar: every download writes a small text file beside the
// media holding the anchor it was fetched with. Loading a saved VOD fills
// pdt_start and segs from that file instead of from a manifest, and the rest
// of the app cannot tell the difference.
//
// Two things make this cheaper than it looks. mpv already plays local files
// (the .ffconcat path for a recording in several sections proves it), so
// pointing a pane at saved media is a matter of what goes in s->manifest.
// And seg.pos is already a position in the CONCATENATED timeline, which is
// exactly what a downloaded file's timeline is, so the segment table carries
// over without translation.
#ifndef VS_DOWNLOAD_H
#define VS_DOWNLOAD_H
#include "vs_app.h"
#include "vs_platform.h"

#define VS_MAX_DL      16    // downloads tracked at once, running and queued
#define VS_DL_PARALLEL  2    // how many actually run; the rest wait their turn

// The limit in force. VS_DL_PARALLEL unless VODSYNC_DL_PARALLEL says
// otherwise, which is how the queue's behaviour is observed rather than
// assumed - see the note in vs_dl_parallel.
int vs_dl_parallel(void);

// The sidecar's own version. Read refuses a number it does not know rather
// than guessing at fields, because a wrong anchor is worse than no anchor:
// it syncs confidently to the wrong moment.
#define VS_DL_META_VER 1

typedef enum {
    VS_DL_NONE = 0,
    VS_DL_QUEUED,      // waiting for a slot
    VS_DL_ANCHOR,      // resolving the manifest, to capture pdt + segments
    VS_DL_RUNNING,     // yt-dlp is fetching media
    VS_DL_DONE,
    VS_DL_FAILED,
    VS_DL_CANCELLED
} vs_dl_state;

typedef struct {
    vs_dl_state state;
    vs_platform platform;
    char        id[96];
    char        channel[96];
    char        title[220];
    char        page_url[VS_URL_MAX];
    char        media[VS_PATH_MAX];     // absolute path of the .ts
    char        meta[VS_PATH_MAX];      // absolute path of its sidecar
    char        status[192];
    int         height;                 // quality asked for; 0 = best there is
    // What will actually arrive: the lower of the request and what the
    // source offers. A 720p recording cannot give you the 1080p you asked
    // for, and the estimate should not pretend otherwise.
    int         vheight;
    double      dur;                    // seconds, from the listing
    double      est_bytes;              // what the tile promised the user
    double      got_bytes;              // yt-dlp's own figure
    double      pct;                    // 0..100, likewise
    double      started;                // unix seconds, for a rate
    double      rate;                   // bytes/sec, smoothed
    // The anchor, captured in VS_DL_ANCHOR before any media is fetched. A
    // download whose anchor never resolved is abandoned rather than saved:
    // see the note at the top of this file.
    double      pdt_start;
    vs_seg     *segs;
    int         nsegs;
    // Resolution borrows the ordinary source machinery rather than
    // reimplementing the manifest walk, Kick's discontinuities included.
    // Never handed to the player - it exists for its parse results.
    vs_source  *probe;
    struct vs_proc *proc;               // NULL unless state is VS_DL_RUNNING
    // Progress arrives in whatever sized pieces the pipe hands over, which
    // do not line up with lines. Whatever follows the last newline waits
    // here for the next read rather than being parsed as a short line.
    char        obuf[1024];
    int         olen;
} vs_dl;

typedef struct {
    char dir[VS_PATH_MAX];
    // Wanted height per platform, indexed by vs_platform. 0 means take the
    // best on offer. Streaming is deliberately NOT governed by this: it
    // keeps its own cap, so turning quality down to save disk cannot
    // quietly make live playback worse.
    int  height[3];
} vs_dl_settings;

typedef struct {
    vs_dl          jobs[VS_MAX_DL];
    int            n;
    vs_dl_settings set;
    char           status[192];
} vs_downloads;

// ---- settings -----------------------------------------------------------
// Fills in the defaults: the user's videos folder and 1080p per platform.
// VODSYNC_DOWNLOAD_DIR wins, which is how the tests keep their downloads
// away from the real folder.
void vs_dl_defaults(vs_dl_settings *set);
bool vs_dl_settings_save(const vs_dl_settings *set);
void vs_dl_settings_load(vs_dl_settings *set);

// The VOD id a page url names: its last path segment, minus any fragment.
// The same rule the chat exporter uses on the same urls, so a VOD's chat log
// and its download agree about what it is called. False when the url names
// no VOD at all, a channel page for instance.
bool vs_dl_id_from_url(const char *url, char *out, int cap);

// ---- what is already here -----------------------------------------------
// The media file a saved VOD would live in, named from its platform and id
// the way chat logs are. Content-derived on purpose: it means a second run
// finds the first run's download without an index to go stale.
void vs_dl_paths(const vs_dl_settings *set, vs_platform plat, const char *id,
                 char *media, int mcap, char *meta, int mecap);
// True when both the media and a sidecar this build understands are present.
// Media without a sidecar is reported through `why` as playable-but-unsynced
// rather than as absent, because the fix is to rebuild the sidecar, not to
// download several gigabytes again.
typedef enum { VS_SAVED_NO = 0, VS_SAVED_YES, VS_SAVED_NO_ANCHOR } vs_saved;
vs_saved vs_dl_saved(const vs_dl_settings *set, vs_platform plat,
                     const char *id, double *bytes);

// ---- estimating ---------------------------------------------------------
// Bytes a VOD is likely to take at `height`, deliberately generous: an
// over-estimate costs a little unused disk, an under-estimate strands a
// download most of the way through.
double vs_dl_estimate(int height, double dur);

// ---- the queue ----------------------------------------------------------
// Add a download. Returns its index, or -1 if it is already queued, already
// on disk, or there is no room. `dur` may be 0 when the listing has not said.
int  vs_dl_add(vs_downloads *d, vs_platform plat, const char *id,
               const char *channel, const char *title, const char *page_url,
               double dur);
int  vs_dl_find(const vs_downloads *d, vs_platform plat, const char *id);
// Stops a running download and leaves its partial file alone, so starting it
// again resumes rather than begins.
void vs_dl_cancel(vs_downloads *d, int i);
void vs_dl_forget(vs_downloads *d, int i);   // drop a finished row
// Drive every job one step: start what can start, read progress, finish what
// has finished. Called once a frame. True when something changed and the UI
// should redraw.
bool vs_dl_poll(vs_downloads *d);
int  vs_dl_active(const vs_downloads *d);    // queued or running

// ---- the sidecar --------------------------------------------------------
bool vs_dl_meta_write(const vs_dl *j);
// Fills the anchor fields of `s` from a sidecar: pdt_start, the segment
// table, duration, height, channel. Leaves s->manifest alone - the caller
// decides what to play.
bool vs_dl_meta_read(const char *path, vs_source *s, char *err, int errcap);

// ---- playing one --------------------------------------------------------
// Point a pane at saved media: reads the sidecar for the anchor, sets
// s->manifest to the media file, and marks the source ready to load. This is
// the whole of "use the saved copy" - everything downstream, the timeline,
// the nudge, the chat alignment, works on pdt_start and segs as before.
bool vs_dl_load_saved(const vs_dl_settings *set, vs_source *s,
                      vs_platform plat, const char *id,
                      char *err, int errcap);

#endif
