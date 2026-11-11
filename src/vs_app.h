// vodsync - core data model and sync engine.
#ifndef VS_APP_H
#define VS_APP_H
// The model deliberately does not include raylib or mpv headers: the GPU-side
// pane state lives behind an opaque pointer owned by vs_player.c, so this file
// (and the sync engine) compile standalone for headless tests.
#include <stdbool.h>
#include "vs_emote.h"

#define VS_MAX_SOURCES 4
#define VS_MAX_BOOKMARKS 128
#define VS_URL_MAX   1024
#define VS_MANI_MAX  4096


struct vs_pane_gpu;

typedef enum { VS_PLAT_UNKNOWN = 0, VS_PLAT_TWITCH, VS_PLAT_KICK } vs_platform;
typedef enum { VS_ROLE_VIDEO = 0, VS_ROLE_CHAT } vs_role;
typedef enum {
    VS_LOAD_EMPTY = 0, VS_LOAD_RESOLVING, VS_LOAD_FETCHING, VS_LOAD_READY, VS_LOAD_FAILED
} vs_loadstate;

// One HLS segment, with its absolute wall-clock time from
// EXT-X-PROGRAM-DATE-TIME. `url` is kept only for live streams, where the
// app writes its own playlist out of these segments; NULL for a VOD.
//
// `map` is the EXT-X-MAP line in force at this segment, also live-only. Some
// Twitch channels now stream fragmented MP4 rather than MPEG-TS, and those
// fragments are undecodable without the initialisation segment that tag
// points at - a playlist missing it fails with "error reading header".
typedef struct {
    double pos; double dur; double pdt;
    // This segment begins a new continuous section: the playlist carried an
    // EXT-X-DISCONTINUITY before it, so its timestamps do not follow on from
    // the previous segment's. Always false for segment 0.
    bool   disc;
    char *url; char *map;
} vs_seg;

typedef struct {
    bool          active;
    vs_platform   platform;
    vs_role       role;
    vs_loadstate  state;
    char          page_url[VS_URL_MAX];
    char          manifest[VS_MANI_MAX];
    char          title[256];
    char          channel[96];
    char          vid[96];
    char          status[192];
    double        duration;      // seconds, from the manifest
    double        api_start;     // unix seconds, yt-dlp metadata (coarse)
    double        pdt_start;     // unix seconds, first PROGRAM-DATE-TIME (authoritative)
    int           height;
    vs_seg       *segs;
    int           nsegs;
    double        offset;        // user nudge in seconds; + plays further ahead
    // playback (owned by vs_player.c)
    struct vs_pane_gpu *gpu;     // mpv handle + render context + FBO texture
    bool          tex_ready;
    bool          mpv_eof;
    bool          seek_refused;  // mpv would not move: live, unseekable
    double        pos;           // last observed time-pos
    double        want;          // last commanded position
    double        speed;
    float         volume;        // 0..1
    bool          solo;
    // Fit (default) letterboxes the picture inside the pane, preserving
    // aspect. Fill samples the largest centred sub-rectangle of the frame
    // that matches the pane's shape instead, so the picture covers the box
    // and the overflow is simply never read - which is what removes black
    // bars baked into the stream, as opposed to bars the pane's own shape
    // introduced.
    bool          fill;
    double        last_seek_t;
    double        refresh_at;    // app-time of the next manifest refresh
    bool          growing;       // VOD is still being recorded
    // ---- a VOD that is not one continuous recording ----
    // Kick records to MPEG-TS and marks a dropped feed with
    // EXT-X-DISCONTINUITY, after which the stream's timestamps restart at
    // zero. ffmpeg's HLS reader cannot address that second timeline at all:
    // the seek is accepted, time-pos reports the target, and no frame ever
    // arrives. So the playlist is split at every cut, one file per continuous
    // section, and mpv is given an ffconcat of them - the concat demuxer does
    // rebase timestamps across its inputs. nsect <= 1 is the ordinary case
    // (every Twitch VOD, most Kick ones) and none of this applies to it.
    int           nsect;          // continuous sections in this VOD
    char          sect_base[280]; // temp path stem the section files share
    bool          sect_reload;    // files rewritten; mpv must re-open them
    // ---- live stream ----
    // A live channel has no VOD manifest to seek around in: its playlist is
    // a sliding ~30s window. So the app keeps every segment it has seen and
    // writes its own playlist, which mpv plays instead. That pins mpv's
    // position 0 to a PROGRAM-DATE-TIME we know exactly, and the window
    // grows for as long as you watch rather than sliding out from under you.
    bool          live;          // a live stream, not a VOD
    char          live_url[VS_MANI_MAX];  // remote playlist, re-read to grow
    char          local_m3u8[300];        // our own playlist, mpv reads this
    int           live_written;   // segments already in local_m3u8
    double        live_edge;      // wall clock of the newest published frame
    double        live_lost;      // app-time of the last failed refresh
} vs_source;

// Does the chat column show? AUTO decides from what is loaded, the other
// two are the user saying so, and outrank it.
typedef enum { VS_CHAT_AUTO = 0, VS_CHAT_SHOW, VS_CHAT_HIDE } vs_chat_vis;

// Which chat a message came from. One per #source line in the log, so a
// merged log declares several and a message names one by index.
typedef struct {
    char          tag[8];        // "tw", "kk" - as written in the log
    vs_platform   platform;      // what to colour the line's dot and border
    char          channel[64];
    char          vid[64];
    // The platform's own id for the channel, which is how 7TV and BTTV
    // address it, and how an avatar is found. Empty for a log written
    // before the format carried it; those get no overlay emotes.
    char          user_id[32];

    // ---- a source the panel is configured to pull, not just one a file
    // ---- happened to declare
    char          vod_url[VS_URL_MAX];  // what gets exported
    char          path[VS_URL_MAX];     // where its own log is written
    double        anchor;        // unix time of ITS offset 0
    double        span;          // seconds to cover; Kick needs it asked for
    // Each platform's chat runs at its own latency against its own VOD, so
    // one nudge for the whole panel cannot straighten both.
    double        offset;
    double        exported_to;   // seconds of it already exported
    // This is the source that follows the video panes rather than a tile the
    // user added by hand. There is at most one, and loading a different VOD
    // replaces it - see vs_chat_source_bind.
    bool          auto_bound;
    bool          sync_wanted;   // queued: one export runs at a time
    vs_loadstate  sync_state;
    char          sync_status[160];
    int           avatar;        // image handle, or -1 for a letter tile
    int           nmsgs;         // how many of the merged array are its
} vs_chat_source;

#define VS_CHAT_SOURCES 4
#define VS_SRC_NONE     0xFF

typedef enum {
    VS_MSG_NORMAL = 0,
    VS_MSG_REPLY,
    VS_MSG_SYSTEM
} vs_msg_kind;

typedef struct {
    double        t;             // offset seconds into the chat's source VOD
    unsigned char r, g, b;
    unsigned char src;           // index into vs_chat.sources, or VS_SRC_NONE
    unsigned char kind;          // vs_msg_kind
    unsigned char nspan;         // emote runs in this message
    int           span_at;       // where they start in vs_chat.spans
    char          user[48];
    char          text[512];
} vs_chat_msg;

typedef struct {
    vs_chat_msg  *msgs;
    int           n, cap;
    // Emote runs for every message, in one array. A message points into it
    // with (span_at, nspan), so a message with no emotes costs nothing and
    // one with fifty is not truncated to fit a fixed field.
    vs_emote_span *spans;
    int           nspans, spans_cap;
    vs_chat_source sources[VS_CHAT_SOURCES];
    int           nsources;
    double        pdt_start;     // wall clock of chat offset 0
    double        offset;        // independent nudge
    char          label[192];
    char          path[VS_URL_MAX];
    char          status[192];
    vs_loadstate  state;
    int           cursor;        // index of last message at/below current wall
    // Periodic export ("update chat") state. Deliberately no VOD url and no
    // exported_to here: a chat is exported one configured source at a time,
    // each into its own log. A second export driven from the panel is what
    // wrote one VOD's chat to two differently named files.
    bool          auto_sync;            // keep topping up while the VOD grows
    bool          manual;              // source was picked by hand, so
                                       // auto-binding leaves it alone
    double        next_sync;            // app-time of the next export
    vs_loadstate  sync_state;
    char          sync_status[192];
    bool          autoscroll;
} vs_chat;

typedef enum {
    VS_LAYOUT_SPLIT = 0, VS_LAYOUT_GRID, VS_LAYOUT_PIP, VS_LAYOUT_SOLO,
    VS_LAYOUT_TRI,          // two across the top, one along the bottom
    VS_LAYOUT_COUNT
} vs_layout;

typedef struct { double wall; char label[64]; } vs_bookmark;

typedef struct {
    vs_source   src[VS_MAX_SOURCES];
    vs_chat     chat;
    int         master;          // index of the source driving wall clock
    int         focus;           // focused pane: big one in PiP, audio solo target
    vs_layout   layout;
    bool        split_vertical;
    // Which of the two panels the first source sits in. A view property, like
    // the orientation above and the ratio below it: the sources do not move,
    // so the master, the focus and the chat binding all stay where they are.
    bool        split_swap;
    float       split_ratio;
    // Chat column width in UNSCALED pixels, so it survives a change of
    // display or text scale: sc() applies it, exactly as it did the theme's
    // constant. 0 means "never dragged, use the theme default".
    int         chat_w;
    bool        chat_collapsed;   // folded to a strip, one button wide
    // Whether the chat column is there at all. AUTO is the old behaviour -
    // present when there is a chat to show, or in solo - which left the
    // panel missing entirely in split with no log loaded, and no way to
    // ask for it: the collapse key flipped a flag nothing was reading.
    vs_chat_vis chat_vis;
    bool        playing;
    double      wall;            // absolute instant being viewed (unix seconds)
    double      wall_lo, wall_hi;// union coverage of all ready sources
    bool        have_window;
    bool        wall_pinned;   // restored from a session: do not re-anchor
    bool        chat_lookup;   // finding the in-progress VOD of a live channel
    bool        live_mode;     // tracking the live edge (see vs_tick)
    vs_bookmark bm[VS_MAX_BOOKMARKS];
    int         nbm;
    // ui state
    bool        layout_menu;
    int         editing;         // -1 none, 0..3 source url field, 4 chat path
    char        edit_buf[VS_URL_MAX];
    int         edit_len;
    int         drag;            // -1 none, 0 divider, 1 unified bar, 10+i pane bar, 2 volume
    int         drag_aux;
    char        toast[256];
    double      toast_until;
    bool        show_help;
    vs_layout   prev_layout;   // restored when un-soloing a pane
    double      last_click_t;  // for double-click detection
    int         last_click_pane;
    float       pip_x, pip_y;  // 0-1 position of the PiP strip in the stage
    // Independent scale per axis, so the PiP box can be reshaped to the
    // aspect of what is in it. Both are multipliers on the scaled base
    // size; the real bound is the stage, applied at drag time.
    float       pip_sx, pip_sy;
    // Where the strip's top-left was when a resize drag began, in stage
    // pixels. pip_x/pip_y are a FRACTION of the space the strip does not
    // occupy, so the left edge moves whenever the width changes: measuring
    // a resize against the live edge meant measuring from a moving origin,
    // one frame stale, which is what made the box jump. The drag works off
    // this latched anchor and then re-solves pip_x/pip_y to keep it still.
    float       pip_ax, pip_ay;
    // Strip height minus the slots it contains, latched at drag start, so
    // a vertical drag can be turned back into a per-slot height whatever
    // the grab bar and padding happen to add.
    float       pip_oh;
    float       drag_dx, drag_dy;  // grab offset within a dragged box
    float       text_scale;    // 0.7 - 2.0, scales every font and the
                               // chrome that has to contain text
    bool        fullscreen;
    bool        theater;       // hide the chrome until the mouse nears it
    double      now;             // app time, seconds
} vs_app;

// ---- lifecycle ----
void   vs_app_init(vs_app *a);
// Clears what is loaded - sources, chat, bookmarks, playhead - and keeps how
// the app is set up: text size, layout, chat width, the PiP box. Refuses
// while a chat export is in flight; see the definition.
bool   vs_app_reset(vs_app *a);
void   vs_app_shutdown(vs_app *a);

// ---- sources ----
int    vs_add_source(vs_app *a, const char *url, vs_role role);
int    vs_add_source_at(vs_app *a, const char *url, vs_role role, int slot);
void   vs_remove_source(vs_app *a, int i);
int    vs_active_video_count(const vs_app *a);
int    vs_nth_active_video(const vs_app *a, int n);

// ---- sync ----
double vs_pos_for_wall(const vs_source *s, double wall);
double vs_wall_for_pos(const vs_source *s, double pos);
// The newest instant any live source can show, and how far behind it we are.
bool   vs_any_live(const vs_app *a);
double vs_live_edge(const vs_app *a);
double vs_live_ceiling(const vs_app *a);   // edge, less the safety floor
double vs_live_floor(const vs_app *a);     // oldest instant every pane has
void   vs_go_live(vs_app *a);
bool   vs_covers(const vs_source *s, double wall);
void   vs_recompute_window(vs_app *a);
void   vs_seek_wall(vs_app *a, double wall);
void   vs_toggle_play(vs_app *a);
void   vs_nudge(vs_app *a, int i, double delta);
void   vs_tick(vs_app *a, double dt);
void   vs_set_master(vs_app *a, int i);
void   vs_apply_audio(vs_app *a);
// Point the chat log at the Twitch VOD loaded in pane `i`.
void   vs_chat_bind_source(vs_app *a, int i);

// ---- bookmarks ----
void   vs_bookmark_add(vs_app *a);
void   vs_bookmark_jump(vs_app *a, int dir);

// ---- util ----
void   vs_toast(vs_app *a, const char *fmt, ...);
void   vs_fmt_clock(double unix_s, char *out, int cap);
void   vs_fmt_date(double unix_s, char *out, int cap);
void   vs_fmt_clock_utc(double unix_s, char *out, int cap);
void   vs_fmt_dur(double secs, char *out, int cap);
const char *vs_platform_name(vs_platform p);
// Does this page url name a recording rather than a channel?
bool vs_url_is_vod(const char *url);

#endif
