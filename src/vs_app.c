// vodsync - app state and the sync engine.
//
// Every source is placed on one absolute wall-clock axis using the first
// EXT-X-PROGRAM-DATE-TIME in its HLS manifest. The master source drives that
// axis from its own decoder position; the others are steered onto it by a
// coarse seek when far out and a gentle playback-rate trim when close, which
// converges without the jarring re-buffer a seek causes.
#include "vs_app.h"
#include "vs_resolve.h"
#include "vs_player.h"
#include "vs_chat.h"
#include "vs_browse.h"
#include "vs_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>

#define VS_HARD_SEEK   1.50   // seconds of error above which we seek
#define VS_DEADBAND    0.08   // below this, leave the rate alone
#define VS_MAX_TRIM    0.08   // +-8% playback rate authority
#define VS_REFRESH_SECS 20.0  // how often a live VOD's manifest is re-read
#define VS_CHAT_SYNC_SECS 60.0 // how often a live VOD's chat is topped up
#define VS_LIVE_REFRESH_SECS 2.0  // a live playlist publishes every 2s
#define VS_LIVE_CHAT_SECS  30.0   // chat top-up while a stream is live
#define VS_LIVE_MARGIN      6.0   // how far behind the edge "go live" lands
#define VS_LIVE_NEAR       20.0   // within this of the edge counts as live
#define VS_LIVE_FLOOR       4.0   // never ask for data this close to the edge
#define VS_LIVE_BOTTOM      8.0   // nor this close to the start of the buffer

void vs_app_init(vs_app *a) {
    memset(a, 0, sizeof *a);
    a->master       = -1;
    a->focus        = 0;
    a->layout       = VS_LAYOUT_SPLIT;
    a->split_ratio  = 0.5f;
    a->chat_w       = 0;        // 0 = the theme's default width
    a->chat_collapsed = false;
    a->chat_vis = VS_CHAT_AUTO;
    a->split_vertical = false;
    a->split_swap     = false;
    a->editing      = -1;
    a->drag         = -1;
    a->chat.autoscroll = true;
    a->text_scale = 1.0f;
    a->prev_layout = VS_LAYOUT_SPLIT;
    a->last_click_pane = -1;
    a->pip_x = 0.97f;
    a->pip_y = 0.95f;
    a->pip_sx = 1.0f;
    a->pip_sy = 1.0f;
    for (int i = 0; i < VS_MAX_SOURCES; i++) {
        a->src[i].volume = 0.8f;
        a->src[i].speed  = 1.0;
    }
}

// Clears the content without disturbing the setup.
//
// This cannot reuse vs_app_init: that memsets the struct, which would drop
// every mpv handle, resolver thread and segment list on the floor. It goes
// through vs_remove_source instead, which already bundles the teardown in
// the right order.
//
// Preferences are deliberately kept. Text size, layout, chat width and the
// PiP box describe the screen the app is on, not the VODs that happen to be
// loaded, and having them reset too would make this button annoying to use
// for the thing it is for.
//
// The saved channel list is NOT kept, but it is not cleared here either:
// that list and the file it is written to belong to vs_ui.c. The whole
// reset is vs_ui_reset, and this is the part of it that is app state.
//
// Returns false, having changed nothing, if a chat export is running: the
// worker thread writes into a->chat as it goes, so clearing it here would
// race a live writer and then have the finished export reload into the log
// that was just emptied. The caller uses that to leave the rest alone too.
bool vs_app_reset(vs_app *a) {
    if (vs_chat_sync_busy(&a->chat)) {
        vs_toast(a, "chat export running - try again when it finishes");
        return false;
    }

    for (int i = 0; i < VS_MAX_SOURCES; i++)
        if (a->src[i].active) vs_remove_source(a, i);

    vs_chat_free(&a->chat);
    memset(&a->chat, 0, sizeof a->chat);
    a->chat.autoscroll = true;

    a->nbm         = 0;
    a->master      = -1;
    a->focus       = 0;
    a->wall        = 0;
    a->wall_lo     = 0;
    a->wall_hi     = 0;
    a->have_window = false;
    a->wall_pinned = false;
    a->chat_lookup = false;
    a->live_mode   = false;
    a->playing     = false;
    a->layout_menu = false;
    a->editing     = -1;
    a->edit_len    = 0;
    a->edit_buf[0] = 0;
    a->drag        = -1;
    a->drag_aux    = 0;
    a->last_click_pane = -1;
    return true;        // vs_ui_reset finishes the job and toasts
}

void vs_app_shutdown(vs_app *a) {
    for (int i = 0; i < VS_MAX_SOURCES; i++) {
        vs_resolve_cancel(&a->src[i]);
        vs_player_close(&a->src[i]);
        vs_segs_free(a->src[i].segs, a->src[i].nsegs);
        a->src[i].segs = NULL;
        a->src[i].nsegs = 0;
        vs_live_cleanup(&a->src[i]);
        vs_sections_cleanup(&a->src[i]);
    }
    vs_chat_free(&a->chat);
}

// ------------------------------------------------------------- formatting

const char *vs_platform_name_safe(vs_platform p) { return vs_platform_name(p); }

void vs_fmt_clock(double unix_s, char *out, int cap) {
    if (unix_s <= 0) { snprintf(out, cap, "--:--:--"); return; }
    struct tm lt;
    memset(&lt, 0, sizeof lt);
    vs_localtime(unix_s, &lt);
    snprintf(out, cap, "%02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
}

void vs_fmt_date(double unix_s, char *out, int cap) {
    if (unix_s <= 0) { snprintf(out, cap, "----"); return; }
    struct tm lt;
    memset(&lt, 0, sizeof lt);
    vs_localtime(unix_s, &lt);
    static const char *mon[] = { "Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec" };
    snprintf(out, cap, "%d %s", lt.tm_mday, mon[lt.tm_mon % 12]);
}

void vs_fmt_clock_utc(double unix_s, char *out, int cap) {
    if (unix_s <= 0) { snprintf(out, cap, "--:--:--"); return; }
    struct tm gt;
    memset(&gt, 0, sizeof gt);
    vs_gmtime(unix_s, &gt);
    snprintf(out, cap, "%02d:%02d:%02d", gt.tm_hour, gt.tm_min, gt.tm_sec);
}

void vs_fmt_dur(double secs, char *out, int cap) {
    if (secs < 0) { snprintf(out, cap, "-"); secs = -secs; }
    int s = (int)(secs + 0.5);
    int h = s / 3600, m = (s % 3600) / 60;
    if (h > 0) snprintf(out, cap, "%d:%02d:%02d", h, m, s % 60);
    else       snprintf(out, cap, "%d:%02d", m, s % 60);
}

void vs_toast(vs_app *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->toast, sizeof a->toast, fmt, ap);
    va_end(ap);
    a->toast_until = a->now + 3.0;
}

// ---------------------------------------------------------------- sources

int vs_active_video_count(const vs_app *a) {
    int n = 0;
    for (int i = 0; i < VS_MAX_SOURCES; i++)
        if (a->src[i].active && a->src[i].role == VS_ROLE_VIDEO) n++;
    return n;
}

int vs_nth_active_video(const vs_app *a, int n) {
    for (int i = 0; i < VS_MAX_SOURCES; i++)
        if (a->src[i].active && a->src[i].role == VS_ROLE_VIDEO && n-- == 0) return i;
    return -1;
}

// Loads into a specific pane slot, replacing whatever is there.
int vs_add_source_at(vs_app *a, const char *url, vs_role role, int slot) {
    if (slot < 0 || slot >= VS_MAX_SOURCES) return vs_add_source(a, url, role);
    if (a->src[slot].active) vs_remove_source(a, slot);
    vs_platform p = vs_platform_of(url);
    if (p == VS_PLAT_UNKNOWN) { vs_toast(a, "not a twitch.tv or kick.com url"); return -1; }

    vs_source *s = &a->src[slot];
    vs_player_close(s);
    vs_segs_free(s->segs, s->nsegs);
    vs_live_cleanup(s);
    vs_sections_cleanup(s);
    memset(s, 0, sizeof *s);
    s->active   = true;
    s->role     = role;
    s->platform = p;
    s->volume   = 0.8f;
    s->speed    = 1.0;
    snprintf(s->page_url, sizeof s->page_url, "%s", url);
    vs_resolve_begin(s);
    if (a->master < 0 && role == VS_ROLE_VIDEO) a->master = slot;
    return slot;
}

int vs_add_source(vs_app *a, const char *url, vs_role role) {
    vs_platform p = vs_platform_of(url);
    if (p == VS_PLAT_UNKNOWN) {
        vs_toast(a, "not a twitch.tv or kick.com url");
        return -1;
    }
    if (role == VS_ROLE_CHAT && p != VS_PLAT_TWITCH) {
        vs_toast(a, "chat replay is Twitch only");
        return -1;
    }
    int slot = -1;
    for (int i = 0; i < VS_MAX_SOURCES; i++) if (!a->src[i].active) { slot = i; break; }
    if (slot < 0) { vs_toast(a, "all %d slots in use", VS_MAX_SOURCES); return -1; }

    vs_source *s = &a->src[slot];
    vs_player_close(s);
    vs_segs_free(s->segs, s->nsegs);
    vs_live_cleanup(s);
    vs_sections_cleanup(s);
    memset(s, 0, sizeof *s);
    s->active   = true;
    s->role     = role;
    s->platform = p;
    s->volume   = 0.8f;
    s->speed    = 1.0;
    snprintf(s->page_url, sizeof s->page_url, "%s", url);
    vs_resolve_begin(s);
    if (a->master < 0 && role == VS_ROLE_VIDEO) a->master = slot;
    return slot;
}

void vs_remove_source(vs_app *a, int i) {
    if (i < 0 || i >= VS_MAX_SOURCES) return;
    vs_resolve_cancel(&a->src[i]);
    vs_player_close(&a->src[i]);
    vs_live_cleanup(&a->src[i]);
    vs_sections_cleanup(&a->src[i]);
    vs_segs_free(a->src[i].segs, a->src[i].nsegs);
    memset(&a->src[i], 0, sizeof a->src[i]);
    a->src[i].volume = 0.8f;
    a->src[i].speed  = 1.0;
    if (a->master == i) {
        a->master = -1;
        for (int k = 0; k < VS_MAX_SOURCES; k++)
            if (a->src[k].active && a->src[k].role == VS_ROLE_VIDEO &&
                a->src[k].state == VS_LOAD_READY) { a->master = k; break; }
    }
    if (a->focus >= VS_MAX_SOURCES || !a->src[a->focus].active) {
        int f = vs_nth_active_video(a, 0);
        a->focus = f < 0 ? 0 : f;
    }
    vs_recompute_window(a);
}

void vs_set_master(vs_app *a, int i) {
    if (i < 0 || i >= VS_MAX_SOURCES) return;
    if (!a->src[i].active || a->src[i].role != VS_ROLE_VIDEO) return;
    a->master = i;
    vs_toast(a, "master: %s", a->src[i].channel[0] ? a->src[i].channel : "source");
}

// ------------------------------------------------------------------- sync

// For an unbroken recording these are one subtraction: mpv's position is the
// time elapsed since the first PROGRAM-DATE-TIME. That is the path every
// Twitch VOD and every live pane takes, and it is left exactly as it was.
//
// A recording the feed dropped out of is not unbroken. Its sections are
// concatenated into a single timeline for mpv (see vs_sections_write), and
// that timeline does not contain the gap between them - on the VOD this was
// measured against, 9.3 seconds of wall clock that no frame covers. Subtract,
// and every instant after the cut reads 9.3s early, holding the Kick pane
// permanently out of step with its Twitch counterpart. Landing a seek a
// second out is forgivable; that is not, because one clock across several
// streams is the whole point of the app. So a sectioned source maps through
// the segment table, which carries the true (position, PDT) pair for every
// segment it holds.

// The last segment starting at or before wall-clock instant `w`.
static const vs_seg *seg_at_pdt(const vs_source *s, double w) {
    int lo = 0, hi = s->nsegs - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (s->segs[mid].pdt <= w) lo = mid; else hi = mid - 1;
    }
    return &s->segs[lo];
}

// The last segment starting at or before timeline position `p`.
static const vs_seg *seg_at_pos(const vs_source *s, double p) {
    int lo = 0, hi = s->nsegs - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (s->segs[mid].pos <= p) lo = mid; else hi = mid - 1;
    }
    return &s->segs[lo];
}

double vs_pos_for_wall(const vs_source *s, double wall) {
    double w = wall + s->offset;
    if (s->nsect <= 1 || s->nsegs <= 0 || !s->segs)
        return w - s->pdt_start;
    if (w <= s->segs[0].pdt)
        return w - s->segs[0].pdt;        // before the start, as before
    const vs_seg *g = seg_at_pdt(s, w);
    double into = w - g->pdt;
    // Inside the gap between two sections no frame exists to ask for. The
    // nearest one is the last of the section that just ended, so land there
    // rather than somewhere the demuxer will never reach.
    if (into > g->dur && g != &s->segs[s->nsegs - 1] && (g + 1)->disc)
        into = g->dur;
    return g->pos + into;
}

double vs_wall_for_pos(const vs_source *s, double pos) {
    if (s->nsect <= 1 || s->nsegs <= 0 || !s->segs)
        return s->pdt_start + pos - s->offset;
    if (pos <= 0) return s->segs[0].pdt + pos - s->offset;
    const vs_seg *g = seg_at_pos(s, pos);
    return g->pdt + (pos - g->pos) - s->offset;
}

// ---- live edge ----
// A live pane can only show what has been published. With more than one, the
// edge is the earliest of them, so every pane has the instant we move to.

bool vs_any_live(const vs_app *a) {
    for (int i = 0; i < VS_MAX_SOURCES; i++)
        if (a->src[i].active && a->src[i].live && a->src[i].growing) return true;
    return false;
}

double vs_live_edge(const vs_app *a) {
    double edge = 0;
    for (int i = 0; i < VS_MAX_SOURCES; i++) {
        const vs_source *s = &a->src[i];
        if (!s->active || !s->live || s->state != VS_LOAD_READY) continue;
        if (!s->growing) continue;   // ended: there is no edge to chase
        double e = s->live_edge - s->offset;
        if (edge == 0 || e < edge) edge = e;
    }
    return edge;
}

// The newest instant it is safe to show: the edge we last saw, less a floor
// that covers the refresh we have not done yet. Going closer than this asks
// mpv for segments that are not in the playlist, which ends in EOF - and a
// live playlist that has hit EOF does not restart on its own.
double vs_live_ceiling(const vs_app *a) {
    double edge = vs_live_edge(a);
    return edge > 0 ? edge - VS_LIVE_FLOOR : 0;
}

// The oldest instant every live pane can actually play from. Panes are
// resolved seconds apart, so their buffers start at different times: taking
// the union low would hand the later pane a position inside the opening
// moments of its own playlist, which is exactly where a seek does not take.
double vs_live_floor(const vs_app *a) {
    double floor = 0;
    for (int i = 0; i < VS_MAX_SOURCES; i++) {
        const vs_source *s = &a->src[i];
        if (!s->active || !s->live || s->state != VS_LOAD_READY) continue;
        double f = s->pdt_start - s->offset + VS_LIVE_BOTTOM;
        if (f > floor) floor = f;
    }
    return floor;
}

void vs_go_live(vs_app *a) {
    double edge = vs_live_ceiling(a);
    if (edge <= 0) { vs_toast(a, "nothing live to jump to"); return; }
    vs_seek_wall(a, edge - VS_LIVE_MARGIN);
    if (!a->playing) vs_toggle_play(a);
    vs_toast(a, "jumped to live");
}

bool vs_covers(const vs_source *s, double wall) {
    if (s->state != VS_LOAD_READY || s->duration <= 0) return false;
    double p = vs_pos_for_wall(s, wall);
    return p >= 0.0 && p <= s->duration;
}

void vs_recompute_window(vs_app *a) {
    double lo = 1e18, hi = -1e18;
    bool any = false;
    for (int i = 0; i < VS_MAX_SOURCES; i++) {
        vs_source *s = &a->src[i];
        if (!s->active || s->state != VS_LOAD_READY) continue;
        // A live pane is scrubbable between these two margins. The top one
        // only applies while segments are still arriving; once the stream has
        // ended everything published is in hand and the last seconds should
        // be watchable like any other recording. The bottom one is there
        // because a seek into the first moments of the buffer does not take:
        // mpv accepts the command and carries on where it was, which reads as
        // the scrubber snapping back to live.
        double x = vs_wall_for_pos(s, 0) +
                   (s->live ? VS_LIVE_BOTTOM : 0.0);
        double y = vs_wall_for_pos(s, s->duration) -
                   ((s->live && s->growing) ? VS_LIVE_FLOOR : 0.0);
        if (x < lo) lo = x;
        if (y > hi) hi = y;
        any = true;
    }
    if (a->chat.state == VS_LOAD_READY && a->chat.n > 0) {
        double x = a->chat.pdt_start - a->chat.offset + a->chat.msgs[0].t;
        double y = a->chat.pdt_start - a->chat.offset +
                   a->chat.msgs[a->chat.n - 1].t;
        if (!any || x < lo) lo = x;
        if (!any || y > hi) hi = y;
        any = true;
    }
    a->have_window = any;
    if (!any) { a->wall_lo = a->wall_hi = 0; return; }
    a->wall_lo = lo;
    a->wall_hi = hi;
    if (a->wall < lo || a->wall > hi) a->wall = lo;
}

void vs_seek_wall(vs_app *a, double wall) {
    if (a->have_window) {
        if (wall < a->wall_lo) wall = a->wall_lo;
        if (wall > a->wall_hi) wall = a->wall_hi;
    }
    double lfloor = vs_live_floor(a);
    if (lfloor > 0 && wall < lfloor) wall = lfloor;
    a->wall = wall;
    for (int i = 0; i < VS_MAX_SOURCES; i++) {
        vs_source *s = &a->src[i];
        if (!s->active || s->role != VS_ROLE_VIDEO || s->state != VS_LOAD_READY) continue;
        double want = vs_pos_for_wall(s, wall);
        if (want < 0) want = 0;
        if (want > s->duration) want = s->duration;
        s->want = want;
        vs_player_seek(s, want);
        s->speed = 1.0;
        vs_player_set_speed(s, 1.0);
        s->last_seek_t = a->now;
    }
    vs_chat_reseek(&a->chat, wall);
}

void vs_toggle_play(vs_app *a) {
    a->playing = !a->playing;
    for (int i = 0; i < VS_MAX_SOURCES; i++) {
        vs_source *s = &a->src[i];
        if (!s->active || s->role != VS_ROLE_VIDEO) continue;
        vs_player_set_paused(s, !a->playing || !vs_covers(s, a->wall));
    }
}

void vs_nudge(vs_app *a, int i, double delta) {
    if (i < 0 || i >= VS_MAX_SOURCES || !a->src[i].active) return;
    a->src[i].offset += delta;
    vs_recompute_window(a);
    // Re-pin this source to the current instant immediately.
    vs_source *s = &a->src[i];
    if (s->role == VS_ROLE_VIDEO && s->state == VS_LOAD_READY) {
        double want = vs_pos_for_wall(s, a->wall);
        if (want >= 0 && want <= s->duration) {
            s->want = want;
            vs_player_seek(s, want);
            s->last_seek_t = a->now;
        }
    }
    vs_toast(a, "%s offset %+.2fs",
             s->channel[0] ? s->channel : "source", s->offset);
}

void vs_chat_bind_source(vs_app *a, int i) {
    if (i < 0 || i >= VS_MAX_SOURCES) return;
    vs_source *s = &a->src[i];
    if (!s->active || s->platform != VS_PLAT_TWITCH) return;
    for (int k = 0; k < a->chat.nsources; k++)
        if (strcmp(a->chat.sources[k].vod_url, s->page_url) == 0) return;

    if (s->live) {
        // Same as the automatic path: a live channel's chat lives in the VOD
        // Twitch is recording, so look that up rather than binding the
        // channel URL the exporter cannot use.
        a->chat.manual    = true;
        a->chat.pdt_start  = 0;
        snprintf(a->chat.label, sizeof a->chat.label, "%s chat (live)",
                 s->channel[0] ? s->channel : "twitch");
        if (!a->chat_lookup && vs_live_vod_begin(s->channel)) {
            a->chat_lookup = true;
            vs_toast(a, "chat source: %s (finding the live VOD)",
                     s->channel[0] ? s->channel : s->vid);
        }
        return;
    }

    int k = vs_chat_source_bind(&a->chat, s->platform, s->channel, "",
                                s->page_url, s->pdt_start, s->duration);
    if (k < 0) {
        vs_toast(a, "an export is running - try that again in a moment");
        return;
    }
    a->chat.manual    = true;
    a->chat.pdt_start = s->pdt_start;
    a->chat.next_sync = 0;
    snprintf(a->chat.label, sizeof a->chat.label, "%s chat",
             s->channel[0] ? s->channel : "twitch");
    // Asked for by hand, so fetch it now instead of waiting for Auto.
    vs_chat_sync_source(&a->chat, k, a->chat.sources[k].exported_to);
    vs_toast(a, "chat source: %s", s->channel[0] ? s->channel : s->vid);
}

void vs_apply_audio(vs_app *a) {
    bool any_solo = false;
    for (int i = 0; i < VS_MAX_SOURCES; i++)
        if (a->src[i].active && a->src[i].solo) any_solo = true;

    for (int i = 0; i < VS_MAX_SOURCES; i++) {
        vs_source *s = &a->src[i];
        if (!s->active || s->role != VS_ROLE_VIDEO) continue;
        bool audible = any_solo ? s->solo : true;
        vs_player_set_volume(s, audible ? s->volume : 0.0f);
    }
}

void vs_tick(vs_app *a, double dt) {
    a->now += dt;

    // Pick up finished resolve jobs.
    for (int i = 0; i < VS_MAX_SOURCES; i++) {
        vs_source *s = &a->src[i];
        if (!s->active) continue;
        // Ready, with no player open. A source can reach READY without any
        // resolve finishing - a saved copy read off disk has no lookup to
        // wait for - and such a pane would otherwise sit there drawn and
        // silent. manifest and gpu are both checked so a pane whose player
        // genuinely cannot open is not retried every tick.
        bool ready_unopened = s->state == VS_LOAD_READY &&
                              s->role == VS_ROLE_VIDEO &&
                              s->manifest[0] && !s->gpu;
        if (vs_resolve_poll(s) || ready_unopened) {
            if (s->state == VS_LOAD_READY) {
                bool first_ready = !a->have_window;
                vs_recompute_window(a);
                if (s->role == VS_ROLE_VIDEO) {
                    if (a->master < 0) a->master = i;
                    // Start on the first source's own timeline rather than the
                    // union low, which may be hours away in another VOD -
                    // unless a restored session already says where to be.
                    if (first_ready && !a->wall_pinned) {
                        a->wall = s->pdt_start;
                        // A live pane now keeps the whole published window,
                        // so its pdt_start is half a minute BEHIND the live
                        // edge - opening there would look like the stream
                        // was lagging. Open at the edge and leave that
                        // window sitting behind the playhead, which is what
                        // makes it scrubbable from the first frame.
                        // Its own edge, not vs_live_ceiling: `growing` is
                        // not set until the refresh loop runs later in this
                        // same tick, and the ceiling ignores a pane that is
                        // not yet known to be growing - so asking here would
                        // get 0 and silently leave the wall at the bottom of
                        // the window.
                        if (s->live && s->live_edge > s->pdt_start) {
                            double c = s->live_edge - s->offset - VS_LIVE_FLOOR;
                            double lo = s->pdt_start - s->offset + VS_LIVE_BOTTOM;
                            a->wall = c - VS_LIVE_MARGIN;
                            if (a->wall < lo) a->wall = lo;
                        }
                    }
                    vs_player_open(s);
                    double want = vs_pos_for_wall(s, a->wall);
                    if (want < 0) want = 0;
                    if (want > s->duration) want = s->duration;
                    s->want = want;
                    vs_player_seek(s, want);
                    vs_player_set_paused(s, !a->playing);
                } else {
                    // Chat-only source: its PDT anchors the chat log, and it
                    // takes over the binding from any video source.
                    a->chat.pdt_start = s->pdt_start;
                    snprintf(a->chat.label, sizeof a->chat.label,
                             "%s chat", s->channel);
                    if (!a->chat.manual)
                        vs_chat_source_bind(&a->chat, s->platform, s->channel,
                                            "", s->page_url, s->pdt_start,
                                            s->duration);
                }
                vs_apply_audio(a);
                vs_toast(a, "%s ready: %s", vs_platform_name(s->platform),
                         s->channel[0] ? s->channel : s->vid);
            } else if (s->state == VS_LOAD_FAILED) {
                vs_toast(a, "failed: %s", s->status);
            }
        }
    }

    // An in-progress VOD keeps growing, so re-read its manifest periodically
    // and extend the timeline. Without this the stream stops at whatever the
    // duration happened to be when it was loaded.
    for (int i = 0; i < VS_MAX_SOURCES; i++) {
        vs_source *s = &a->src[i];
        if (!s->active || s->state != VS_LOAD_READY) continue;

        double added = 0;
        if (s->live) vs_live_refresh_poll(s, &added);
        else         vs_refresh_poll(s, &added);
        if (added > 0) vs_recompute_window(a);
        // The files behind a sectioned VOD have been rewritten. concat read
        // its input list when it opened, so until it opens again the tail of
        // the timeline is there in our arithmetic and not in mpv's - and
        // seeking into that is the freeze all of this exists to prevent.
        if (s->sect_reload) {
            s->sect_reload = false;
            if (s->gpu) {
                vs_player_reload(s, vs_pos_for_wall(s, a->wall));
                s->last_seek_t = a->now;
            }
        }

        bool was_growing = s->growing;
        s->growing = vs_source_is_growing(s);
        if (was_growing && !s->growing && s->live)
            vs_toast(a, "%s went offline", s->channel[0] ? s->channel : "stream");
        if (!s->growing) continue;
        if (a->now >= s->refresh_at) {
            s->refresh_at = a->now +
                (s->live ? VS_LIVE_REFRESH_SECS : VS_REFRESH_SECS);
            if (s->live) vs_live_refresh_begin(s);
            else         vs_refresh_begin(s);
        }
    }

    // Pump mpv events / positions.
    for (int i = 0; i < VS_MAX_SOURCES; i++) {
        vs_source *s = &a->src[i];
        if (s->active && s->role == VS_ROLE_VIDEO) vs_player_poll(s);
    }

    // A live pane that read its playlist to the end stops there: mpv has hit
    // EOF and will not look again on its own. Segments have been appended
    // since, so seek it back into the stream.
    for (int i = 0; i < VS_MAX_SOURCES; i++) {
        vs_source *s = &a->src[i];
        if (!s->active || !s->live || !s->gpu || !s->mpv_eof) continue;
        if (!s->growing || a->now - s->last_seek_t < 2.0) continue;
        // Back to the instant being watched - not to the edge, which would
        // throw this pane out of sync with the others.
        double want = vs_pos_for_wall(s, a->wall);
        if (want > s->duration - 1.0) want = s->duration - VS_LIVE_MARGIN;
        if (want < 0) want = 0;
        s->mpv_eof = false;
        s->want = want;
        vs_player_seek(s, want);
        s->last_seek_t = a->now;
        if (i == a->master) a->wall = vs_wall_for_pos(s, want);
    }

    // Near the live edge the wall clock runs on real time, not on any one
    // pane's decoder. The edge itself advances in real time, so this is the
    // only clock that keeps pace with it - and it makes every pane a follower,
    // steered by the same seek-and-trim loop, so one pane buffering cannot
    // drag the others off the edge. Scrub back into the recording and the
    // master takes over again, exactly as it does for a VOD.
    double ceiling = vs_live_ceiling(a);
    a->live_mode = (ceiling > 0) && (ceiling - a->wall) < VS_LIVE_NEAR;

    vs_source *m = (a->master >= 0) ? &a->src[a->master] : NULL;
    if (a->live_mode) {
        if (a->playing) a->wall += dt;
        if (a->wall > ceiling) a->wall = ceiling;
    } else if (m && m->active && m->state == VS_LOAD_READY && m->gpu) {
        double mw = m->pdt_start + m->pos - m->offset;
        // Only accept the master's clock while it is actually moving, so a
        // buffering master doesn't drag everyone backwards.
        if (a->playing) {
            double jump = mw - a->wall;   // signed: direction is the tell
            // A live HLS playlist that is still growing can make mpv re-sync
            // toward the live edge if the demuxer falls behind - seen once
            // while a long first-time chat export was competing for CPU and
            // network. Anything we did not ask for and that is too large to be
            // real playback gets pulled back instead of dragging the whole
            // session with it.
            //
            // Backwards needs a far tighter bound than forwards, because
            // playback never goes backwards on its own: any such jump we did
            // not ask for is mpv having reloaded or restarted the file, and
            // its position momentarily reading near zero. Accepting that
            // hands the whole session a wall clock below the window, which
            // vs_recompute_window then clamps to the bottom of it - watched
            // from the outside, scrubbing back into a live stream "didn't
            // take" and threw the playhead to the start instead.
            bool unasked = (a->now - m->last_seek_t) > 2.0 && m->growing;
            // A seek we asked for a moment ago has not reached mpv's
            // time-pos yet: it still reads where playback WAS. Following it
            // writes that stale instant straight back into the wall clock,
            // undoing the seek on the very next frame - which is exactly
            // what made scrubbing back in a live stream look impossible,
            // and why it sometimes appeared to work (the race is won or
            // lost depending on whether the segment was already buffered).
            // Hold the wall where the seek put it until mpv agrees, or
            // until it is clear mpv is not going to.
            if (a->now - m->last_seek_t < 2.0 &&
                fabs(m->pos - m->want) > VS_HARD_SEEK) {
                /* nothing: a->wall stays where the seek left it */
            } else if (unasked && (jump > 60.0 || jump < -3.0)) {
                double want = vs_pos_for_wall(m, a->wall);
                if (want >= 0 && want <= m->duration) {
                    vs_player_seek(m, want);
                    m->last_seek_t = a->now;
                    vs_toast(a, "pulled %s back %.0fs after a live re-sync",
                             m->channel[0] ? m->channel : "master", fabs(jump));
                }
            } else {
                a->wall = mw;
            }
        }
    } else if (a->playing && a->have_window) {
        a->wall += dt;                   // master not ready yet: free-run
    }
    if (a->have_window) {
        if (a->wall < a->wall_lo) a->wall = a->wall_lo;
        if (a->wall > a->wall_hi) {
            a->wall = a->wall_hi;
            // Only stop at the end if nothing is still being recorded;
            // otherwise just hold here until the next refresh extends it.
            bool any_growing = false;
            for (int i = 0; i < VS_MAX_SOURCES; i++)
                if (a->src[i].active && a->src[i].growing) any_growing = true;
            if (!any_growing) a->playing = false;
        }
    }

    // Steer the followers.
    for (int i = 0; i < VS_MAX_SOURCES; i++) {
        vs_source *s = &a->src[i];
        if (!s->active || s->role != VS_ROLE_VIDEO || s->state != VS_LOAD_READY) continue;
        if (!s->gpu) continue;

        bool covers = vs_covers(s, a->wall);
        if (!covers) {
            vs_player_set_paused(s, true);
            continue;
        }
        if (i == a->master && !a->live_mode) {
            s->want = vs_pos_for_wall(s, a->wall);   // display only
            vs_player_set_paused(s, !a->playing);
            if (s->speed != 1.0) { s->speed = 1.0; vs_player_set_speed(s, 1.0); }
            continue;
        }
        vs_player_set_paused(s, !a->playing);
        if (!a->playing) continue;
        // Give a fresh seek time to settle before judging error.
        if (a->now - s->last_seek_t < 0.6) continue;

        double want = vs_pos_for_wall(s, a->wall);
        double err  = want - s->pos;
        s->want = want;

        if (fabs(err) > VS_HARD_SEEK) {
            vs_player_seek(s, want);
            s->last_seek_t = a->now;
            if (s->speed != 1.0) { s->speed = 1.0; vs_player_set_speed(s, 1.0); }
        } else if (fabs(err) > VS_DEADBAND) {
            double trim = err * 0.30;
            if (trim >  VS_MAX_TRIM) trim =  VS_MAX_TRIM;
            if (trim < -VS_MAX_TRIM) trim = -VS_MAX_TRIM;
            double sp = 1.0 + trim;
            if (fabs(sp - s->speed) > 0.004) { s->speed = sp; vs_player_set_speed(s, sp); }
        } else if (s->speed != 1.0) {
            s->speed = 1.0;
            vs_player_set_speed(s, 1.0);
        }
    }

    // ---- chat export ----------------------------------------------------
    // A finished VOD is exported once; one that is still recording gets topped
    // up on an interval, resuming from where the last export reached so each
    // update only fetches the new stretch.
    {
        vs_chat *c = &a->chat;
        if (vs_chat_sync_poll(c)) {
            if (c->sync_state == VS_LOAD_READY) {
                vs_recompute_window(a);
                vs_chat_reseek(c, a->wall);
            }
            vs_toast(a, "chat: %s", c->sync_status);
        }

        // Bind the chat to a Twitch VOD, preferring a chat-only source.
        if (vs_chat_source_auto(c) < 0 && !c->manual && !a->chat_lookup) {
            int pick = -1;
            for (int i = 0; i < VS_MAX_SOURCES; i++) {
                vs_source *s2 = &a->src[i];
                if (!s2->active || s2->state != VS_LOAD_READY) continue;
                if (s2->platform != VS_PLAT_TWITCH) continue;
                if (s2->role == VS_ROLE_CHAT) { pick = i; break; }
                if (pick < 0) pick = i;
            }
            if (pick >= 0 && a->src[pick].live) {
                // The exporter works on VODs, and a live channel's URL is not
                // one. Twitch records the stream as it goes, so the chat we
                // want is in that in-progress VOD; find it in the background.
                if (vs_live_vod_begin(a->src[pick].channel))
                    a->chat_lookup = true;
            } else if (pick >= 0) {
                vs_source *ps = &a->src[pick];
                int k = vs_chat_source_bind(c, ps->platform, ps->channel, "",
                                            ps->page_url, ps->pdt_start,
                                            ps->duration);
                if (k >= 0 && c->pdt_start <= 0) c->pdt_start = ps->pdt_start;
            }
        }
        if (a->chat_lookup) {
            char found[VS_URL_MAX];
            if (vs_live_vod_poll(found, sizeof found)) {
                a->chat_lookup = false;
                if (found[0]) {
                    // Start the export near the playhead, not at the top of a
                    // stream that may be twelve hours old: the first export
                    // would otherwise pull the whole day's chat before showing
                    // a single line. yt-dlp's timestamp for a live stream is
                    // when it started, which is offset 0 of its VOD.
                    double since = 0;
                    const char *chan = "";
                    for (int i = 0; i < VS_MAX_SOURCES; i++) {
                        vs_source *ls = &a->src[i];
                        if (!ls->active || !ls->live) continue;
                        if (ls->platform != VS_PLAT_TWITCH) continue;
                        chan = ls->channel;
                        if (ls->api_start <= 0) continue;
                        double into = a->wall - ls->api_start - 120.0;
                        if (into > since) since = into;
                        break;
                    }
                    int k = vs_chat_source_bind(c, VS_PLAT_TWITCH, chan, "",
                                                found, 0, 0);
                    if (k >= 0) {
                        // Only for a tile that is new here: one already
                        // holding a log has exported further than this.
                        if (c->sources[k].exported_to <= 0)
                            c->sources[k].exported_to = since;
                        c->next_sync = 0;
                    }
                    vs_toast(a, "chat: reading the in-progress VOD");
                } else {
                    vs_toast(a, "no VOD yet for this live stream's chat");
                }
            }
        }

        if (c->auto_sync && c->nsources > 0 && !vs_chat_sync_busy(c) &&
            a->now >= c->next_sync) {
            // A live stream's chat is in a VOD that is still being written,
            // so it is always worth topping up - and more often, since the
            // picture is only a few seconds behind the edge.
            bool live = vs_any_live(a);
            bool growing = live;
            for (int i = 0; i < VS_MAX_SOURCES; i++) {
                if (!a->src[i].active || !a->src[i].growing) continue;
                for (int k = 0; k < c->nsources; k++)
                    if (strcmp(a->src[i].page_url, c->sources[k].vod_url) == 0)
                        growing = true;
            }
            double iv = live ? VS_LIVE_CHAT_SECS : VS_CHAT_SYNC_SECS;

            if (vs_chat_sync_all(c)) {
                // A finished VOD needs exporting only once.
                c->next_sync = growing ? a->now + iv : 1e18;
            } else {
                c->next_sync = a->now + iv;   // retry later
            }
        }
    }

    vs_chat_advance(&a->chat, a->wall);
    if (a->toast_until > 0 && a->now > a->toast_until) {
        a->toast[0] = 0;
        a->toast_until = 0;
    }
}

// ------------------------------------------------------------- bookmarks

void vs_bookmark_add(vs_app *a) {
    if (a->nbm >= VS_MAX_BOOKMARKS) { vs_toast(a, "bookmark list full"); return; }
    // keep sorted by wall clock
    int at = a->nbm;
    for (int i = 0; i < a->nbm; i++) if (a->bm[i].wall > a->wall) { at = i; break; }
    for (int i = a->nbm; i > at; i--) a->bm[i] = a->bm[i - 1];
    a->bm[at].wall = a->wall;
    char c[32];
    vs_fmt_clock(a->wall, c, sizeof c);
    snprintf(a->bm[at].label, sizeof a->bm[at].label, "%s", c);
    a->nbm++;
    vs_toast(a, "bookmark %d at %s", at + 1, c);
}

void vs_bookmark_jump(vs_app *a, int dir) {
    if (a->nbm == 0) { vs_toast(a, "no bookmarks"); return; }
    int best = -1;
    if (dir > 0) {
        for (int i = 0; i < a->nbm; i++)
            if (a->bm[i].wall > a->wall + 0.5) { best = i; break; }
    } else {
        for (int i = a->nbm - 1; i >= 0; i--)
            if (a->bm[i].wall < a->wall - 0.5) { best = i; break; }
    }
    if (best < 0) { vs_toast(a, dir > 0 ? "no later bookmark" : "no earlier bookmark"); return; }
    vs_seek_wall(a, a->bm[best].wall);
    vs_toast(a, "bookmark %d", best + 1);
}
