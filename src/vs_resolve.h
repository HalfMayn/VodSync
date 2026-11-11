// vodsync - async VOD resolution (yt-dlp) and HLS timeline fetch.
#ifndef VS_RESOLVE_H
#define VS_RESOLVE_H
#include "vs_app.h"

// Kick off background resolution of s->page_url. False if a job is already running.
bool vs_resolve_begin(vs_source *s);
// Poll from the UI thread; applies results to *s when finished. True on state change.
bool vs_resolve_poll(vs_source *s);
void vs_resolve_cancel(vs_source *s);

// An in-progress VOD keeps growing. Re-fetching its manifest (a single HTTP
// GET - the CloudFront url stays valid) extends the timeline. Note that
// EXT-X-ENDLIST is NOT a reliable "finished" marker: Kick emits it on live
// VODs too, so growth is detected by comparing the VOD's end against now.
bool vs_source_is_growing(const vs_source *s);
bool vs_refresh_begin(vs_source *s);
bool vs_refresh_poll(vs_source *s, double *added_seconds);

// Blocking helpers, also used directly by tests.
bool vs_http_get(const char *url, char **out, int *out_len, char *err, int errcap);
bool vs_parse_manifest(const char *text, vs_seg **segs, int *nsegs,
                       double *pdt_start, double *total, char *err, int errcap);
// Same, but keeps each segment's URL (live streams need them to build a
// playlist of their own). Caller frees with vs_segs_free.
bool vs_parse_manifest_ex(const char *text, bool keep_urls, vs_seg **segs,
                          int *nsegs, double *pdt_start, double *total,
                          char *err, int errcap);
void vs_segs_free(vs_seg *segs, int n);

// ---- VODs that are not one continuous recording ----
// How many continuous sections the parsed segments fall into. 1 means an
// unbroken recording, which is what this app took for granted until a Kick
// VOD with a dropped feed turned up.
int  vs_sections_count(const vs_seg *segs, int nsegs);
// Writes one HLS playlist per section, plus the ffconcat that stitches them
// together, under the path stem `base`; returns the ffconcat's own path in
// `out`. Needs the segment URLs, so the manifest must have been parsed with
// keep_urls.
bool vs_sections_write(const vs_seg *segs, int nsegs, const char *base,
                       char *out, int outcap);
// A stem of its own in the temp directory, for one source's section files.
void vs_sections_base(char *out, int cap);
// Removes the files vs_sections_write wrote.
void vs_sections_cleanup(vs_source *s);

// ---- live streams ----
// Re-read the remote live playlist, append whatever is new to the local one
// mpv is playing, and extend s->duration. Same begin/poll shape as the VOD
// refresh above, but it merges a sliding window instead of expecting the
// manifest to start where it did last time.
bool vs_live_refresh_begin(vs_source *s);
bool vs_live_refresh_poll(vs_source *s, double *added_seconds);
// Remove the playlist file this source wrote.
void vs_live_cleanup(vs_source *s);
// Extra yt-dlp arguments, currently the cookie source. Set
// VODSYNC_YTDLP_COOKIES to a browser name (e.g. "firefox", "chrome") to let
// yt-dlp use your own logged-in session, which is what reaches VODs that are
// restricted to your subscription. Returns "" when unset.
void vs_ytdlp_extra(char *out, int cap);

double vs_iso8601_to_unix(const char *s);
vs_platform vs_platform_of(const char *url);

#endif
