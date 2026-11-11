// vodsync - headless check: resolve real VODs, build timelines, align them.
// Build/run this before touching the UI; it exercises every risky path.
#include "vs_resolve.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vs_platform.h"

static void fmt_utc(double t, char *out, int cap) {
    long long secs = (long long)t;
    int ms = (int)((t - (double)secs) * 1000.0 + 0.5);
    // inverse of days_from_civil, good enough for display
    long long z = secs / 86400 + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = (long long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    int rem = (int)(secs % 86400);
    snprintf(out, cap, "%04lld-%02u-%02uT%02d:%02d:%02d.%03dZ",
             y, m, d, rem / 3600, (rem % 3600) / 60, rem % 60, ms);
}

typedef struct {
    const char *url;
    vs_source   s;
} entry;

static int load(entry *e) {
    snprintf(e->s.page_url, sizeof e->s.page_url, "%s", e->url);
    e->s.platform = vs_platform_of(e->url);
    printf("[%s] resolving %s\n", vs_platform_name(e->s.platform), e->url);
    if (!vs_resolve_begin(&e->s)) { printf("  begin failed\n"); return 0; }
    for (int i = 0; i < 2400; i++) {          // up to 240s
        if (vs_resolve_poll(&e->s)) break;
        vs_sleep_ms(100);
    }
    if (e->s.state != VS_LOAD_READY) {
        printf("  FAILED: %s\n", e->s.status);
        return 0;
    }
    char a[48], b[48];
    fmt_utc(e->s.pdt_start, a, sizeof a);
    fmt_utc(e->s.pdt_start + e->s.duration, b, sizeof b);
    printf("  ok  %s / %s\n", e->s.channel, e->s.vid);
    printf("      %s\n", e->s.status);
    printf("      window %s -> %s  (%.0fs)\n", a, b, e->s.duration);
    printf("      first seg pdt delta vs start: %+.3fs, last seg pos %.1f\n",
           e->s.segs[0].pdt - e->s.pdt_start,
           e->s.segs[e->s.nsegs - 1].pos);
    return 1;
}

int main(int argc, char **argv) {
    // Self-test the date parser first: a known instant.
    double t = vs_iso8601_to_unix("2026-11-05T18:39:30.847Z");
    printf("iso8601 parse  2026-11-05T18:39:30.847Z -> %.3f\n", t);
    char chk[48]; fmt_utc(t, chk, sizeof chk);
    printf("  round trip   %s  %s\n\n", chk,
           strcmp(chk, "2026-11-05T18:39:30.847Z") == 0 ? "OK" : "MISMATCH");

    entry es[4];
    int n = 0;
    memset(es, 0, sizeof es);
    for (int i = 1; i < argc && n < 4; i++) es[n++].url = argv[i];
    if (n == 0) {
        printf("usage: vs_test_align <vod-url> [<vod-url> ...]\n");
        return 2;
    }

    int ready = 0;
    for (int i = 0; i < n; i++) ready += load(&es[i]);
    printf("\n%d of %d sources ready\n", ready, n);
    if (ready < 2) return ready ? 0 : 1;

    // Union + intersection of coverage.
    double lo = 1e18, hi = -1e18, ilo = -1e18, ihi = 1e18;
    for (int i = 0; i < n; i++) {
        if (es[i].s.state != VS_LOAD_READY) continue;
        double a = es[i].s.pdt_start, b = a + es[i].s.duration;
        if (a < lo) lo = a;
        if (b > hi) hi = b;
        if (a > ilo) ilo = a;
        if (b < ihi) ihi = b;
    }
    char x[48], y[48];
    fmt_utc(lo, x, sizeof x); fmt_utc(hi, y, sizeof y);
    printf("union        %s -> %s\n", x, y);
    if (ihi > ilo) {
        fmt_utc(ilo, x, sizeof x); fmt_utc(ihi, y, sizeof y);
        printf("overlap      %s -> %s  (%.0fs of common wall clock)\n", x, y, ihi - ilo);
        double probe = ilo + (ihi - ilo) * 0.5;
        fmt_utc(probe, x, sizeof x);
        printf("\nat wall clock %s each source plays from:\n", x);
        for (int i = 0; i < n; i++) {
            if (es[i].s.state != VS_LOAD_READY) continue;
            double pos = probe - es[i].s.pdt_start;
            printf("  %-7s %-18s pos %8.2fs  (%.0f%% through)\n",
                   vs_platform_name(es[i].s.platform), es[i].s.channel,
                   pos, 100.0 * pos / es[i].s.duration);
        }
    } else {
        printf("overlap      NONE - these VODs do not share wall-clock time\n");
        printf("             gap is %.0fs\n", ilo - ihi);
    }
    return 0;
}
