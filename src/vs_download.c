// vodsync - downloading a VOD, and the sidecar that keeps it syncable.
// See vs_download.h for why the sidecar exists at all.
#include "vs_download.h"
#include "vs_platform.h"
#include "vs_resolve.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------- helpers

// Defined further down with the rest of the job machinery; vs_dl_add
// needs it before then.
static int set_height(const vs_downloads *d, vs_platform plat);

static const char *plat_tag(vs_platform p) {
    return p == VS_PLAT_KICK ? "kk" : "tw";
}

static vs_platform tag_plat(const char *t) {
    return (t[0] == 'k' && t[1] == 'k') ? VS_PLAT_KICK : VS_PLAT_TWITCH;
}

// yt-dlp calls a Twitch VOD "v2919692356"; the url it came from, and every
// path the app derives from one - chat logs included - call it
// "2919692356". Either spelling may reach this file depending on whether a
// tile came from the listing or from a resolve, and the two must not name
// different files, or a saved VOD becomes invisible to the code looking for
// it. Normalised here because this is the one place both routes pass
// through.
static void norm_id(const char *in, char *out, int cap) {
    if (!in) { if (cap > 0) out[0] = 0; return; }
    const char *p = in;
    if ((p[0] == 'v' || p[0] == 'V') && p[1]) {
        const char *q = p + 1;
        while (*q >= '0' && *q <= '9') q++;
        if (*q == 0) p++;          // "v" followed by digits only
    }
    snprintf(out, cap, "%s", p);
}

// Size in bytes, or 0 when the file is not there. One call answers both
// "does it exist" and "how much of it arrived", which is all this file asks.
static double file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long n = ftell(f);
    fclose(f);
    return n > 0 ? (double)n : 0;
}

// ------------------------------------------------------------ settings

void vs_dl_defaults(vs_dl_settings *set) {
    memset(set, 0, sizeof *set);
    vs_videos_dir(set->dir, sizeof set->dir);
    // 1080 rather than "best": best on Twitch can mean a source-quality
    // stream several times the size, which is not what someone who has not
    // opened the settings panel is asking for.
    set->height[VS_PLAT_TWITCH] = 1080;
    set->height[VS_PLAT_KICK]   = 1080;
}

static void settings_path(char *out, int cap) {
    char dir[VS_PATH_MAX];
    vs_state_dir(dir, sizeof dir);
    snprintf(out, cap, "%s%cvodsync.downloads", dir, vs_path_sep());
}

bool vs_dl_settings_save(const vs_dl_settings *set) {
    char path[VS_PATH_MAX];
    settings_path(path, sizeof path);
    char buf[VS_PATH_MAX + 128];
    // One field per line, like the session file: a download folder can
    // contain spaces, and two space-separated fields on one line cannot be
    // told apart afterwards.
    int n = snprintf(buf, sizeof buf,
                     "dir %s\nheight_tw %d\nheight_kk %d\n",
                     set->dir, set->height[VS_PLAT_TWITCH],
                     set->height[VS_PLAT_KICK]);
    return vs_file_write(path, buf, n, true);
}

void vs_dl_settings_load(vs_dl_settings *set) {
    vs_dl_defaults(set);
    char path[VS_PATH_MAX];
    settings_path(path, sizeof path);
    unsigned char *txt = NULL;
    int len = 0;
    if (!vs_file_read(path, &txt, &len) || !txt) return;

    char *p = (char *)txt;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        while (*p == '\r') p++;
        int v = 0;
        if (strncmp(p, "dir ", 4) == 0 && p[4]) {
            snprintf(set->dir, sizeof set->dir, "%s", p + 4);
            // Trim a trailing CR that a hand-edited file may carry.
            int dn = (int)strlen(set->dir);
            while (dn > 0 && (set->dir[dn - 1] == '\r' || set->dir[dn - 1] == ' '))
                set->dir[--dn] = 0;
        } else if (sscanf(p, "height_tw %d", &v) == 1) {
            set->height[VS_PLAT_TWITCH] = v;
        } else if (sscanf(p, "height_kk %d", &v) == 1) {
            set->height[VS_PLAT_KICK] = v;
        }
        if (!nl) break;
        p = nl + 1;
    }
    free(txt);
    if (!set->dir[0]) vs_videos_dir(set->dir, sizeof set->dir);
}

// --------------------------------------------------------------- paths

void vs_dl_paths(const vs_dl_settings *set, vs_platform plat, const char *id,
                 char *media, int mcap, char *meta, int mecap) {
    // Named from the platform and the VOD id, the way chat logs are. Derived
    // from content rather than from when it was fetched, so a later run finds
    // an earlier run's download with no index that could go stale.
    char nid[96];
    norm_id(id, nid, sizeof nid);
    if (media) snprintf(media, mcap, "%s%cvod_%s_%s.ts",
                        set->dir, vs_path_sep(), plat_tag(plat), nid);
    if (meta)  snprintf(meta,  mecap, "%s%cvod_%s_%s.vsmeta",
                        set->dir, vs_path_sep(), plat_tag(plat), nid);
}

bool vs_dl_id_from_url(const char *url, char *out, int cap) {
    if (cap > 0) out[0] = 0;
    if (!url || !url[0]) return false;
    const char *slash = strrchr(url, '/');
    const char *last  = slash ? slash + 1 : url;
    if (!last[0]) return false;
    char buf[128];
    snprintf(buf, sizeof buf, "%s", last);
    char *frag = strchr(buf, '#');      // Kick urls can carry a fragment
    if (frag) *frag = 0;
    char *q = strchr(buf, '?');
    if (q) *q = 0;
    if (!buf[0]) return false;
    char nid[96];
    norm_id(buf, nid, sizeof nid);
    snprintf(out, cap, "%s", nid);
    return out[0] != 0;
}

vs_saved vs_dl_saved(const vs_dl_settings *set, vs_platform plat,
                     const char *id, double *bytes) {
    char media[VS_PATH_MAX], meta[VS_PATH_MAX];
    vs_dl_paths(set, plat, id, media, sizeof media, meta, sizeof meta);
    double n = file_size(media);
    if (bytes) *bytes = n;
    if (n <= 0) return VS_SAVED_NO;
    // Media with no sidecar is playable but cannot be aligned. Reported
    // separately because the repair is to rebuild the sidecar from the
    // manifest - one HTTP GET - not to fetch the video again.
    if (file_size(meta) <= 0) return VS_SAVED_NO_ANCHOR;
    return VS_SAVED_YES;
}

// ------------------------------------------------------------ estimate

double vs_dl_estimate(int height, double dur) {
    if (dur <= 0) return 0;
    int kbps;
    if      (height >= 1080) kbps = 6000;
    else if (height >=  900) kbps = 4500;
    else if (height >=  720) kbps = 3500;
    else if (height >=  480) kbps = 1600;
    else if (height >=  360) kbps =  850;
    else if (height >=  160) kbps =  300;
    else                     kbps = 6000;   // 0 means "best there is"
    // Deliberately 30% over. Unused disk costs nothing; running out three
    // hours into a four hour VOD costs the whole download.
    return (double)kbps * 1000.0 / 8.0 * dur * 1.3;
}

// --------------------------------------------------------------- queue

int vs_dl_find(const vs_downloads *d, vs_platform plat, const char *id) {
    char want[96];
    norm_id(id, want, sizeof want);
    for (int i = 0; i < d->n; i++)
        if (d->jobs[i].platform == plat && strcmp(d->jobs[i].id, want) == 0)
            return i;
    return -1;
}

int vs_dl_active(const vs_downloads *d) {
    int n = 0;
    for (int i = 0; i < d->n; i++) {
        vs_dl_state st = d->jobs[i].state;
        if (st == VS_DL_QUEUED || st == VS_DL_ANCHOR || st == VS_DL_RUNNING) n++;
    }
    return n;
}

int vs_dl_parallel(void) {
    // Read once. A limit that could change between two calls in the same
    // poll would let the running count and the gate disagree.
    static int n = 0;
    if (n == 0) {
        const char *env = getenv("VODSYNC_DL_PARALLEL");
        n = (env && env[0]) ? atoi(env) : VS_DL_PARALLEL;
        if (n < 1)           n = 1;
        if (n > VS_MAX_DL)   n = VS_MAX_DL;
    }
    return n;
}

static int running_count(const vs_downloads *d) {
    int n = 0;
    for (int i = 0; i < d->n; i++)
        if (d->jobs[i].state == VS_DL_ANCHOR || d->jobs[i].state == VS_DL_RUNNING)
            n++;
    return n;
}

int vs_dl_add(vs_downloads *d, vs_platform plat, const char *id,
              const char *channel, const char *title, const char *page_url,
              double dur) {
    if (!id || !id[0] || !page_url || !page_url[0]) return -1;
    int k = vs_dl_find(d, plat, id);
    if (k >= 0) {
        vs_dl_state st = d->jobs[k].state;
        // Already finished or in flight: nothing to do. A failed or cancelled
        // row is restarted in place, which is what pressing download again
        // on a stalled one should mean.
        if (st == VS_DL_QUEUED || st == VS_DL_ANCHOR ||
            st == VS_DL_RUNNING || st == VS_DL_DONE) return -1;
    } else {
        if (d->n >= VS_MAX_DL) {
            snprintf(d->status, sizeof d->status, "too many downloads");
            return -1;
        }
        k = d->n++;
    }

    vs_dl *j = &d->jobs[k];
    if (j->segs) { free(j->segs); j->segs = NULL; }
    if (j->proc) { vs_proc_free(j->proc); j->proc = NULL; }
    memset(j, 0, sizeof *j);
    j->platform = plat;
    j->height   = set_height(d, plat);
    j->dur      = dur;
    norm_id(id, j->id, sizeof j->id);
    snprintf(j->channel,  sizeof j->channel,  "%s", channel ? channel : "");
    snprintf(j->title,    sizeof j->title,    "%s", title ? title : "");
    snprintf(j->page_url, sizeof j->page_url, "%s", page_url);
    vs_dl_paths(&d->set, plat, id, j->media, sizeof j->media,
                j->meta, sizeof j->meta);
    j->est_bytes = vs_dl_estimate(j->height, dur);
    j->state     = VS_DL_QUEUED;
    snprintf(j->status, sizeof j->status, "queued");
    return k;
}

void vs_dl_cancel(vs_downloads *d, int i) {
    if (i < 0 || i >= d->n) return;
    vs_dl *j = &d->jobs[i];
    if (j->proc) {
        // Kill, but leave the part file: starting this download again should
        // carry on rather than begin, which is the whole point of resume.
        vs_proc_kill(j->proc);
        vs_proc_free(j->proc);
        j->proc = NULL;
    }
    j->state = VS_DL_CANCELLED;
    snprintf(j->status, sizeof j->status, "cancelled");
}

void vs_dl_forget(vs_downloads *d, int i) {
    if (i < 0 || i >= d->n) return;
    vs_dl *j = &d->jobs[i];
    if (j->proc) { vs_proc_kill(j->proc); vs_proc_free(j->proc); }
    if (j->segs) free(j->segs);
    if (j->probe) {
        vs_segs_free(j->probe->segs, j->probe->nsegs);
        free(j->probe);
    }
    for (int k = i; k < d->n - 1; k++) d->jobs[k] = d->jobs[k + 1];
    d->n--;
    memset(&d->jobs[d->n], 0, sizeof d->jobs[d->n]);
}

// ------------------------------------------------------------- the work

static int set_height(const vs_downloads *d, vs_platform plat) {
    int p = (int)plat;
    if (p < 0 || p > 2) return 1080;
    return d->set.height[p];
}

// Resolve the VOD far enough to know its anchor. This borrows the ordinary
// source resolver rather than repeating the manifest walk, so a Kick VOD
// whose feed dropped is handled here exactly as it is for playback.
static bool anchor_begin(vs_dl *j) {
    j->probe = (vs_source *)calloc(1, sizeof(vs_source));
    if (!j->probe) {
        snprintf(j->status, sizeof j->status, "out of memory");
        return false;
    }
    j->probe->platform = j->platform;
    j->probe->role     = VS_ROLE_VIDEO;
    snprintf(j->probe->page_url, sizeof j->probe->page_url, "%s", j->page_url);
    if (!vs_resolve_begin(j->probe)) {
        free(j->probe);
        j->probe = NULL;
        snprintf(j->status, sizeof j->status, "could not start the lookup");
        return false;
    }
    j->state = VS_DL_ANCHOR;
    snprintf(j->status, sizeof j->status, "reading the timeline ...");
    return true;
}

// Take the anchor off the probe and let the probe go.
static void anchor_take(vs_dl *j) {
    vs_source *s = j->probe;
    j->pdt_start = s->pdt_start;
    j->nsegs     = s->nsegs;
    if (s->duration > 0) j->dur = s->duration;
    // NOT j->height: that is the quality asked for, and fetch_begin is about
    // to build its format selector from it. Overwriting it here is what made
    // a request for 160p fetch 1080p.
    j->vheight = j->height;
    if (s->height > 0 && (j->vheight == 0 || s->height < j->vheight))
        j->vheight = s->height;
    if (!j->channel[0] && s->channel[0])
        snprintf(j->channel, sizeof j->channel, "%s", s->channel);
    if (j->nsegs > 0 && s->segs) {
        j->segs = (vs_seg *)calloc((size_t)j->nsegs, sizeof(vs_seg));
        if (j->segs)
            for (int i = 0; i < j->nsegs; i++) {
                // Numbers only. The segment urls belong to the probe and
                // mean nothing once the media is a local file; what has to
                // survive is where each segment sits in the concatenated
                // timeline and what wall-clock moment it was recorded at.
                j->segs[i].pos  = s->segs[i].pos;
                j->segs[i].dur  = s->segs[i].dur;
                j->segs[i].pdt  = s->segs[i].pdt;
                j->segs[i].disc = s->segs[i].disc;
            }
        else j->nsegs = 0;
    }
    // Now that both numbers are known the estimate can use the one that
    // will actually arrive.
    j->est_bytes = vs_dl_estimate(j->vheight, j->dur);
    vs_sections_cleanup(s);
    vs_segs_free(s->segs, s->nsegs);
    free(s);
    j->probe = NULL;
}

static bool fetch_begin(vs_dl *j) {
    char extra[256];
    vs_ytdlp_extra(extra, sizeof extra);

    char fmt[64];
    if (j->height > 0) snprintf(fmt, sizeof fmt, "best[height<=?%d]", j->height);
    else               snprintf(fmt, sizeof fmt, "best");

    // --hls-prefer-native is what makes this work with no ffmpeg anywhere on
    // either platform: yt-dlp fetches the segments itself and writes them out
    // back to back. The result is an MPEG-TS stream, which libmpv reads
    // natively, and its timeline is the concatenated one the sidecar's
    // segment table already describes.
    //
    // The progress template is machine-readable on purpose - the human
    // "[download]  3.4% of ~4.52GiB" line is a parsing trap - and carries no
    // spaces, so argument splitting cannot break it. Fragment counts rather
    // than byte totals because a byte total for HLS is usually unknown,
    // while the fragment count is exact.
    char cmd[2048];
    snprintf(cmd, sizeof cmd,
             "%s --no-warnings --no-playlist --newline --no-colors "
             "--hls-prefer-native --continue --fragment-retries 10 "
             "--progress-template "
             "VSDL|%%(progress.downloaded_bytes)s|%%(progress.fragment_index)s"
             "|%%(progress.fragment_count)s|%%(progress.speed)s "
             "-f \"%s\" -o \"%s\" %s \"%s\"",
             vs_ytdlp_cmd(), fmt, j->media, extra, j->page_url);

    char err[256];
    err[0] = 0;
    j->proc = vs_proc_start(cmd, err, sizeof err);
    if (!j->proc) {
        snprintf(j->status, sizeof j->status, "%s",
                 err[0] ? err : "could not start the downloader");
        return false;
    }
    j->state   = VS_DL_RUNNING;
    j->started = vs_now_unix();
    j->olen    = 0;
    snprintf(j->status, sizeof j->status, "downloading ...");
    return true;
}

// One progress line. Unknown fields arrive as "NA", which is not a failure -
// yt-dlp simply has not worked that number out yet.
static void progress_line(vs_dl *j, const char *line) {
    if (strncmp(line, "VSDL|", 5) != 0) return;
    const char *p = line + 5;
    double got = 0, idx = 0, cnt = 0, spd = 0;
    char f[4][64];
    int fi = 0, fo = 0;
    memset(f, 0, sizeof f);
    for (; *p && fi < 4; p++) {
        if (*p == '|') { f[fi][fo] = 0; fi++; fo = 0; continue; }
        if (fo < 63) f[fi][fo++] = *p;
    }
    if (fi < 4) f[fi][fo] = 0;

    got = atof(f[0]);
    idx = atof(f[1]);
    cnt = atof(f[2]);
    spd = atof(f[3]);

    if (got > 0) j->got_bytes = got;
    if (spd > 0) j->rate = spd;
    if (cnt > 0 && idx >= 0) {
        double pct = idx / cnt * 100.0;
        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;
        j->pct = pct;
    } else if (j->est_bytes > 0 && got > 0) {
        // No fragment count: fall back to the estimate, which is 30% high,
        // so cap it short of 100 rather than letting it claim to be finished.
        double pct = got / j->est_bytes * 100.0;
        j->pct = pct > 99 ? 99 : pct;
    }
}

static void drain(vs_dl *j) {
    char buf[8192];
    int got;
    while ((got = vs_proc_read(j->proc, buf, sizeof buf)) > 0) {
        for (int i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                // A read can land mid-line, so a partial line waits in obuf
                // for the next one rather than being parsed as a short one.
                if (j->olen > 0) {
                    j->obuf[j->olen] = 0;
                    progress_line(j, j->obuf);
                    j->olen = 0;
                }
                continue;
            }
            if (j->olen < (int)sizeof j->obuf - 1) j->obuf[j->olen++] = c;
        }
    }
}

static void finish(vs_dl *j, int code) {
    double n = file_size(j->media);
    if (code != 0 || n <= 0) {
        j->state = VS_DL_FAILED;
        // The last line yt-dlp managed is the most useful thing to show, and
        // obuf is holding it if the process died mid-line.
        if (j->olen > 0 && strncmp(j->obuf, "VSDL|", 5) != 0) {
            j->obuf[j->olen] = 0;
            snprintf(j->status, sizeof j->status, "%s", j->obuf);
        } else if (n <= 0) {
            snprintf(j->status, sizeof j->status, "nothing was written");
        } else {
            snprintf(j->status, sizeof j->status, "downloader failed (%d)", code);
        }
        return;
    }
    j->got_bytes = n;
    j->pct       = 100;
    // The sidecar goes last, so a file with a sidecar is always a file whose
    // download finished. A partial media file beside no sidecar is exactly
    // the state that reads back as playable-but-unsynced.
    if (!vs_dl_meta_write(j)) {
        j->state = VS_DL_FAILED;
        snprintf(j->status, sizeof j->status, "saved, but the anchor did not write");
        return;
    }
    j->state = VS_DL_DONE;
    snprintf(j->status, sizeof j->status, "saved");
}

bool vs_dl_poll(vs_downloads *d) {
    bool changed = false;
    for (int i = 0; i < d->n; i++) {
        vs_dl *j = &d->jobs[i];
        switch (j->state) {
        case VS_DL_QUEUED:
            if (running_count(d) >= vs_dl_parallel()) break;
            if (!vs_dir_create(d->set.dir)) {
                j->state = VS_DL_FAILED;
                snprintf(j->status, sizeof j->status,
                         "cannot create the download folder");
            } else if (!anchor_begin(j)) {
                j->state = VS_DL_FAILED;
            }
            changed = true;
            break;

        case VS_DL_ANCHOR:
            if (!j->probe) { j->state = VS_DL_FAILED; changed = true; break; }
            if (!vs_resolve_poll(j->probe)) break;
            if (j->probe->state == VS_LOAD_FAILED) {
                snprintf(j->status, sizeof j->status, "%s", j->probe->status);
                vs_segs_free(j->probe->segs, j->probe->nsegs);
                free(j->probe);
                j->probe = NULL;
                j->state = VS_DL_FAILED;
                changed  = true;
                break;
            }
            if (j->probe->live) {
                // Its manifest is still growing, so any segment table taken
                // now stops before the media does. Everything past that
                // point would have no wall-clock mapping at all. Streaming
                // handles a growing VOD properly; downloading cannot yet.
                snprintf(j->status, sizeof j->status,
                         "still being recorded - stream it instead");
                vs_sections_cleanup(j->probe);
                vs_segs_free(j->probe->segs, j->probe->nsegs);
                free(j->probe);
                j->probe = NULL;
                j->state = VS_DL_FAILED;
                changed  = true;
                break;
            }
            anchor_take(j);
            if (j->pdt_start <= 0) {
                // Without this the download would still produce a playable
                // file, and it could never be lined up against anything.
                // Better to refuse than to save something that looks fine.
                j->state = VS_DL_FAILED;
                snprintf(j->status, sizeof j->status,
                         "no timeline in the manifest; cannot be synced");
            } else if (!fetch_begin(j)) {
                j->state = VS_DL_FAILED;
            }
            changed = true;
            break;

        case VS_DL_RUNNING: {
            if (!j->proc) { j->state = VS_DL_FAILED; changed = true; break; }
            double was = j->pct;
            drain(j);
            int code = 0;
            if (vs_proc_done(j->proc, &code)) {
                drain(j);           // whatever was still in the pipe
                vs_proc_free(j->proc);
                j->proc = NULL;
                finish(j, code);
                changed = true;
            } else if (j->pct != was) {
                changed = true;
            }
            break;
        }
        default: break;
        }
    }
    return changed;
}

// ------------------------------------------------------------- sidecar

bool vs_dl_meta_write(const vs_dl *j) {
    // Built in memory and written once. A sidecar half on disk would be
    // read back as a valid anchor with a short segment table, which aligns
    // confidently to the wrong moment - the one failure worth engineering
    // against here.
    int cap = 4096 + j->nsegs * 64;
    char *buf = (char *)malloc((size_t)cap);
    if (!buf) return false;
    int n = snprintf(buf, (size_t)cap,
                     "vodsync-vod %d\n"
                     "platform %s\n"
                     "id %s\n"
                     "channel %s\n"
                     "height %d\n"
                     "duration %.3f\n"
                     "pdt_start %.3f\n"
                     "media vod_%s_%s.ts\n"
                     "title %s\n"
                     "nsegs %d\n",
                     VS_DL_META_VER, plat_tag(j->platform), j->id,
                     j->channel, j->vheight > 0 ? j->vheight : j->height,
                     j->dur, j->pdt_start,
                     plat_tag(j->platform), j->id, j->title, j->nsegs);
    for (int i = 0; i < j->nsegs && n < cap - 64; i++)
        n += snprintf(buf + n, (size_t)(cap - n), "s %.3f %.3f %.3f %d\n",
                      j->segs[i].pos, j->segs[i].dur, j->segs[i].pdt,
                      j->segs[i].disc ? 1 : 0);
    bool ok = vs_file_write(j->meta, buf, n, true);
    free(buf);
    return ok;
}

bool vs_dl_meta_read(const char *path, vs_source *s, char *err, int errcap) {
    unsigned char *txt = NULL;
    int len = 0;
    if (!vs_file_read(path, &txt, &len) || !txt) {
        snprintf(err, errcap, "no anchor file beside the video");
        return false;
    }

    int ver = 0, nsegs = 0, height = 0;
    double dur = 0, pdt = 0;
    char plat[8] = { 0 }, id[96] = { 0 }, chan[96] = { 0 };
    vs_seg *segs = NULL;
    int seen = 0;

    char *p = (char *)txt;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        int ln = (int)strlen(p);
        while (ln > 0 && p[ln - 1] == '\r') p[--ln] = 0;

        if (sscanf(p, "vodsync-vod %d", &ver) == 1) {
            // A version this build does not know could mean anything, and a
            // misread anchor is worse than none: it syncs, and it is wrong.
            if (ver != VS_DL_META_VER) {
                snprintf(err, errcap, "anchor file version %d is not readable", ver);
                free(txt);
                free(segs);
                return false;
            }
        } else if (strncmp(p, "platform ", 9) == 0) {
            snprintf(plat, sizeof plat, "%s", p + 9);
        } else if (strncmp(p, "id ", 3) == 0) {
            snprintf(id, sizeof id, "%s", p + 3);
        } else if (strncmp(p, "channel ", 8) == 0) {
            snprintf(chan, sizeof chan, "%s", p + 8);
        } else if (sscanf(p, "height %d", &height) == 1) {
        } else if (sscanf(p, "duration %lf", &dur) == 1) {
        } else if (sscanf(p, "pdt_start %lf", &pdt) == 1) {
        } else if (sscanf(p, "nsegs %d", &nsegs) == 1) {
            if (nsegs < 0 || nsegs > 1000000) { nsegs = 0; }
            if (nsegs > 0 && !segs)
                segs = (vs_seg *)calloc((size_t)nsegs, sizeof(vs_seg));
        } else if (p[0] == 's' && p[1] == ' ' && segs && seen < nsegs) {
            double pos = 0, sd = 0, spdt = 0;
            int disc = 0;
            if (sscanf(p, "s %lf %lf %lf %d", &pos, &sd, &spdt, &disc) == 4) {
                segs[seen].pos  = pos;
                segs[seen].dur  = sd;
                segs[seen].pdt  = spdt;
                segs[seen].disc = disc != 0;
                seen++;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    free(txt);

    if (ver != VS_DL_META_VER || pdt <= 0) {
        free(segs);
        snprintf(err, errcap, "anchor file is not usable");
        return false;
    }
    if (segs && seen != nsegs) {
        // Short table: the file was truncated. Refuse rather than align
        // against a timeline that stops early.
        free(segs);
        snprintf(err, errcap, "anchor file is incomplete (%d of %d segments)",
                 seen, nsegs);
        return false;
    }

    s->platform  = tag_plat(plat);
    s->pdt_start = pdt;
    s->api_start = pdt;
    if (dur    > 0) s->duration = dur;
    if (height > 0) s->height   = height;
    if (chan[0])    snprintf(s->channel, sizeof s->channel, "%s", chan);
    if (id[0])      snprintf(s->vid,     sizeof s->vid,     "%s", id);
    if (s->segs) vs_segs_free(s->segs, s->nsegs);
    s->segs  = segs;
    s->nsegs = seen;
    s->nsect = segs ? vs_sections_count(segs, seen) : 1;
    return true;
}

// ------------------------------------------------------------ play saved

bool vs_dl_load_saved(const vs_dl_settings *set, vs_source *s,
                      vs_platform plat, const char *id,
                      char *err, int errcap) {
    char media[VS_PATH_MAX], meta[VS_PATH_MAX];
    vs_dl_paths(set, plat, id, media, sizeof media, meta, sizeof meta);
    if (file_size(media) <= 0) {
        snprintf(err, errcap, "no saved copy of that VOD");
        return false;
    }
    if (!vs_dl_meta_read(meta, s, err, errcap)) return false;

    // The only thing that makes this a local playback: what mpv is handed.
    // Everything downstream - the timeline, the nudge, the chat alignment -
    // works from pdt_start and segs, which the sidecar has just filled in.
    snprintf(s->manifest, sizeof s->manifest, "%s", media);
    s->live        = false;
    s->seek_refused = false;
    s->state       = VS_LOAD_READY;
    snprintf(s->status, sizeof s->status, "%dp  saved  %.0f min",
             s->height, s->duration / 60.0);
    return true;
}
