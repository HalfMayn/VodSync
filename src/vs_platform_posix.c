// vodsync - POSIX implementation of the platform layer (Linux, Steam Deck).
//
// Mirrors vs_platform_win32.c: libcurl for HTTP, posix_spawn for yt-dlp,
// pthreads for the background jobs, /proc/self/exe for the install folder.
//
// Nothing here is Steam Deck specific. The Deck is an ordinary x86_64 Linux
// machine as far as this file is concerned; what is Deck specific lives in
// the build script and in the hwdec choice in vs_player.c.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vs_platform.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>      // strcasecmp
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
#include <curl/curl.h>

extern char **environ;

// ------------------------------------------------------------- threads

typedef struct { vs_thread_fn fn; void *arg; } thread_start;

static void *thread_trampoline(void *p) {
    thread_start *t = (thread_start *)p;
    vs_thread_fn fn = t->fn;
    void *arg = t->arg;
    free(t);
    fn(arg);
    return NULL;
}

bool vs_thread_start(vs_thread_fn fn, void *arg) {
    thread_start *t = (thread_start *)malloc(sizeof *t);
    if (!t) return false;
    t->fn = fn;
    t->arg = arg;

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    int rc = pthread_create(&th, &at, thread_trampoline, t);
    pthread_attr_destroy(&at);
    if (rc != 0) { free(t); return false; }
    return true;
}

// Release/acquire rather than relaxed: the worker fills its job struct
// before setting the flag, and the reader must see those writes once it
// reads the flag as set.
void vs_flag_set(vs_flag *f) {
    __atomic_store_n(&f->v, 1L, __ATOMIC_RELEASE);
}

bool vs_flag_get(const vs_flag *f) {
    return __atomic_load_n((volatile long *)&f->v, __ATOMIC_ACQUIRE) != 0;
}

void vs_flag_set_value(vs_flag *f, long v) {
    __atomic_store_n(&f->v, v, __ATOMIC_RELEASE);
}

long vs_flag_value(const vs_flag *f) {
    return __atomic_load_n((volatile long *)&f->v, __ATOMIC_ACQUIRE);
}

long vs_next_id(void) {
    static long counter = 0;
    return __atomic_add_fetch(&counter, 1L, __ATOMIC_RELAXED);
}

// Prefer a yt-dlp shipped beside the executable - the Deck has no system
// Python, and bundling the standalone binary is how the app stays
// self-contained there - then fall back to one on PATH.
const char *vs_ytdlp_cmd(void) {
    static char cmd[VS_PATH_MAX];
    const char *env = getenv("VODSYNC_YTDLP");
    if (env && env[0]) return env;
    if (!cmd[0]) {
        char dir[VS_PATH_MAX];
        vs_exe_dir(dir, sizeof dir);
        char local[VS_PATH_MAX];
        snprintf(local, sizeof local, "%s/yt-dlp", dir);
        if (access(local, X_OK) == 0) snprintf(cmd, sizeof cmd, "\"%s\"", local);
        else                          snprintf(cmd, sizeof cmd, "yt-dlp");
    }
    return cmd;
}

// ---------------------------------------------------------- subprocess

// Splits a Windows-style command line into argv, honouring double quotes.
// The app builds these strings itself and quotes every argument that can
// contain a space, so this is the exact inverse of how they are written.
// No shell runs, so a URL carrying ; or ` is just an argument.
static int split_args(const char *cmd, char **argv, int max) {
    int n = 0;
    const char *p = cmd;
    char buf[4096];
    while (*p && n < max - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        int len = 0;
        bool quoted = false;
        while (*p && (quoted || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') { quoted = !quoted; p++; continue; }
            if (len < (int)sizeof buf - 1) buf[len++] = *p;
            p++;
        }
        buf[len] = 0;
        argv[n++] = strdup(buf);
    }
    argv[n] = NULL;
    return n;
}

static void free_args(char **argv, int n) {
    for (int i = 0; i < n; i++) free(argv[i]);
}

const char *vs_python_cmd(void) {
    const char *env = getenv("VODSYNC_PYTHON");
    return (env && env[0]) ? env : "python3";
}

bool vs_proc_capture(const char *cmdline, char **out, int *out_len,
                     int timeout_ms, char *err, int errcap) {
    return vs_proc_capture_code(cmdline, out, out_len, NULL,
                                timeout_ms, err, errcap);
}

bool vs_proc_capture_code(const char *cmdline, char **out, int *out_len,
                          int *exit_code, int timeout_ms,
                          char *err, int errcap) {
    char *argv[64];
    int argc = split_args(cmdline, argv, 64);
    if (argc == 0) {
        snprintf(err, errcap, "empty command");
        return false;
    }

    int fds[2];
    if (pipe(fds) != 0) {
        free_args(argv, argc);
        snprintf(err, errcap, "pipe failed: %s", strerror(errno));
        return false;
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, fds[1]);

    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        snprintf(err, errcap, "cannot start %s: %s", argv[0], strerror(rc));
        free_args(argv, argc);
        return false;
    }

    int cap = 1 << 16, len = 0;
    char *buf = (char *)malloc(cap);
    bool timed_out = false;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (long)((now.tv_sec - t0.tv_sec) * 1000 +
                              (now.tv_nsec - t0.tv_nsec) / 1000000);
        long left = timeout_ms - elapsed;
        if (left <= 0) { timed_out = true; break; }

        struct pollfd pf = { fds[0], POLLIN, 0 };
        int pr = poll(&pf, 1, left > 1000 ? 1000 : (int)left);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;

        if (len + 4096 + 1 > cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
        ssize_t got = read(fds[0], buf + len, 4096);
        if (got < 0) { if (errno == EINTR) continue; break; }
        if (got == 0) break;              // child closed stdout: it is done
        len += (int)got;
    }
    buf[len] = 0;
    close(fds[0]);

    if (timed_out) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        free(buf);
        snprintf(err, errcap, "%s timed out", argv[0]);
        free_args(argv, argc);
        return false;
    }

    int status = 0;
    waitpid(pid, &status, 0);
    free_args(argv, argc);

    *out = buf;
    *out_len = len;
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    if (exit_code) *exit_code = code;
    if (code != 0) {
        const char *tail = (len > 220) ? buf + len - 220 : buf;
        snprintf(err, errcap, "command failed: %s", tail);
        return false;
    }
    return true;
}

// ------------------------------------------- long-running subprocess

struct vs_proc {
    int   fd;
    pid_t pid;
    bool  ended;
    int   code;
};

vs_proc *vs_proc_start(const char *cmdline, char *err, int errcap) {
    char *argv[64];
    int argc = split_args(cmdline, argv, 64);
    if (argc == 0) {
        snprintf(err, errcap, "empty command");
        return NULL;
    }

    int fds[2];
    if (pipe(fds) != 0) {
        free_args(argv, argc);
        snprintf(err, errcap, "pipe failed: %s", strerror(errno));
        return NULL;
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, fds[1]);

    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    free_args(argv, argc);
    if (rc != 0) {
        close(fds[0]);
        snprintf(err, errcap, "cannot start the downloader: %s", strerror(rc));
        return NULL;
    }

    // Non-blocking, so a read on the UI thread with nothing to say returns
    // EAGAIN immediately instead of waiting for yt-dlp to speak.
    int fl = fcntl(fds[0], F_GETFL, 0);
    fcntl(fds[0], F_SETFL, (fl < 0 ? 0 : fl) | O_NONBLOCK);

    struct vs_proc *p = (struct vs_proc *)calloc(1, sizeof *p);
    if (!p) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(fds[0]);
        snprintf(err, errcap, "out of memory");
        return NULL;
    }
    p->fd   = fds[0];
    p->pid  = pid;
    p->code = -1;
    return p;
}

int vs_proc_read(struct vs_proc *p, char *buf, int cap) {
    if (!p || !buf || cap < 2) return 0;
    ssize_t got = read(p->fd, buf, (size_t)cap - 1);
    if (got <= 0) return 0;         // EAGAIN, or the pipe closed
    buf[got] = 0;
    return (int)got;
}

bool vs_proc_done(struct vs_proc *p, int *exit_code) {
    if (!p) return true;
    if (!p->ended) {
        int st = 0;
        pid_t r = waitpid(p->pid, &st, WNOHANG);
        if (r == p->pid) {
            p->ended = true;
            p->code  = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
        } else if (r < 0) {
            // Already reaped, or never ours. Either way there is nothing
            // left to wait for, so do not spin on it every frame.
            p->ended = true;
            p->code  = 1;
        }
    }
    if (p->ended && exit_code) *exit_code = p->code;
    return p->ended;
}

void vs_proc_kill(struct vs_proc *p) {
    // SIGTERM, not SIGKILL: yt-dlp cleans up and leaves its .part file in a
    // state it can resume from, which is what cancelling should leave behind.
    if (p && !p->ended) kill(p->pid, SIGTERM);
}

void vs_proc_free(struct vs_proc *p) {
    if (!p) return;
    if (!p->ended) {
        kill(p->pid, SIGKILL);
        waitpid(p->pid, NULL, 0);
    }
    close(p->fd);
    free(p);
}

// ---------------------------------------------------------------- disk

double vs_disk_free(const char *path) {
    struct statvfs s;
    if (statvfs(path, &s) != 0) return 0;
    // f_bavail, not f_bfree: what this user may actually use, which on a
    // filesystem with reserved blocks is the smaller and honest number.
    return (double)s.f_bavail * (double)s.f_frsize;
}

void vs_videos_dir(char *out, int cap) {
    const char *env = getenv("VODSYNC_DOWNLOAD_DIR");
    if (env && env[0]) { snprintf(out, cap, "%s", env); return; }
    const char *home = getenv("HOME");
    if (home && home[0]) snprintf(out, cap, "%s/Videos/vodsync", home);
    else                 vs_state_dir(out, cap);
}

// ---------------------------------------------------------------- http

typedef struct { char *data; int len, cap; } curl_buf;

static size_t curl_sink(void *p, size_t sz, size_t n, void *user) {
    curl_buf *b = (curl_buf *)user;
    int add = (int)(sz * n);
    if (b->len + add + 1 > b->cap) {
        while (b->len + add + 1 > b->cap) b->cap *= 2;
        b->data = (char *)realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, p, add);
    b->len += add;
    return add;
}

static void curl_init_once(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }

bool vs_http_get(const char *url, char **out, int *out_len, char *err, int errcap) {
    // curl_global_init is not thread safe and these calls come off worker
    // threads, so it happens exactly once however many are racing.
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, curl_init_once);

    CURL *c = curl_easy_init();
    if (!c) { snprintf(err, errcap, "curl_easy_init failed"); return false; }

    curl_buf b = { (char *)malloc(1 << 18), 0, 1 << 18 };
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_sink);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);     // called from worker threads
    curl_easy_setopt(c, CURLOPT_USERAGENT,
                     "Mozilla/5.0 (X11; Linux x86_64)");

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
        snprintf(err, errcap, "manifest request failed: %s",
                 curl_easy_strerror(rc));
        free(b.data);
        return false;
    }
    if (status != 200) {
        snprintf(err, errcap, "manifest HTTP %ld", status);
        free(b.data);
        return false;
    }
    b.data[b.len] = 0;
    *out = b.data;
    *out_len = b.len;
    return true;
}

// --------------------------------------------------------------- paths

void vs_exe_dir(char *out, int cap) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) { snprintf(out, cap, "."); return; }
    exe[n] = 0;
    char *slash = strrchr(exe, '/');
    if (slash) *slash = 0;
    snprintf(out, cap, "%s", exe);
}

// ---------------------------------------------------------------- fonts

// In preference order. Only .ttf: stb_truetype, which is what raylib parses
// with, does not handle CFF outlines, so Fedora's Cantarell-VF.otf would
// load as nothing.
static const char *k_font_names[] = {
    "DejaVuSans.ttf",
    "NotoSans-Regular.ttf",
    "LiberationSans-Regular.ttf",
    "FreeSans.ttf",
    "Arimo-Regular.ttf",
};
#define K_FONT_N ((int)(sizeof k_font_names / sizeof k_font_names[0]))

static bool font_readable(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

typedef struct { char best[VS_PATH_MAX]; int rank; } font_hunt;

// Depth-limited, and it stops early once the first choice turns up. A font
// directory is a few hundred files, so this costs nothing measurable - and
// it only runs when none of the known paths matched.
static void font_scan(const char *dir, int depth, font_hunt *h) {
    if (depth > 4 || h->rank == 0) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;            // and so no .. loop
        char p[VS_PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            font_scan(p, depth + 1, h);
            if (h->rank == 0) break;
            continue;
        }
        for (int i = 0; i < K_FONT_N; i++) {
            if (strcmp(e->d_name, k_font_names[i]) != 0) continue;
            if (i < h->rank) {
                h->rank = i;
                snprintf(h->best, sizeof h->best, "%s", p);
            }
            break;
        }
    }
    closedir(d);
}

bool vs_find_font(char *out, int cap) {
    char p[VS_PATH_MAX];

    // Next to the executable first. A bundled copy is the only one that is
    // certain to be there, and it is what makes an unpacked folder work on
    // a distro nobody has tried yet.
    char dir[VS_PATH_MAX];
    vs_exe_dir(dir, sizeof dir);
    for (int i = 0; i < K_FONT_N; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, k_font_names[i]);
        if (font_readable(p)) { snprintf(out, cap, "%s", p); return true; }
    }

    // Then the places distros are known to use, so the common case costs
    // no directory walk at all.
    static const char *fixed[] = {
        "/usr/share/fonts/TTF/DejaVuSans.ttf",                 // Arch, SteamOS
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",     // Debian, Ubuntu
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",   // Fedora
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",              // Fedora, older
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",   // Fedora
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation-fonts/LiberationSans-Regular.ttf",
    };
    for (int i = 0; i < (int)(sizeof fixed / sizeof fixed[0]); i++)
        if (font_readable(fixed[i])) { snprintf(out, cap, "%s", fixed[i]); return true; }

    // And then go looking, because the list above is exactly the kind of
    // assertion that was wrong in the first place.
    font_hunt h;
    h.best[0] = 0;
    h.rank = K_FONT_N;
    const char *home = getenv("HOME");
    char u1[VS_PATH_MAX], u2[VS_PATH_MAX];
    snprintf(u1, sizeof u1, "%s/.local/share/fonts", home ? home : "/nonexistent");
    snprintf(u2, sizeof u2, "%s/.fonts", home ? home : "/nonexistent");
    const char *roots[] = {
        "/usr/share/fonts", "/usr/local/share/fonts", "/run/host/fonts", u1, u2,
    };
    for (int i = 0; i < (int)(sizeof roots / sizeof roots[0]); i++) {
        font_scan(roots[i], 0, &h);
        if (h.rank == 0) break;
    }
    if (h.best[0]) { snprintf(out, cap, "%s", h.best); return true; }

    if (cap > 0) out[0] = 0;
    return false;
}

void vs_state_dir(char *out, int cap) {
    const char *env = getenv("VODSYNC_STATE_DIR");
    if (env && env[0]) { snprintf(out, cap, "%s", env); mkdir(out, 0755); return; }

    // Beside the binary when that is writable - a folder you copied to the
    // Deck should keep everything together. An installed or read-only copy
    // falls back to the usual per-user location rather than silently
    // failing to save, which is what it did before.
    char exe[VS_PATH_MAX];
    vs_exe_dir(exe, sizeof exe);
    if (access(exe, W_OK) == 0) { snprintf(out, cap, "%s", exe); return; }

    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0]) snprintf(out, cap, "%s/vodsync", xdg);
    else {
        const char *home = getenv("HOME");
        snprintf(out, cap, "%s/.local/share/vodsync", home ? home : ".");
        char parent[VS_PATH_MAX];
        snprintf(parent, sizeof parent, "%s/.local", home ? home : ".");
        mkdir(parent, 0755);
        snprintf(parent, sizeof parent, "%s/.local/share", home ? home : ".");
        mkdir(parent, 0755);
    }
    mkdir(out, 0755);
}

void vs_temp_dir(char *out, int cap) {
    const char *d = getenv("XDG_RUNTIME_DIR");
    if (!d || !d[0]) d = getenv("TMPDIR");
    if (!d || !d[0]) d = "/tmp";
    snprintf(out, cap, "%s/", d);
}

char vs_path_sep(void) { return '/'; }

// --------------------------------------------------------------- files

bool vs_file_write(const char *path, const void *data, int len, bool create) {
    int flags = O_WRONLY | O_CREAT | (create ? O_TRUNC : O_APPEND);
    int fd = open(path, flags, 0644);
    if (fd < 0) return false;
    // One write: a reader (mpv, re-reading the live playlist) sees either
    // the old file or the whole new tail, never half a line.
    ssize_t wrote = write(fd, data, (size_t)len);
    close(fd);
    return wrote == (ssize_t)len;
}

bool vs_dir_create(const char *path) {
    if (mkdir(path, 0755) == 0) return true;
    return errno == EEXIST;
}

bool vs_file_read(const char *path, unsigned char **out, int *out_len) {
    *out = NULL;
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return false; }
    unsigned char *buf = (unsigned char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return false; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    *out = buf;
    *out_len = (int)got;
    return true;
}

void vs_file_delete(const char *path) { unlink(path); }

// ---------------------------------------------------------------- time

double vs_now_unix(void) { return (double)time(NULL); }

void vs_gmtime(double unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    gmtime_r(&t, out);
}

void vs_localtime(double unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    localtime_r(&t, out);
}

char *vs_strdup(const char *s) { return strdup(s); }

int vs_stricmp(const char *a, const char *b) { return strcasecmp(a, b); }

static volatile sig_atomic_t g_quit;

static void on_quit(int sig) { (void)sig; g_quit = 1; }

void vs_install_quit_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_quit;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
}

bool vs_quit_requested(void) { return g_quit != 0; }

void vs_sleep_ms(int ms) {
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}
