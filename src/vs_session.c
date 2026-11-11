// vodsync - session persistence.
#include "vs_session.h"
#include "vs_chat.h"
#include "vs_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void vs_session_default_path(char *out, int cap) {
    char dir[VS_PATH_MAX];
    vs_state_dir(dir, sizeof dir);
    snprintf(out, cap, "%s/vodsync.session", dir);
}

void vs_session_autosave_path(char *out, int cap) {
    char dir[VS_PATH_MAX];
    vs_state_dir(dir, sizeof dir);
    snprintf(out, cap, "%s/vodsync.autosave", dir);
}

bool vs_session_save(const vs_app *a, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;

    fprintf(f, "# vodsync session\n");
    fprintf(f, "layout %d\n", (int)a->layout);
    fprintf(f, "text_scale %.3f\n", a->text_scale);
    fprintf(f, "split_vertical %d\n", a->split_vertical ? 1 : 0);
    fprintf(f, "split_swap %d\n", a->split_swap ? 1 : 0);
    fprintf(f, "split_ratio %.4f\n", a->split_ratio);
    fprintf(f, "chat_w %d\n", a->chat_w);
    fprintf(f, "chat_collapsed %d\n", a->chat_collapsed ? 1 : 0);
    fprintf(f, "chat_vis %d\n", (int)a->chat_vis);
    // The PiP box was never saved, which was tolerable while it was one
    // number nobody tuned. Now that it is two and can be reshaped per
    // stream, losing it on every restart is not.
    fprintf(f, "pip %.4f %.4f %.4f %.4f\n",
            a->pip_x, a->pip_y, a->pip_sx, a->pip_sy);
    fprintf(f, "master %d\n", a->master);
    fprintf(f, "focus %d\n", a->focus);
    fprintf(f, "wall %.3f\n", a->wall);

    for (int i = 0; i < VS_MAX_SOURCES; i++) {
        const vs_source *s = &a->src[i];
        if (!s->active || !s->page_url[0]) continue;
        fprintf(f, "source %d %d %.4f %.4f %d %s\n",
                i, (int)s->role, s->offset, (double)s->volume,
                s->solo ? 1 : 0, s->page_url);
    }
    for (int i = 0; i < VS_MAX_SOURCES; i++)
        if (a->src[i].active && a->src[i].fill)
            fprintf(f, "source_fill %d 1\n", i);
    if (a->chat.path[0]) {
        fprintf(f, "chat %.4f %s\n", a->chat.offset, a->chat.path);
        fprintf(f, "chat_anchor %.3f\n", a->chat.pdt_start);
    }
    // The configured chat sources. Written whether or not a single log was
    // ever loaded, because with these the panel does not have one.
    fprintf(f, "chat_offset %.4f\n", a->chat.offset);
    for (int i = 0; i < a->chat.nsources; i++) {
        const vs_chat_source *cs = &a->chat.sources[i];
        // Either route in counts: a VOD to export, or a log already on
        // disk. Requiring the url dropped the second kind entirely.
        if (!cs->vod_url[0] && !cs->path[0]) continue;
        // exported_to is kept so a restart tops the log up instead of
        // fetching all of it again - which for Kick is twelve minutes.
        fprintf(f, "chat_source %d %.4f %.3f %.0f %.3f %s %s %s\n",
                (int)cs->platform, cs->offset, cs->anchor, cs->span,
                cs->exported_to,
                cs->user_id[0] ? cs->user_id : "-",
                cs->channel[0] ? cs->channel : "-",
                cs->vod_url[0] ? cs->vod_url : "-");
        if (cs->path[0])
            fprintf(f, "chat_source_path %d %s\n", i, cs->path);
        // Which tile follows the video panes. Without this a restored
        // session has none, and the binder adds a second tile for a VOD
        // that already has one.
        if (cs->auto_bound)
            fprintf(f, "chat_source_auto %d\n", i);
    }
    for (int i = 0; i < a->nbm; i++)
        fprintf(f, "bookmark %.3f %s\n", a->bm[i].wall, a->bm[i].label);

    fclose(f);
    return true;
}

bool vs_session_load(vs_app *a, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    // Clear existing sources but keep the window/UI defaults sane.
    for (int i = 0; i < VS_MAX_SOURCES; i++)
        if (a->src[i].active) vs_remove_source(a, i);
    a->nbm = 0;
    a->master = -1;

    bool fill_want[VS_MAX_SOURCES];
    memset(fill_want, 0, sizeof fill_want);
    struct pending { char url[VS_URL_MAX]; int role; double off; double vol; int solo;
                     int idx; };
    struct pending pend[VS_MAX_SOURCES];
    int npend = 0;
    char chat_path[VS_URL_MAX]; chat_path[0] = 0;
    double chat_off = 0, chat_anchor = 0;
    struct {
        int    plat;
        double off, anchor, span, exported;
        char   uid[32], chan[64], url[VS_URL_MAX], path[VS_URL_MAX];
        int    autob;
    } csrc[VS_CHAT_SOURCES];
    int ncsrc = 0;
    memset(csrc, 0, sizeof csrc);

    char line[VS_URL_MAX + 256];
    while (fgets(line, sizeof line, f)) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;
        if (len == 0 || line[0] == '#') continue;

        if (strncmp(line, "layout ", 7) == 0) {
            int v = atoi(line + 7);
            if (v >= 0 && v < VS_LAYOUT_COUNT) a->layout = (vs_layout)v;
        } else if (strncmp(line, "theater ", 8) == 0) {
            a->theater = atoi(line + 8) != 0;
        } else if (strncmp(line, "text_scale ", 11) == 0) {
            float v = (float)atof(line + 11);
            // Must track the clamp in vs_ui_set_text_scale: a value this
            // rejects is silently dropped, so a saved 4.0 would come back
            // as the default and look like the setting had not persisted.
            if (v >= 0.7f && v <= 5.0f) a->text_scale = v;
        } else if (strncmp(line, "pip ", 4) == 0) {
            float px, py, sx, sy;
            if (sscanf(line + 4, "%f %f %f %f", &px, &py, &sx, &sy) == 4) {
                if (px >= 0 && px <= 1) a->pip_x = px;
                if (py >= 0 && py <= 1) a->pip_y = py;
                if (sx >= 0.3f && sx <= 8.0f) a->pip_sx = sx;
                if (sy >= 0.3f && sy <= 8.0f) a->pip_sy = sy;
            }
        } else if (strncmp(line, "source_fill ", 12) == 0) {
            int idx, on;
            if (sscanf(line + 12, "%d %d", &idx, &on) == 2 &&
                idx >= 0 && idx < VS_MAX_SOURCES)
                fill_want[idx] = on != 0;
        } else if (strncmp(line, "chat_w ", 7) == 0) {
            int v = atoi(line + 7);
            // 0 is legitimate: it means the column was never dragged.
            if (v == 0 || (v >= 120 && v <= 900)) a->chat_w = v;
        } else if (strncmp(line, "chat_vis ", 9) == 0) {
            int v = atoi(line + 9);
            if (v >= VS_CHAT_AUTO && v <= VS_CHAT_HIDE) a->chat_vis = (vs_chat_vis)v;
        } else if (strncmp(line, "chat_collapsed ", 15) == 0) {
            a->chat_collapsed = atoi(line + 15) != 0;
        } else if (strncmp(line, "split_vertical ", 15) == 0) {
            a->split_vertical = atoi(line + 15) != 0;
        } else if (strncmp(line, "split_swap ", 11) == 0) {
            a->split_swap = atoi(line + 11) != 0;
        } else if (strncmp(line, "split_ratio ", 12) == 0) {
            float r = (float)atof(line + 12);
            if (r > 0.1f && r < 0.9f) a->split_ratio = r;
        } else if (strncmp(line, "focus ", 6) == 0) {
            int v = atoi(line + 6);
            if (v >= 0 && v < VS_MAX_SOURCES) a->focus = v;
        } else if (strncmp(line, "wall ", 5) == 0) {
            a->wall = atof(line + 5);
            a->wall_pinned = a->wall > 0;
        } else if (strncmp(line, "source ", 7) == 0) {
            if (npend >= VS_MAX_SOURCES) continue;
            int idx, role, solo;
            double off, vol;
            // "source <i> <role> <off> <vol> <solo> <url with spaces>"
            int consumed = 0;
            if (sscanf(line + 7, "%d %d %lf %lf %d %n",
                       &idx, &role, &off, &vol, &solo, &consumed) == 5 && consumed > 0) {
                const char *url = line + 7 + consumed;
                if (url[0]) {
                    struct pending *p = &pend[npend++];
                    snprintf(p->url, sizeof p->url, "%s", url);
                    p->role = role; p->off = off; p->vol = vol; p->solo = solo;
                    p->idx = (idx >= 0 && idx < VS_MAX_SOURCES) ? idx : 0;
                }
            }
        } else if (strncmp(line, "chat ", 5) == 0) {
            int consumed = 0;
            if (sscanf(line + 5, "%lf %n", &chat_off, &consumed) == 1 && consumed > 0)
                snprintf(chat_path, sizeof chat_path, "%s", line + 5 + consumed);
        } else if (strncmp(line, "chat_anchor ", 12) == 0) {
            chat_anchor = atof(line + 12);
        } else if (strncmp(line, "chat_offset ", 12) == 0) {
            chat_off = atof(line + 12);
        } else if (strncmp(line, "chat_source ", 12) == 0) {
            if (ncsrc < VS_CHAT_SOURCES) {
                int consumed = 0;
                if (sscanf(line + 12, "%d %lf %lf %lf %lf %31s %63s %n",
                           &csrc[ncsrc].plat, &csrc[ncsrc].off,
                           &csrc[ncsrc].anchor, &csrc[ncsrc].span,
                           &csrc[ncsrc].exported, csrc[ncsrc].uid,
                           csrc[ncsrc].chan, &consumed) == 7 && consumed > 0) {
                    snprintf(csrc[ncsrc].url, sizeof csrc[ncsrc].url, "%s",
                             line + 12 + consumed);
                    if (csrc[ncsrc].url[0]) ncsrc++;
                }
            }
        } else if (strncmp(line, "chat_source_path ", 17) == 0) {
            int idx = -1, consumed = 0;
            if (sscanf(line + 17, "%d %n", &idx, &consumed) == 1 &&
                consumed > 0 && idx >= 0 && idx < ncsrc) {
                snprintf(csrc[idx].path, sizeof csrc[idx].path, "%s",
                         line + 17 + consumed);
            }
        } else if (strncmp(line, "chat_source_auto ", 17) == 0) {
            int idx = atoi(line + 17);
            if (idx >= 0 && idx < ncsrc) csrc[idx].autob = 1;
        } else if (strncmp(line, "bookmark ", 9) == 0) {
            if (a->nbm >= VS_MAX_BOOKMARKS) continue;
            int consumed = 0;
            double w;
            if (sscanf(line + 9, "%lf %n", &w, &consumed) == 1) {
                a->bm[a->nbm].wall = w;
                snprintf(a->bm[a->nbm].label, sizeof a->bm[a->nbm].label,
                         "%s", consumed > 0 ? line + 9 + consumed : "");
                a->nbm++;
            }
        }
    }
    fclose(f);

    for (int i = 0; i < npend; i++) {
        int slot = vs_add_source(a, pend[i].url, (vs_role)pend[i].role);
        if (slot >= 0) {
            a->src[slot].offset = pend[i].off;
            a->src[slot].volume = (float)pend[i].vol;
            a->src[slot].solo   = pend[i].solo != 0;
            a->src[slot].fill   = fill_want[pend[i].idx];
        }
    }
    if (ncsrc > 0) {
        // Configured sources win: merging rebuilds the whole array, so
        // loading a single log first would only be thrown away.
        for (int i = 0; i < ncsrc; i++) {
            const char *uid  = strcmp(csrc[i].uid, "-")  ? csrc[i].uid  : "";
            const char *chan = strcmp(csrc[i].chan, "-") ? csrc[i].chan : "";
            const char *url = strcmp(csrc[i].url, "-") ? csrc[i].url : "";
            int k;
            if (url[0]) {
                k = vs_chat_source_add(&a->chat, (vs_platform)csrc[i].plat,
                                       chan, uid, url,
                                       csrc[i].anchor, csrc[i].span);
            } else {
                // No VOD url: a source added straight from a log file.
                k = vs_chat_source_add_file(&a->chat, csrc[i].path);
                if (k >= 0)
                    a->chat.sources[k].platform = (vs_platform)csrc[i].plat;
            }
            if (k >= 0) {
                a->chat.sources[k].offset = csrc[i].off;
                a->chat.sources[k].exported_to = csrc[i].exported;
                a->chat.sources[k].anchor = csrc[i].anchor;
                a->chat.sources[k].auto_bound = csrc[i].autob != 0;
                // The saved path wins over the derived one.
                if (csrc[i].path[0])
                    snprintf(a->chat.sources[k].path,
                             sizeof a->chat.sources[k].path, "%s",
                             csrc[i].path);
            }
        }
        vs_chat_merge(&a->chat);
        a->chat.offset = chat_off;
    } else if (chat_path[0]) {
        if (chat_anchor > 0) vs_chat_set_anchor(&a->chat, chat_anchor);
        vs_chat_load(&a->chat, chat_path);
        a->chat.offset = chat_off;
        if (chat_anchor > 0) a->chat.pdt_start = chat_anchor;
    }
    vs_toast(a, "loaded %d source%s from session", npend, npend == 1 ? "" : "s");
    return true;
}
