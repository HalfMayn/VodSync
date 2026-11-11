// vodsync - channel browser: look up a channel's VODs on both platforms.
//
// Typing one channel name lists Twitch and Kick side by side, which is the
// quickest way to spot a simulcast pair worth syncing.
#ifndef VS_BROWSE_H
#define VS_BROWSE_H
#include "vs_app.h"

#define VS_MAX_VODS   40     // per platform
#define VS_MAX_SAVED  24
#define VS_CHAN_MAX   80

typedef struct {
    vs_platform platform;
    char        id[96];
    char        url[VS_URL_MAX];
    char        title[220];
    double      ts;          // unix seconds; 0 = not known yet
    double      dur;
    bool        ts_wanted;   // queued for background backfill
} vs_vod;

typedef struct {
    char         query[VS_CHAN_MAX];   // channel being listed
    vs_vod       tw[VS_MAX_VODS];
    int          ntw;
    vs_vod       kk[VS_MAX_VODS];
    int          nkk;
    vs_loadstate state;
    char         status[192];
    int          slot;                 // pane slot the results load into
    // Opened from the chat panel: a VOD picked here binds the CHAT to it
    // rather than loading it into `slot`. Cleared by vs_browse_open, so
    // every other way in is unaffected.
    bool         chat_target;
    bool         open;
    int          backfill_done;        // how many Twitch timestamps resolved
    bool         tw_live, kk_live;     // channel is streaming right now
} vs_browser;

typedef struct {
    char names[VS_MAX_SAVED][VS_CHAN_MAX];
    int  n;
} vs_channels;

void vs_browse_open(vs_browser *b, int slot);
void vs_browse_close(vs_browser *b);
bool vs_browse_begin(vs_browser *b, const char *channel);
bool vs_browse_poll(vs_browser *b);          // true when something changed
int  vs_browse_count(const vs_browser *b);

// A live channel's chat cannot be read from the channel URL: the export
// tool works on VODs. This finds the newest (in-progress) VOD for a
// channel, which is the one being recorded right now.
bool vs_live_vod_begin(const char *channel);
bool vs_live_vod_poll(char *url_out, int cap);   // true once finished

// Is the channel streaming right now, on either platform? Runs beside the
// VOD listing rather than inside it, so the list is not held up waiting for
// the answer. Polled by vs_browse_poll, which fills in tw_live / kk_live.
bool vs_live_check_begin(const char *channel);
bool vs_live_check_poll(bool *tw, bool *kk);

// Saved channel list, persisted next to the executable.
void vs_channels_load(vs_channels *c);
void vs_channels_save(const vs_channels *c);
bool vs_channels_add(vs_channels *c, const char *name);
// Accepts a bare name or a pasted channel URL; lowercases and trims.
// False when nothing usable is left.
bool vs_channel_normalize(const char *in, char *out, int cap);
void vs_channels_remove(vs_channels *c, int i);
// Empties the list and writes the file. Reset uses this; leaving the file
// alone would bring every channel back on the next start, which is not what
// clearing them means.
void vs_channels_clear(vs_channels *c);

#endif
