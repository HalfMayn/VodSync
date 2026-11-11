// vodsync - Twitch chat replay, synced independently of any video pane.
//
// The app reads a flat chat log (see tools/vs_chat_prep.py) rather than talking
// to Twitch directly: chat ingestion is the most fragile part of this problem,
// so it stays outside the app in a script you can swap or re-run.
#ifndef VS_CHAT_H
#define VS_CHAT_H
#include "vs_app.h"

// Loads a .tsv chat log, or a TwitchDownloader .json (converted via the
// bundled python helper first). Safe to call repeatedly.
bool vs_chat_load(vs_chat *c, const char *path);
void vs_chat_free(vs_chat *c);

// Anchor offset 0 of the log to an absolute instant (the VOD's PDT start).
void vs_chat_set_anchor(vs_chat *c, double pdt_start);

void vs_chat_reseek(vs_chat *c, double wall);   // jump the cursor

// ---- several chats at once ----
//
// Each configured source exports to a file of its own and they are merged
// into the one array the panel draws. Separate files rather than one
// combined export: each can then be topped up on its own while a stream is
// still running, which a single file would make a re-export of everything.
//
// Returns the source's index, or -1 when there is no room or it is already
// there. `anchor` is the unix time of the VOD's first frame; pass 0 if it
// is not known yet and it will be taken from the export.
int  vs_chat_source_add(vs_chat *c, vs_platform plat, const char *channel,
                        const char *user_id, const char *vod_url,
                        double anchor, double span);
// Add a source that is already exported, by its log file. The file's own
// #source line supplies the platform, channel and ids.
int  vs_chat_source_add_file(vs_chat *c, const char *path);
// Bind a VOD as the source that follows the video panes. Every automatic
// route in goes through this, so a VOD's chat has one log file whichever way
// it arrived; binding one panel-wide as well is what exported the same chat
// twice, as chat_<vid>.tsv and chat_<tag>_<vid>.tsv.
//
// At most one source is bound this way and a different VOD replaces it, so
// the tiles follow the panes - while a tile the user added by hand is never
// taken away under them. Returns its index, or -1 if there is no room or an
// export is in flight, since a running job holds its source's index and
// retiring the old one would renumber it. Callers retry on the next tick.
int  vs_chat_source_bind(vs_chat *c, vs_platform plat, const char *channel,
                         const char *user_id, const char *vod_url,
                         double anchor, double span);
// The index of the source that follows the panes, or -1 if there is none.
int  vs_chat_source_auto(const vs_chat *c);
void vs_chat_source_remove(vs_chat *c, int i);
// Re-reads every source's file and rebuilds the merged array. Cheap enough
// to call whenever one of them changes.
bool vs_chat_merge(vs_chat *c);
// Shift one source against the others. The offset is applied to the stored
// message times and the array re-sorted, NOT applied when a time is read:
// vs_chat_reseek binary-searches those times, so they must stay in the same
// order as the instants they stand for.
void vs_chat_source_nudge(vs_chat *c, int i, double delta);
// Export one configured source into its own log. One export runs at a
// time, so this returns false if another is already going.
bool vs_chat_sync_source(vs_chat *c, int i, double from_offset);
// Queue every configured source and start the first. Each finishes, merges
// and hands over to the next.
bool vs_chat_sync_all(vs_chat *c);
// Which source a given index is, for the UI.
int  vs_chat_source_count(const vs_chat *c);
void vs_chat_advance(vs_chat *c, double wall);  // step the cursor forward

// Index range to draw: the `want` most recent messages at or before now.
void vs_chat_window(const vs_chat *c, int want, int *first, int *count);

// Collect a finished export: records it against its source, rebuilds the
// merged array, and reports what happened in c->sync_status.
bool vs_chat_sync_poll(vs_chat *c);
bool vs_chat_sync_busy(const vs_chat *c);
double vs_chat_msg_wall(const vs_chat *c, int i);

#endif
