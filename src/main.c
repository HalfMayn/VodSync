// vodsync - entry point: window, Clay lifecycle, frame loop.
#define CLAY_IMPLEMENTATION
#include "clay.h"

#include "vs_app.h"
#include "vs_ui.h"
#include "vs_emote_img.h"
#include "vs_emote_sets.h"
#include "vs_player.h"
#include "vs_chat.h"
#include "vs_session.h"
#include "vs_browse.h"
#include "vs_download.h"
#include "vs_resolve.h"
#include "vs_platform.h"
#include "vs_theme.h"
#include "vs_logo.h"
#include "raylib.h"
#include "rlgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Provided by clay_renderer_raylib.c
extern void Clay_Raylib_Initialize(int w, int h, const char *title, unsigned flags);
extern void Clay_Raylib_Close(void);
extern void Clay_Raylib_Render(Clay_RenderCommandArray cmds, Font *fonts);
extern Clay_Dimensions Raylib_MeasureText(Clay_StringSlice text,
                                          Clay_TextElementConfig *config,
                                          void *userData);

int g_clay_error_count;          // read by the UI so errors are not silent
bool g_clay_text_cache_full;     // set below, cleared once the cache is reset

static void on_clay_error(Clay_ErrorData e) {
    // errorText is not null terminated.
    g_clay_error_count++;
    if (e.errorType == CLAY_ERROR_TYPE_TEXT_MEASUREMENT_CAPACITY_EXCEEDED)
        g_clay_text_cache_full = true;
    fprintf(stderr, "[clay error] %.*s\n",
            (int)e.errorText.length, e.errorText.chars);
}

static const char *cmd_type_name(int t) {
    switch (t) {
        case CLAY_RENDER_COMMAND_TYPE_NONE:                return "none";
        case CLAY_RENDER_COMMAND_TYPE_RECTANGLE:           return "rectangle";
        case CLAY_RENDER_COMMAND_TYPE_BORDER:              return "border";
        case CLAY_RENDER_COMMAND_TYPE_TEXT:                return "text";
        case CLAY_RENDER_COMMAND_TYPE_IMAGE:               return "image";
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:       return "scissor_start";
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:         return "scissor_end";
        case CLAY_RENDER_COMMAND_TYPE_CUSTOM:              return "custom";
        case CLAY_RENDER_COMMAND_TYPE_OVERLAY_COLOR_START: return "overlay_start";
        case CLAY_RENDER_COMMAND_TYPE_OVERLAY_COLOR_END:   return "overlay_end";
        default:                                           return "?";
    }
}

static void report_commands(Clay_RenderCommandArray cmds) {
    int counts[12] = { 0 };
    for (int i = 0; i < cmds.length; i++) {
        Clay_RenderCommand *c = Clay_RenderCommandArray_Get(&cmds, i);
        int t = (int)c->commandType;
        if (t >= 0 && t < 12) counts[t]++;
    }
    printf("render commands: %d total\n", cmds.length);
    for (int t = 1; t < 12; t++)
        if (counts[t]) printf("  %-14s %d\n", cmd_type_name(t), counts[t]);
}


static double g_selftest_secs = 0;      // >0: run headless-ish, shoot, exit

// What reset must clear, and what it must not. Printed either side of a
// --reset so the two lines can simply be compared.
// Ask for an image for every emote the loaded chat refers to.
//
// The distinct list is collected once: a five-hour log has tens of thousands
// of spans and walking them every frame to re-ask for a few hundred emotes
// would be pure waste. Asking repeatedly IS the interface - a request that
// found no free slot simply happens on a later frame - so this only has to
// keep asking for the ones that are not ready.
static void warm_emotes(vs_app *a) {
    enum { WARM_MAX = 256 };
    static int list[WARM_MAX];
    static int n = -1;
    if (n < 0) {
        n = 0;
        for (int i = 0; i < a->chat.nspans && n < WARM_MAX; i++) {
            int e = a->chat.spans[i].emote;
            bool seen = false;
            for (int k = 0; k < n; k++) if (list[k] == e) { seen = true; break; }
            if (!seen) list[n++] = e;
        }
        printf("warming %d distinct emote%s\n", n, n == 1 ? "" : "s");
    }
    int ready = 0;
    for (int i = 0; i < n; i++)
        if (vs_emote_img_get(list[i], VS_EMSZ_INLINE)) ready++;

    // Once, when they have all arrived: what each one actually decoded to.
    static bool dumped;
    if (!dumped && n > 0 && ready == n) {
        dumped = true;
        for (int i = 0; i < n; i++) {
            const vs_emote_img *im = vs_emote_img_get(list[i], VS_EMSZ_INLINE);
            if (!im) continue;
            const char *sn = "?";
            switch (vs_emote_source(list[i])) {
                case VS_EM_TWITCH: sn = "twitch"; break;
                case VS_EM_KICK:   sn = "kick";   break;
                case VS_EM_7TV:    sn = "7tv";    break;
                case VS_EM_BTTV:   sn = "bttv";   break;
                default: break;
            }
            printf("    emote %-6s %-20s %3dx%-3d  %2d frame%s  %.3fs\n",
                   sn, vs_emote_name(list[i]), im->fw, im->fh, im->frames,
                   im->frames == 1 ? " " : "s", im->frame_secs);
        }
        fflush(stdout);
    }
}

static void print_reset_state(const char *tag, const vs_app *a) {
    int nsrc = 0, nchat = 0;
    for (int i = 0; i < VS_MAX_SOURCES; i++) {
        if (!a->src[i].active) continue;
        nsrc++;
        if (a->src[i].role == VS_ROLE_CHAT) nchat++;
    }
    printf("%s: sources %d chatrole %d chat_n %d chat_state %d marks %d "
           "chans %d wall %.0f master %d | text_scale %.2f layout %d "
           "chat_w %d chat_vis %d pip %.2fx%.2f\n",
           tag, nsrc, nchat, a->chat.n, (int)a->chat.state, a->nbm,
           vs_ui_channels_n(), a->wall, a->master,
           (double)a->text_scale, (int)a->layout, a->chat_w,
           (int)a->chat_vis, (double)a->pip_sx, (double)a->pip_sy);
}
static double g_live_back = 0;          // --live-back: scrub off the edge, then return
static float  g_dpi_startup = 1.0f;     // display scaling, applied once fonts exist
static double g_watch_until = 0;        // print the master closely until here
static double g_watch_secs = 0;         // --watch: for how long, after the seek

// Where the session is kept between runs. Resolved once: the directory can
// be the app's own folder or a per-user one, and asking twice is wasteful.
static char g_autosave_path[VS_PATH_MAX];

static void autosave_now(vs_app *a) {
    if (g_selftest_secs > 0 && !getenv("VODSYNC_AUTOSAVE_PATH")) return;
    if (!g_autosave_path[0]) return;
    vs_session_save(a, g_autosave_path);
}

int main(int argc, char **argv) {
    // --selftest <seconds> runs the real renderer for a while, reports what it
    // drew and screenshots it, so the UI can be verified without a human.
    const char *urls[VS_MAX_SOURCES];
    int nurls = 0;
    const char *chat_path = NULL;
    bool   chat_auto = false;
    float  text_scale_arg = 0;
    const char *save_channel = NULL;
    bool where_only = false;
    const char *dl_test = NULL;
    bool play_saved = false;
    const char *dl_queue[VS_MAX_SOURCES];
    int ndl_queue = 0;
    int dl_secs = 20, dl_height = 160;
    bool   want_fullscreen = false;
    bool   want_theater = false;
    bool   open_menu = false;
    int    win_w = 1600, win_h = 950;
    bool   size_given = false;
    const char *export_logo = NULL;
    const char *export_set = NULL;
    float  pip_x = -1, pip_y = -1, pip_sx = -1, pip_sy = -1;
    // --pip-drag: grab the PiP's resize grip at one point and release at
    // another, with no mouse involved. Prints the strip box before and
    // after, which is how "it grows from the corner you drag" stops being
    // a claim: the latched corner has to read the same in both.
    float  pdx1 = -1, pdy1 = -1, pdx2 = -1, pdy2 = -1;
    bool   pip_dragged = false;
    // --reset prints what is loaded, resets, and prints it again, so "the
    // content goes and the preferences stay" is two lines to compare rather
    // than a claim. --chat-vod-commit drives the chat panel's new "chat from
    // another VOD" field the way typing into it and pressing Enter does.
    bool   want_reset = false, did_reset = false, did_chatvod = false;
    const char *chatvod_commit = NULL;
    int    toggle_solo = -2;
    // --swap drives the split view's panel swap through the same function the
    // button and the X key call, so what gets tested is what ships. It needs
    // no new printing to be observable: the pane boxes in the layout line are
    // keyed by SOURCE, so after a swap pane0 is reported in the second half.
    bool   want_swap = false, did_swap = false;
    bool   did_chat_at = false;
    const char *chat_vod = NULL;
    double seek_into = -1;          // seconds into the first VOD
    int    start_layout = -1;       // 0 split, 1 grid, 2 focus+pip
    bool   start_vsplit = false;    // --vsplit: stacked, not columns
    bool   emote_warm = false;      // --emote-warm: fetch what the log uses
    double chat_at = -1;            // --chat-at: seconds into the chat log
    const char *browse_channel = NULL;
    const char *chat_browse = NULL;   // --chat-browse: browser, chat target
    const char *chat_add[4];          // --chat-add: merge these exported logs
    int    nchat_add = 0;
    int    pick_row = -1;           // test hook: load this browser row
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--selftest") == 0 && i + 1 < argc) {
            g_selftest_secs = atof(argv[++i]);
        } else if (strcmp(argv[i], "--pick") == 0 && i + 1 < argc) {
            pick_row = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--chat-add") == 0 && i + 1 < argc) {
            if (nchat_add < 4) chat_add[nchat_add++] = argv[++i];
            else i++;
        } else if (strcmp(argv[i], "--chat-browse") == 0 && i + 1 < argc) {
            chat_browse = argv[++i];
        } else if (strcmp(argv[i], "--browse") == 0 && i + 1 < argc) {
            browse_channel = argv[++i];
        } else if (strcmp(argv[i], "--layout") == 0 && i + 1 < argc) {
            start_layout = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--at") == 0 && i + 1 < argc) {
            seek_into = atof(argv[++i]);
        } else if (strcmp(argv[i], "--text-scale") == 0 && i + 1 < argc) {
            text_scale_arg = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--pip") == 0 && i + 3 < argc) {
            pip_x = (float)atof(argv[++i]);
            pip_y = (float)atof(argv[++i]);
            pip_sx = (float)atof(argv[++i]);
            pip_sy = pip_sx;
            // Optional fourth value: the vertical scale, when the two
            // differ. Scales are positive, so a leading '-' is the next
            // switch rather than a number.
            if (i + 1 < argc && argv[i + 1][0] != '-')
                pip_sy = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--reset") == 0) {
            want_reset = true;
        } else if (strcmp(argv[i], "--chat-vod-commit") == 0 && i + 1 < argc) {
            chatvod_commit = argv[++i];
        } else if (strcmp(argv[i], "--pip-drag") == 0 && i + 4 < argc) {
            pdx1 = (float)atof(argv[++i]);
            pdy1 = (float)atof(argv[++i]);
            pdx2 = (float)atof(argv[++i]);
            pdy2 = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--chat-at") == 0 && i + 1 < argc) {
            chat_at = atof(argv[++i]);
        } else if (strcmp(argv[i], "--emote-warm") == 0) {
            emote_warm = true;
        } else if (strcmp(argv[i], "--vsplit") == 0) {
            start_vsplit = true;
        } else if (strcmp(argv[i], "--swap") == 0) {
            want_swap = true;
        } else if (strcmp(argv[i], "--toggle-solo") == 0 && i + 1 < argc) {
            toggle_solo = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--export-logo-set") == 0 && i + 1 < argc) {
            export_set = argv[++i];
        } else if (strcmp(argv[i], "--export-logo") == 0 && i + 1 < argc) {
            export_logo = argv[++i];
        } else if (strcmp(argv[i], "--watch") == 0 && i + 1 < argc) {
            // Report every pane once a second, from the moment the seek
            // --at asks for has been issued.
            g_watch_secs = atof(argv[++i]);
        } else if (strcmp(argv[i], "--live-back") == 0 && i + 1 < argc) {
            g_live_back = atof(argv[++i]);
        } else if (strcmp(argv[i], "--save-channel") == 0 && i + 1 < argc) {
            save_channel = argv[++i];
        } else if (strcmp(argv[i], "--dl-queue") == 0 && i + 1 < argc) {
            if (ndl_queue < VS_MAX_SOURCES) dl_queue[ndl_queue++] = argv[++i];
        } else if (strcmp(argv[i], "--saved") == 0) {
            play_saved = true;
        } else if (strcmp(argv[i], "--dl-test") == 0 && i + 1 < argc) {
            dl_test = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') dl_secs   = atoi(argv[++i]);
            if (i + 1 < argc && argv[i + 1][0] != '-') dl_height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--where") == 0) {
            where_only = true;
        } else if (strcmp(argv[i], "--open-menu") == 0) {
            open_menu = true;
        } else if (strcmp(argv[i], "--size") == 0 && i + 2 < argc) {
            win_w = atoi(argv[++i]);
            win_h = atoi(argv[++i]);
            size_given = true;
        } else if (strcmp(argv[i], "--theater") == 0) {
            want_theater = true;
        } else if (strcmp(argv[i], "--fullscreen") == 0) {
            want_fullscreen = true;
        } else if (strcmp(argv[i], "--chat-vod") == 0 && i + 1 < argc) {
            chat_vod = argv[++i];
        } else if (strcmp(argv[i], "--chat-auto") == 0) {
            chat_auto = true;
        } else if (strcmp(argv[i], "--chat") == 0 && i + 1 < argc) {
            chat_path = argv[++i];
        } else if (nurls < VS_MAX_SOURCES) {
            urls[nurls++] = argv[i];
        }
    }

    if (export_set) {
        static const int isz[] = { 16, 24, 32, 48, 64, 128, 256 };
        for (int k = 0; k < (int)(sizeof isz / sizeof isz[0]); k++) {
            Image im = vs_logo_image(isz[k]);
            char path[1024];
            snprintf(path, sizeof path, "%s/icon_%d.png", export_set, isz[k]);
            ExportImage(im, path);
            UnloadImage(im);
        }
        printf("exported icon sizes to %s\n", export_set);
        return 0;        // no window: vs_logo_image touches no GL
    }

    // Where everything is resolved to, and exit. The questions this answers
    // are the ones an installed copy raises: where did my channels go, and
    // why does every VOD fail to load. Both were guesswork from outside the
    // process before - the paths are computed, not configured.
    if (where_only) {
        char dir[VS_PATH_MAX];
        vs_exe_dir(dir, sizeof dir);
        printf("exe dir:    %s\n", dir);
        vs_state_dir(dir, sizeof dir);
        printf("state dir:  %s\n", dir);
        printf("  sessions, the saved channel list and chat logs go here\n");
        printf("yt-dlp:     %s\n", vs_ytdlp_cmd());
        printf("python:     %s\n", vs_python_cmd());
        printf("  chat replay needs python; playback does not\n");
        return 0;
    }

    // Queue several downloads and report what each one is doing, so the
    // two-at-a-time rule can be watched rather than taken on trust.
    if (ndl_queue > 0) {
        vs_downloads d;
        memset(&d, 0, sizeof d);
        vs_dl_defaults(&d.set);
        d.set.height[VS_PLAT_TWITCH] = 160;
        d.set.height[VS_PLAT_KICK]   = 160;
        printf("dl-queue: limit %d, %d urls\n", vs_dl_parallel(), ndl_queue);

        for (int i = 0; i < ndl_queue; i++) {
            char id[96];
            if (!vs_dl_id_from_url(dl_queue[i], id, sizeof id)) continue;
            int k = vs_dl_add(&d, vs_platform_of(dl_queue[i]), id, "", "",
                              dl_queue[i], 0);
            printf("dl-queue: added %s as job %d\n", id, k);
        }

        // Long enough for every job that is allowed to start to have got
        // past its lookup and into media.
        double t0 = vs_now_unix();
        int run_peak = 0;
        while (vs_now_unix() - t0 < 75) {
            vs_dl_poll(&d);
            int running = 0;
            for (int i = 0; i < d.n; i++)
                if (d.jobs[i].state == VS_DL_RUNNING) running++;
            if (running > run_peak) run_peak = running;
            vs_sleep_ms(200);
        }
        for (int i = 0; i < d.n; i++)
            printf("dl-queue: job %d state=%d pct=%.1f bytes=%.0f %s\n",
                   i, (int)d.jobs[i].state, d.jobs[i].pct,
                   d.jobs[i].got_bytes, d.jobs[i].status);
        printf("dl-queue: most running at once %d (limit %d)\n",
               run_peak, vs_dl_parallel());
        for (int i = 0; i < d.n; i++) vs_dl_cancel(&d, i);
        printf("dl-queue: %s\n",
               run_peak <= vs_dl_parallel() && run_peak > 0 ? "PASS" : "FAIL");
        return (run_peak <= vs_dl_parallel() && run_peak > 0) ? 0 : 1;
    }

    // Download a VOD and prove the saved copy still aligns with the streamed
    // one. See the note on the sidecar in vs_download.h for why this is the
    // test that matters: a wrong anchor syncs, and syncs to the wrong place.
    if (dl_test) {
        vs_platform plat = vs_platform_of(dl_test);
        printf("dl-test: url %s\n", dl_test);
        printf("dl-test: platform %s height %d secs %d\n",
               vs_platform_name(plat), dl_height, dl_secs);

        // The streamed side, resolved exactly as a pane would.
        vs_source *a = (vs_source *)calloc(1, sizeof(vs_source));
        a->platform = plat;
        a->role     = VS_ROLE_VIDEO;
        snprintf(a->page_url, sizeof a->page_url, "%s", dl_test);
        if (!vs_resolve_begin(a)) {
            printf("dl-test: result FAIL (could not start the lookup)\n");
            return 1;
        }
        for (int t = 0; t < 1200 && !vs_resolve_poll(a); t++) vs_sleep_ms(50);
        if (a->state != VS_LOAD_READY) {
            printf("dl-test: result FAIL (stream lookup: %s)\n", a->status);
            return 1;
        }
        printf("dl-test: streamed pdt=%.3f dur=%.3f nsegs=%d nsect=%d h=%d\n",
               a->pdt_start, a->duration, a->nsegs, a->nsect, a->height);

        // The download side. Its anchor comes from its own lookup, so a
        // mismatch here would catch a non-deterministic resolve as well as a
        // broken sidecar.
        vs_downloads d;
        memset(&d, 0, sizeof d);
        vs_dl_defaults(&d.set);
        if (dl_height > 0) {
            d.set.height[VS_PLAT_TWITCH] = dl_height;
            d.set.height[VS_PLAT_KICK]   = dl_height;
        }
        printf("dl-test: folder %s\n", d.set.dir);
        printf("dl-test: free %.1f GB\n", vs_disk_free(d.set.dir) / 1e9);

        char vid[96];
        snprintf(vid, sizeof vid, "%s", a->vid[0] ? a->vid : "dltest");
        int k = vs_dl_add(&d, plat, vid, a->channel, a->title, dl_test,
                          a->duration);
        if (k < 0) {
            printf("dl-test: result FAIL (queueing refused)\n");
            return 1;
        }
        printf("dl-test: estimate %.2f GB (quality table, +30%%)\n",
               d.jobs[k].est_bytes / 1e9);

        // Poll until the anchor is captured and media is actually moving.
        for (int t = 0; t < 1200 && d.jobs[k].state != VS_DL_RUNNING &&
                        d.jobs[k].state != VS_DL_FAILED; t++) {
            vs_dl_poll(&d);
            vs_sleep_ms(50);
        }
        if (d.jobs[k].state == VS_DL_FAILED) {
            printf("dl-test: result FAIL (anchor: %s)\n", d.jobs[k].status);
            return 1;
        }
        printf("dl-test: anchored pdt=%.3f dur=%.3f nsegs=%d asked=%d get=%d\n",
               d.jobs[k].pdt_start, d.jobs[k].dur, d.jobs[k].nsegs,
               d.jobs[k].height, d.jobs[k].vheight);
        printf("dl-test: estimate now %.2f GB\n", d.jobs[k].est_bytes / 1e9);

        // Write the sidecar now rather than at the end, so the comparison
        // does not depend on a multi-gigabyte download finishing first.
        if (!vs_dl_meta_write(&d.jobs[k])) {
            printf("dl-test: result FAIL (sidecar did not write)\n");
            return 1;
        }
        printf("dl-test: sidecar %s\n", d.jobs[k].meta);

        vs_source *b = (vs_source *)calloc(1, sizeof(vs_source));
        char err[256];
        err[0] = 0;
        if (!vs_dl_meta_read(d.jobs[k].meta, b, err, sizeof err)) {
            printf("dl-test: result FAIL (sidecar read: %s)\n", err);
            return 1;
        }
        printf("dl-test: restored pdt=%.3f dur=%.3f nsegs=%d nsect=%d h=%d\n",
               b->pdt_start, b->duration, b->nsegs, b->nsect, b->height);

        int bad = 0;
        printf("dl-test: anchor pdt      %s\n",
               a->pdt_start == b->pdt_start ? "OK" : (bad++, "FAIL"));
        printf("dl-test: anchor duration %s\n",
               a->duration == b->duration ? "OK" : (bad++, "FAIL"));
        printf("dl-test: anchor nsegs    %s\n",
               a->nsegs == b->nsegs ? "OK" : (bad++, "FAIL"));
        printf("dl-test: anchor nsect    %s\n",
               a->nsect == b->nsect ? "OK" : (bad++, "FAIL"));

        // The mapping itself, across the whole recording. This is the check
        // the feature lives or dies by: same wall-clock moment in, same
        // position out, whether the frames came off the network or off disk.
        int    samples = 256, off = 0;
        double worst = 0;
        for (int i = 0; i < samples; i++) {
            double wall = a->pdt_start + a->duration * (double)i / (samples - 1);
            double pa = vs_pos_for_wall(a, wall);
            double pb = vs_pos_for_wall(b, wall);
            double diff = pa > pb ? pa - pb : pb - pa;
            if (diff > worst) worst = diff;
            if (diff > 0.001) off++;
        }
        printf("dl-test: map samples     %s (%d/%d within 1ms, worst %.6fs)\n",
               off == 0 ? "OK" : (bad++, "FAIL"), samples - off, samples, worst);

        // And the real downloader, to show bytes arriving and the progress
        // template parsing. dl_secs 0 means run it to the end.
        double t0 = vs_now_unix(), last = -1;
        for (;;) {
            vs_dl_poll(&d);
            vs_dl *j = &d.jobs[k];
            if (j->state != VS_DL_RUNNING) break;
            if (j->pct - last >= 5.0) {
                last = j->pct;
                printf("dl-test: progress %.1f%% %.1f MB %.1f MB/s\n",
                       j->pct, j->got_bytes / 1e6, j->rate / 1e6);
                fflush(stdout);
            }
            if (dl_secs > 0 && vs_now_unix() - t0 > dl_secs) {
                vs_dl_cancel(&d, k);
                break;
            }
            vs_sleep_ms(100);
        }
        vs_dl *j = &d.jobs[k];
        printf("dl-test: media state=%d status=%s\n", (int)j->state, j->status);
        printf("dl-test: media bytes=%.0f pct=%.1f\n", j->got_bytes, j->pct);
        // What actually arrived, rather than what was asked for. Bytes per
        // second of recording cannot be fooled by a format string that
        // silently did not apply, which is the bug the first run of this
        // test hid behind a PASS.
        if (j->pct > 0 && j->nsegs > 0 && j->dur > 0) {
            double frags = j->pct / 100.0 * j->nsegs;
            double secs  = frags * (j->dur / j->nsegs);
            if (secs > 0)
                printf("dl-test: media %.0f kbps over %.0fs of video\n",
                       j->got_bytes * 8.0 / secs / 1000.0, secs);
        }
        if (j->state == VS_DL_FAILED) bad++;
        if (j->got_bytes <= 0) {
            printf("dl-test: media FAIL (nothing arrived)\n");
            bad++;
        } else {
            printf("dl-test: media OK\n");
        }

        printf("dl-test: result %s\n", bad == 0 ? "PASS" : "FAIL");
        return bad == 0 ? 0 : 1;
    }

    // Add a channel to the saved list and exit, before any window exists.
    // Useful where the UI is awkward to type into - a Steam Deck in Game
    // Mode, say - and as a check that the list is being written at all.
    if (save_channel) {
        vs_channels chans;
        vs_channels_load(&chans);
        char name[VS_CHAN_MAX];
        if (!vs_channel_normalize(save_channel, name, sizeof name)) {
            printf("not a channel name: %s\n", save_channel);
            return 2;
        }
        bool added = vs_channels_add(&chans, name);
        char where[VS_PATH_MAX];
        vs_state_dir(where, sizeof where);   // where it was actually written
        printf("%s %s  (%d saved, in %s/vodsync.channels)\n",
               added ? "saved" : "already saved", name, chans.n, where);
        return added ? 0 : 1;
    }

    Clay_Raylib_Initialize(win_w, win_h, "vodsync",
        FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT);
    if (!getenv("VODSYNC_NO_ICON")) vs_logo_set_window_icon();
    if (export_logo) {
        Image li = vs_logo_image(512);
        ExportImage(li, export_logo);
        UnloadImage(li);
        printf("logo exported to %s\n", export_logo);
    }
    SetExitKey(KEY_NULL);               // Esc is used to cancel text entry

    // What the display is scaled at. A 4K screen is normally run at 150% or
    // 200%, and a window that ignores that draws its text at a third of the
    // intended size. VODSYNC_DPI overrides it, which is also how it gets
    // tested on an ordinary screen.
    float dpi = GetWindowScaleDPI().x;
    const char *dpi_env = getenv("VODSYNC_DPI");
    if (dpi_env) dpi = (float)atof(dpi_env);
    if (!(dpi >= 0.5f && dpi <= 4.0f)) dpi = 1.0f;
    g_dpi_startup = dpi;
    SetWindowMinSize((int)(700 * dpi), (int)(420 * dpi));

    // The default window was sized for a 1x screen; grow it with the display
    // so the same amount of UI fits, without spilling off a smaller monitor.
    if (!size_given && dpi > 1.05f) {
        int mon = GetCurrentMonitor();
        int mw = GetMonitorWidth(mon), mh = GetMonitorHeight(mon);
        int w = (int)(win_w * dpi), h = (int)(win_h * dpi);
        if (mw > 0 && w > mw - 80) w = mw - 80;
        if (mh > 0 && h > mh - 120) h = mh - 120;
        SetWindowSize(w, h);
        if (mw > 0 && mh > 0) SetWindowPosition((mw - w) / 2, (mh - h) / 2);
    }

    // Put the window somewhere specific. Only for development: a test run
    // on the same screen as the copy being used covers it up, and these
    // runs take over the display for a few seconds each.
    //   set VODSYNC_WINDOW_POS=-2500,100
    // Applied after the DPI block above, which centres the window, so this
    // wins. Coordinates are virtual-desktop ones, so a monitor to the left
    // of the primary has negative x.
    {
        const char *wp = getenv("VODSYNC_WINDOW_POS");
        if (wp) {
            int wx = 0, wy = 0;
            if (sscanf(wp, "%d,%d", &wx, &wy) == 2) SetWindowPosition(wx, wy);
        }
    }

    // Rasterise one atlas per UI size. Drawing a glyph atlas at a size other
    // than the one it was built at is what makes small text look smeared.
    Font fonts[3];
    const int sizes[3] = { VS_SIZE_BODY, VS_SIZE_SMALL, VS_SIZE_MICRO };
    // Finding the font is the platform layer's job: where one lives is a
    // per-distro question, and answering it with a fixed list here is what
    // left Fedora on raylib's bitmap font. See vs_find_font.
    char found[VS_PATH_MAX];
    const char *font_path = NULL;
    const char *font_env = getenv("VODSYNC_FONT");
    if (font_env && FileExists(font_env)) font_path = font_env;
    else if (vs_find_font(found, sizeof found)) font_path = found;
    if (!font_path)
        fprintf(stderr, "[font] no usable font found; falling back to the "
                        "built-in bitmap font, which is hard to read. "
                        "Set VODSYNC_FONT=/path/to/font.ttf\n");

    for (int i = 0; i < 3; i++) {
        fonts[i] = font_path ? LoadFontEx(font_path, sizes[i], 0, 400)
                             : GetFontDefault();
        if (fonts[i].texture.id == 0) fonts[i] = GetFontDefault();
        else SetTextureFilter(fonts[i].texture, TEXTURE_FILTER_POINT);
    }
    printf("font: %s  (base sizes %d/%d/%d, glyphs %d)\n",
           font_path ? font_path : "raylib default",
           fonts[0].baseSize, fonts[1].baseSize, fonts[2].baseSize,
           fonts[0].glyphCount);

    // Clay caches one entry per measured word, keyed by string + font + size,
    // and only clears it on Clay_ResetMeasureTextCache(). Chat feeds it a
    // steady stream of strings it will never see again, so the default 16384
    // fills during a long session and text silently stops rendering. Both
    // calls must precede Clay_MinMemorySize(), which sizes the arena.
    Clay_SetMaxElementCount(16384);
    int words = 131072;
    if (getenv("VODSYNC_TEXT_CACHE")) {
        int v = atoi(getenv("VODSYNC_TEXT_CACHE"));
        if (v >= 1024) words = v;
    }
    Clay_SetMaxMeasureTextCacheWordCount(words);
    uint32_t sz = Clay_MinMemorySize();
    void *mem = malloc(sz);
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(sz, mem);
    Clay_Initialize(arena,
        (Clay_Dimensions){ (float)GetScreenWidth(), (float)GetScreenHeight() },
        (Clay_ErrorHandler){ on_clay_error, NULL });
    Clay_SetMeasureTextFunction(Raylib_MeasureText, fonts);

    vs_app app;
    vs_app_init(&app);
    vs_ui_init(fonts, font_path);

    // Any VOD urls on the command line load straight away.
    for (int i = 0; i < nurls; i++) vs_add_source(&app, urls[i], VS_ROLE_VIDEO);
    // Play these from disk rather than from the network. The pane was just
    // set up and has already asked for a lookup, so that lookup is called off
    // first - otherwise it would come back later and overwrite the anchor the
    // sidecar just supplied with one fetched over the network.
    if (play_saved) {
        vs_dl_settings dset;
        vs_dl_settings_load(&dset);
        printf("saved: folder %s\n", dset.dir);
        for (int i = 0; i < VS_MAX_SOURCES; i++) {
            vs_source *s = &app.src[i];
            if (!s->active || s->role != VS_ROLE_VIDEO) continue;
            char id[96], err[256];
            err[0] = 0;
            if (!vs_dl_id_from_url(s->page_url, id, sizeof id)) {
                printf("saved: %s has no VOD id\n", s->page_url);
                continue;
            }
            vs_resolve_cancel(s);
            if (vs_dl_load_saved(&dset, s, s->platform, id, err, sizeof err)) {
                printf("saved: pane %d plays %s\n", i, s->manifest);
                printf("saved: pane %d anchor pdt=%.3f dur=%.1f nsegs=%d "
                       "nsect=%d\n", i, s->pdt_start, s->duration,
                       s->nsegs, s->nsect);
            } else {
                printf("saved: pane %d cannot use a saved copy: %s\n", i, err);
            }
        }
    }
    if (chat_path) {
        extern bool vs_chat_load(vs_chat *, const char *);
        vs_chat_load(&app.chat, chat_path);
    }
    if (start_layout >= 0 && start_layout < VS_LAYOUT_COUNT)
        app.layout = (vs_layout)start_layout;
    if (start_vsplit) app.split_vertical = true;
    if (browse_channel) vs_ui_browse(browse_channel, nurls < VS_MAX_SOURCES ? nurls : 0);
    if (chat_browse) vs_ui_browse_chat(&app, chat_browse);
    if (nchat_add > 0) {
        for (int i = 0; i < nchat_add; i++)
            vs_chat_source_add_file(&app.chat, chat_add[i]);
        vs_chat_merge(&app.chat);
        printf("merged %d source(s): %s\n", app.chat.nsources,
               app.chat.status);
        for (int i = 0; i < app.chat.nsources; i++)
            printf("  src %d  %-7s %-16s %6d msgs  anchor %.0f\n", i,
                   vs_platform_name(app.chat.sources[i].platform),
                   app.chat.sources[i].channel,
                   app.chat.sources[i].nmsgs, app.chat.sources[i].anchor);
    }
    if (chat_vod) vs_add_source_at(&app, chat_vod, VS_ROLE_CHAT,
                                   nurls < VS_MAX_SOURCES ? nurls : 1);
    vs_ui_set_dpi(&app, g_dpi_startup);
    if (text_scale_arg > 0) vs_ui_set_text_scale(&app, text_scale_arg);
    if (want_fullscreen) { ToggleBorderlessWindowed(); app.fullscreen = true; }
    if (pip_x >= 0) {
        app.pip_x = pip_x; app.pip_y = pip_y;
        if (pip_sx > 0) app.pip_sx = pip_sx;
        if (pip_sy > 0) app.pip_sy = pip_sy;
    }
    // With no VODs named on the command line, pick up where the last run
    // left off: same sources, layout and playhead.
    const char *ap_env = getenv("VODSYNC_AUTOSAVE_PATH");
    if (ap_env) snprintf(g_autosave_path, sizeof g_autosave_path, "%s", ap_env);
    else vs_session_autosave_path(g_autosave_path, sizeof g_autosave_path);
    if (nurls == 0 && !browse_channel && !chat_vod && !chat_browse &&
        nchat_add == 0) {
        if (vs_session_load(&app, g_autosave_path))
            printf("restored previous session from %s\n", g_autosave_path);
    }
    printf("state in %s\n", g_autosave_path);
    if (want_theater) app.theater = true;
    if (open_menu) app.layout_menu = true;
    if (chat_auto) app.chat.auto_sync = true;
    if (g_selftest_secs > 0) app.playing = true;
    bool did_seek = false;

    double elapsed = 0, next_status = 0;
    bool   shot = false;
    bool debug = false;
    vs_install_quit_handler();
    while (!WindowShouldClose() && !vs_quit_requested()) {
        float dt = GetFrameTime();
        if (dt > 0.25f) dt = 0.25f;          // don't let a stall blow up the sync

        if (IsKeyPressed(KEY_F12)) {
            debug = !debug;
            Clay_SetDebugModeEnabled(debug);
        }

        // Self-heal if the measurement cache does fill anyway: dropping it
        // costs one frame of re-measuring, against text not drawing at all.
        if (g_clay_text_cache_full) {
            g_clay_text_cache_full = false;
            Clay_ResetMeasureTextCache();
        }

        vs_tick(&app, dt);

        // mpv draws into its own FBOs first, outside raylib's draw batch.
        // Whatever the emote workers finished becomes a texture here,
        // on the thread that owns the GL context, a few per frame.
        // Downloaded set documents are parsed here, on the one thread
        // allowed to write the emote table.
        vs_emote_sets_pump();
        vs_emote_img_pump();
        if (emote_warm) warm_emotes(&app);
        vs_ui_render_video(&app);

        Clay_SetLayoutDimensions((Clay_Dimensions){ (float)GetScreenWidth(),
                                                    (float)GetScreenHeight() });
        Vector2 mp = GetMousePosition();
        // The same override the UI's own proximity tests use, applied to
        // Clay's pointer as well: otherwise a screenshot run has two
        // pointers, the faked one for reveals and the real one for hover,
        // and the two disagree about what is under the cursor.
        {
            const char *fx = getenv("VODSYNC_FAKE_MOUSE_X");
            const char *fy = getenv("VODSYNC_FAKE_MOUSE_Y");
            if (fx) mp.x = (float)atof(fx);
            if (fy) mp.y = (float)atof(fy);
        }
        Clay_SetPointerState((Clay_Vector2){ mp.x, mp.y },
                             IsMouseButtonDown(MOUSE_BUTTON_LEFT));
        Vector2 wheel = GetMouseWheelMoveV();
        // Drag-scrolling off: a few pixels of movement while clicking a
        // control inside a scrollable panel would scroll it instead.
        Clay_UpdateScrollContainers(false,
            (Clay_Vector2){ wheel.x * 6, wheel.y * 6 }, dt);

        Clay_RenderCommandArray cmds = vs_ui_frame(&app, dt);

        bool shoot_now = (g_selftest_secs > 0 && !shot &&
                          elapsed + dt > g_selftest_secs);
        BeginDrawing();
        ClearBackground((Color){ 18, 18, 22, 255 });
        Clay_Raylib_Render(cmds, fonts);
        if (shoot_now) {
            // raylib batches geometry and only submits it on EndDrawing, so
            // flush first or the screenshot captures just the clear.
            rlDrawRenderBatchActive();
            TakeScreenshot("selftest.png");
        }
        EndDrawing();

        // Saving only at exit loses everything when the app is killed
        // rather than closed - which is what Steam's Game Mode does to a
        // non-Steam shortcut, and what a flat battery does anywhere.
        static double next_autosave = 0;
        if (app.now >= next_autosave) {
            next_autosave = app.now + 20.0;
            autosave_now(&app);
        }

        if (g_selftest_secs > 0) {
            elapsed += dt;
            if (elapsed >= next_status) {
                next_status = elapsed + 30.0;
                for (int i = 0; i < VS_MAX_SOURCES; i++) {
                    vs_source *s2 = &app.src[i];
                    if (!s2->active || s2->state != VS_LOAD_READY) continue;
                    printf("  t=%5.0fs [%d] %-7s dur=%8.1f growing=%d pos=%8.1f"
                           "%s%s" "\n",
                           elapsed, i, vs_platform_name(s2->platform),
                           s2->duration, s2->growing ? 1 : 0, s2->pos,
                           i == app.master ? "  MASTER" : "",
                           s2->live ? "  LIVE" : "");
                    if (s2->live)
                        printf("      behind live %.2fs  edge %.2f\n",
                               s2->live_edge - vs_wall_for_pos(s2, s2->pos),
                               s2->live_edge);
                }
                fflush(stdout);
            }
            if (want_swap && !did_swap && elapsed > 2) {
                did_swap = true;
                vs_ui_swap_split(&app);
                printf("swapped split panels -> split_swap %d\n",
                       app.split_swap ? 1 : 0);
            }
            if (toggle_solo > -2 && elapsed > 3) {
                vs_ui_toggle_solo(&app, toggle_solo);
                printf("toggled solo on pane %d -> layout %d\n",
                       toggle_solo, (int)app.layout);
                toggle_solo = -2;
            }
            if (pick_row >= 0 && elapsed > 2 && vs_ui_browse_pick(&app, pick_row)) {
                printf("picked browser row %d (%s)\n", pick_row,
                       chat_browse ? "chat target" : "into slots 0 and 1");
                pick_row = -1;
            }
            if (chat_at >= 0 && !did_chat_at && elapsed > 1 &&
                app.chat.state == VS_LOAD_READY && app.chat.pdt_start > 0) {
                did_chat_at = true;
                vs_seek_wall(&app, app.chat.pdt_start + chat_at);
                printf("chat playhead at %.0fs -> cursor %d\n",
                       chat_at, app.chat.cursor);
            }
            if (seek_into >= 0 && !did_seek && app.master >= 0 &&
                app.src[app.master].state == VS_LOAD_READY && elapsed > 3) {
                did_seek = true;
                vs_seek_wall(&app,
                             vs_wall_for_pos(&app.src[app.master], seek_into));
                printf("seeked to %.0fs into %s\n", seek_into,
                       app.src[app.master].channel);
                if (g_watch_secs > 0) g_watch_until = elapsed + g_watch_secs;
            }
            if (g_watch_until > 0 && elapsed < g_watch_until) {
                static double next_watch = 0;
                if (elapsed > next_watch) {
                    next_watch = elapsed + 1.0;
                    for (int q = 0; q < VS_MAX_SOURCES; q++) {
                        vs_source *sq = &app.src[q];
                        if (!sq->active || !sq->gpu) continue;
                        printf("    +pane%d %-6s pos %9.2f want %9.2f "
                               "wall %.2f parts %d refused %d\n",
                               q, vs_platform_name(sq->platform), sq->pos,
                               sq->want, vs_wall_for_pos(sq, sq->pos),
                               sq->nsect, sq->seek_refused ? 1 : 0);
                    }
                    vs_source *mm = app.master >= 0 ? &app.src[app.master] : NULL;
                    if (mm) printf("    +watch pos %.2f want %.2f wall %.2f "
                                   "live_mode %d\n",
                                   mm->pos, mm->want, app.wall, app.live_mode);
                }
            }

            // Scrub back off the live edge, then jump back to it: the
            // check that a live pane really is a recording you can move
            // around in, not just a window onto now.
            if (g_live_back > 0 && elapsed > 25 && elapsed < 26) {
                printf("live_mode=%d before scrub, wall %.2f\n",
                       app.live_mode ? 1 : 0, app.wall);
                vs_seek_wall(&app, app.wall - g_live_back);
                printf("scrubbed back %.0fs -> wall %.2f  master pos %.2f\n",
                       g_live_back, app.wall,
                       app.master >= 0 ? app.src[app.master].pos : -1);
                g_live_back = -g_live_back;
                g_watch_until = elapsed + 8.0;
            }
            if (g_live_back < 0 && elapsed > 45 && elapsed < 46) {
                printf("after %.0fs of replay: live_mode=%d wall %.2f "
                       "behind-edge %.2f\n",
                       20.0, app.live_mode ? 1 : 0, app.wall,
                       vs_live_ceiling(&app) - app.wall);
                for (int i = 0; i < VS_MAX_SOURCES; i++) {
                    vs_source *s2 = &app.src[i];
                    if (!s2->active || !s2->gpu) continue;
                    printf("   [%d] pos %.2f want %.2f err %+.3f\n", i, s2->pos,
                           s2->want, s2->want - s2->pos);
                }
                vs_go_live(&app);
                printf("go live -> wall %.2f\n", app.wall);
                g_live_back = 0;
            }
            if (want_reset && !did_reset && elapsed > g_selftest_secs * 0.5) {
                did_reset = true;
                print_reset_state("before", &app);
                vs_ui_reset(&app);
                print_reset_state("after", &app);
            }
            if (chatvod_commit && !did_chatvod && elapsed > g_selftest_secs * 0.4) {
                did_chatvod = true;
                vs_ui_chat_vod_commit(&app, chatvod_commit);
                print_reset_state("committed", &app);
            }
            // Half way in, so the layout exists to latch against and a
            // few more frames run before the box is read back.
            if (pdx1 >= 0 && !pip_dragged && elapsed > g_selftest_secs * 0.5) {
                pip_dragged = true;
                char pb[1024];
                vs_ui_probe_boxes(pb, sizeof pb);
                printf("pipbefore: %s\n", pb);
                vs_ui_pip_drag_begin(&app, pdx1, pdy1);
                vs_ui_pip_drag_to(&app, pdx2, pdy2);
                app.drag = -1;
            }
            if (!shot && elapsed > g_selftest_secs) {
                shot = true;
                report_commands(cmds);
                {
                    char pb[1024];
                    vs_ui_probe_boxes(pb, sizeof pb);
                    printf("layout: %s\n", pb);
                }
                printf("\nsource state after %.0fs:\n", elapsed);
                for (int i = 0; i < VS_MAX_SOURCES; i++) {
                    vs_source *s = &app.src[i];
                    if (!s->active) continue;
                    printf("  [%d] %-7s %-16s state=%d frame=%d pos=%8.2f "
                           "want=%8.2f err=%+6.3f speed=%.3f%s\n",
                           i, vs_platform_name(s->platform),
                           s->channel[0] ? s->channel : "-",
                           (int)s->state, vs_player_has_frame(s) ? 1 : 0,
                           s->pos, s->want, s->want - s->pos, s->speed,
                           i == app.master ? "  MASTER" : "");
                    char dbg[256];
                    vs_player_debug(s, dbg, sizeof dbg);
                    printf("      mpv: %s\n", dbg);
                    if (s->state == VS_LOAD_FAILED) printf("      %s\n", s->status);
                }
                if (app.chat.state == VS_LOAD_READY) {
                    printf("  chat: %d msgs, cursor %d, anchor %.0f\n",
                           app.chat.n, app.chat.cursor, app.chat.pdt_start);
                    // How many messages carry emotes, not just how many
                    // spans exist: one message with forty and forty with one
                    // are very different pictures.
                    int withem = 0;
                    for (int q = 0; q < app.chat.n; q++)
                        if (app.chat.msgs[q].nspan) withem++;
                    printf("  chat: %d spans in %d msgs, %d distinct emotes\n",
                           app.chat.nspans, withem, vs_emote_count());
                    printf("  chat: %d source(s)", app.chat.nsources);
                    for (int q = 0; q < app.chat.nsources; q++)
                        printf("  [%s %s %s]", app.chat.sources[q].tag,
                               vs_platform_name(app.chat.sources[q].platform),
                               app.chat.sources[q].channel);
                    printf("\n");
                    printf("  chat status: %s\n", app.chat.status);
                    int er, ep, ef, ec;
                    long eb;
                    vs_emote_img_stats(&er, &ep, &ef, &eb, &ec);
                    printf("  emotes: %d ready (%d from cache), %d pending, "
                           "%d failed, %ld KB\n", er, ec, ep, ef, eb / 1024);
                    int sf, sp, sx, se;
                    vs_emote_sets_status(&sf, &sp, &sx, &se);
                    printf("  emote sets: %d fetched, %d pending, %d failed, "
                           "%d emotes -> %d in the table\n",
                           sf, sp, sx, se, vs_emote_count());
                }
                int ab = vs_chat_source_auto(&app.chat);
                printf("  chat follows the panes: %s\n",
                       ab >= 0 ? app.chat.sources[ab].vod_url : "(nothing)");
                for (int q = 0; q < app.chat.nsources; q++) {
                    vs_chat_source *cs = &app.chat.sources[q];
                    const char *pl = cs->platform == VS_PLAT_KICK ? "kick"
                                                                  : "twitch";
                    const char *av = cs->user_id[0]
                        ? vs_emote_sets_avatar(pl, cs->user_id) : "";
                    printf("  tile %d  %-7s %-14s uid=%-10s handle=%2d  "
                           "img=%s  url=%.48s\n", q, pl, cs->channel,
                           cs->user_id[0] ? cs->user_id : "-", cs->avatar,
                           (cs->avatar >= 0 && vs_img_get(cs->avatar))
                               ? "ready" : "none",
                           av[0] ? av : "(none found)");
                }
                printf("clay errors: %d\n", g_clay_error_count);
                break;
            }
        }
    }

    // And once more on the way out, so the last few seconds are not lost.
    autosave_now(&app);

    vs_app_shutdown(&app);
    vs_emote_img_shutdown();    // before the GL context goes
    Clay_Raylib_Close();
    free(mem);
    return 0;
}
