// vodsync - Windows implementation of the platform layer.
//
// This is the code that was spread through vs_resolve.c, vs_browse.c and
// vs_chat.c, moved rather than rewritten: it is the build in daily use.
#include "vs_platform.h"
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------- threads

typedef struct { vs_thread_fn fn; void *arg; } thread_start;

static DWORD WINAPI thread_trampoline(LPVOID p) {
    thread_start *t = (thread_start *)p;
    vs_thread_fn fn = t->fn;
    void *arg = t->arg;
    free(t);
    fn(arg);
    return 0;
}

bool vs_thread_start(vs_thread_fn fn, void *arg) {
    thread_start *t = (thread_start *)malloc(sizeof *t);
    if (!t) return false;
    t->fn = fn;
    t->arg = arg;
    HANDLE h = CreateThread(NULL, 0, thread_trampoline, t, 0, NULL);
    if (!h) { free(t); return false; }
    CloseHandle(h);
    return true;
}

void vs_flag_set(vs_flag *f)       { InterlockedExchange(&f->v, 1); }
bool vs_flag_get(const vs_flag *f) {
    return InterlockedCompareExchange((volatile LONG *)&f->v, 1, 1) != 0;
}

void vs_flag_set_value(vs_flag *f, long v) { InterlockedExchange(&f->v, v); }
long vs_flag_value(const vs_flag *f) {
    return (long)InterlockedCompareExchange((volatile LONG *)&f->v, 0, 0);
}

long vs_next_id(void) {
    static volatile LONG counter = 0;
    return (long)InterlockedIncrement(&counter);
}

static bool path_exists(const char *path) {
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

// Prefer a yt-dlp shipped beside the executable, then one on PATH, and only
// then the module inside whatever python happens to be installed.
//
// The posix build has looked beside the exe from the start, because the Deck
// has no system python. This returned "python -m yt_dlp" and nothing else,
// so an installed copy on a machine without python AND the yt_dlp module
// could not resolve a single VOD - and it failed the way a dead url fails,
// with nothing on screen to say why. The python form is kept as the last
// resort so a working development machine behaves exactly as before.
const char *vs_ytdlp_cmd(void) {
    static char cmd[VS_PATH_MAX];
    const char *env = getenv("VODSYNC_YTDLP");
    if (env && env[0]) return env;
    if (!cmd[0]) {
        char dir[VS_PATH_MAX], local[VS_PATH_MAX], found[VS_PATH_MAX];
        vs_exe_dir(dir, sizeof dir);
        snprintf(local, sizeof local, "%s\\yt-dlp.exe", dir);
        DWORD n = SearchPathA(NULL, "yt-dlp.exe", NULL,
                              (DWORD)sizeof found, found, NULL);
        if (path_exists(local))            snprintf(cmd, sizeof cmd, "\"%s\"", local);
        else if (n > 0 && n < sizeof found) snprintf(cmd, sizeof cmd, "\"%s\"", found);
        else                                snprintf(cmd, sizeof cmd, "python -m yt_dlp");
    }
    return cmd;
}

// ---------------------------------------------------------- subprocess

const char *vs_python_cmd(void) {
    const char *env = getenv("VODSYNC_PYTHON");
    return (env && env[0]) ? env : "python";
}

bool vs_proc_capture(const char *cmdline, char **out, int *out_len,
                     int timeout_ms, char *err, int errcap) {
    return vs_proc_capture_code(cmdline, out, out_len, NULL,
                                timeout_ms, err, errcap);
}

bool vs_proc_capture_code(const char *cmdline, char **out, int *out_len,
                          int *exit_code, int timeout_ms,
                          char *err, int errcap) {
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        snprintf(err, errcap, "CreatePipe failed");
        return false;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb         = sizeof si;
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.hStdInput  = NULL;

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);

    char *mut = _strdup(cmdline);
    BOOL started = CreateProcessA(NULL, mut, NULL, NULL, TRUE,
                                  CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(mut);
    CloseHandle(wr);
    if (!started) {
        CloseHandle(rd);
        snprintf(err, errcap, "cannot start yt-dlp (is python on PATH?)");
        return false;
    }

    int cap = 1 << 16, len = 0;
    char *buf = (char *)malloc(cap);
    DWORD deadline = GetTickCount() + (DWORD)timeout_ms;
    for (;;) {
        if (len + 4096 + 1 > cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
        DWORD got = 0;
        if (!ReadFile(rd, buf + len, 4096, &got, NULL) || got == 0) break;
        len += (int)got;
        if (GetTickCount() > deadline) {
            TerminateProcess(pi.hProcess, 1);
            snprintf(err, errcap, "yt-dlp timed out");
            CloseHandle(rd); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            free(buf);
            return false;
        }
    }
    buf[len] = 0;
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    *out = buf;
    *out_len = len;
    if (exit_code) *exit_code = (int)code;
    if (code != 0) {
        const char *tail = (len > 220) ? buf + len - 220 : buf;
        snprintf(err, errcap, "yt-dlp failed: %s", tail);
        return false;
    }
    return true;
}

// ------------------------------------------- long-running subprocess

struct vs_proc {
    HANDLE rd, proc;
    bool   ended;
    int    code;
};

vs_proc *vs_proc_start(const char *cmdline, char *err, int errcap) {
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        snprintf(err, errcap, "CreatePipe failed");
        return NULL;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb         = sizeof si;
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.hStdInput  = NULL;

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);

    char *mut = _strdup(cmdline);
    BOOL started = CreateProcessA(NULL, mut, NULL, NULL, TRUE,
                                  CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(mut);
    CloseHandle(wr);
    if (!started) {
        CloseHandle(rd);
        snprintf(err, errcap, "cannot start the downloader");
        return NULL;
    }
    CloseHandle(pi.hThread);

    struct vs_proc *p = (struct vs_proc *)calloc(1, sizeof *p);
    if (!p) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(rd);
        snprintf(err, errcap, "out of memory");
        return NULL;
    }
    p->rd   = rd;
    p->proc = pi.hProcess;
    p->code = -1;
    return p;
}

int vs_proc_read(struct vs_proc *p, char *buf, int cap) {
    if (!p || !buf || cap < 2) return 0;
    // PeekNamedPipe first is the whole point of this function: ReadFile on a
    // pipe with nothing in it blocks until the child writes something, which
    // on the UI thread means a frozen frame for as long as yt-dlp is quiet.
    DWORD avail = 0;
    if (!PeekNamedPipe(p->rd, NULL, 0, NULL, &avail, NULL)) return 0;
    if (avail == 0) return 0;
    DWORD want = (DWORD)(cap - 1);
    if (avail < want) want = avail;
    DWORD got = 0;
    if (!ReadFile(p->rd, buf, want, &got, NULL) || got == 0) return 0;
    buf[got] = 0;
    return (int)got;
}

bool vs_proc_done(struct vs_proc *p, int *exit_code) {
    if (!p) return true;
    if (!p->ended) {
        // WaitForSingleObject rather than testing GetExitCodeProcess against
        // STILL_ACTIVE: STILL_ACTIVE is 259, so a child that genuinely exits
        // with code 259 would look like it was running forever.
        if (WaitForSingleObject(p->proc, 0) == WAIT_OBJECT_0) {
            DWORD code = 1;
            GetExitCodeProcess(p->proc, &code);
            p->ended = true;
            p->code  = (int)code;
        }
    }
    if (p->ended && exit_code) *exit_code = p->code;
    return p->ended;
}

void vs_proc_kill(struct vs_proc *p) {
    if (p && !p->ended) TerminateProcess(p->proc, 1);
}

void vs_proc_free(struct vs_proc *p) {
    if (!p) return;
    if (!p->ended) TerminateProcess(p->proc, 1);
    CloseHandle(p->rd);
    CloseHandle(p->proc);
    free(p);
}

// ---------------------------------------------------------------- disk

double vs_disk_free(const char *path) {
    ULARGE_INTEGER avail;
    avail.QuadPart = 0;
    if (!GetDiskFreeSpaceExA(path, &avail, NULL, NULL)) return 0;
    return (double)avail.QuadPart;
}

void vs_videos_dir(char *out, int cap) {
    const char *env = getenv("VODSYNC_DOWNLOAD_DIR");
    if (env && env[0]) { snprintf(out, cap, "%s", env); return; }
    // SHGetKnownFolderPath would mean linking shell32 for one path. The
    // profile plus Videos is the same folder on every Windows this runs on,
    // and a redirected Videos folder is still reachable through the profile.
    const char *home = getenv("USERPROFILE");
    if (home && home[0]) snprintf(out, cap, "%s\\Videos\\vodsync", home);
    else                 vs_state_dir(out, cap);
}

// ---------------------------------------------------------------- http

bool vs_http_get(const char *url, char **out, int *out_len, char *err, int errcap) {
    wchar_t wurl[4096];
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 4096);

    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof uc);
    wchar_t host[512], path[4096];
    host[0] = 0; path[0] = 0;
    uc.dwStructSize   = sizeof uc;
    uc.lpszHostName   = host;  uc.dwHostNameLength = 511;
    uc.lpszUrlPath    = path;  uc.dwUrlPathLength  = 4095;
    uc.dwSchemeLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        snprintf(err, errcap, "bad manifest url");
        return false;
    }

    HINTERNET ses = WinHttpOpen(L"vodsync/0.1",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { snprintf(err, errcap, "WinHttpOpen failed"); return false; }
    WinHttpSetTimeouts(ses, 10000, 10000, 20000, 30000);

    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!con) {
        WinHttpCloseHandle(ses);
        snprintf(err, errcap, "connect to manifest host failed");
        return false;
    }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(con, L"GET", path, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
        snprintf(err, errcap, "open request failed");
        return false;
    }

    static const wchar_t *hdr =
        L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)\r\n";
    BOOL ok = WinHttpSendRequest(req, hdr, (DWORD)-1,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(req, NULL);
    if (!ok) {
        snprintf(err, errcap, "manifest request failed (win32 %lu)", GetLastError());
        WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
        return false;
    }

    DWORD status = 0, slen = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen,
                        WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        snprintf(err, errcap, "manifest HTTP %lu", status);
        WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
        return false;
    }

    int cap = 1 << 18, len = 0;
    char *buf = (char *)malloc(cap);
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        if (len + (int)avail + 1 > cap) {
            while (len + (int)avail + 1 > cap) cap *= 2;
            buf = (char *)realloc(buf, cap);
        }
        DWORD got = 0;
        if (!WinHttpReadData(req, buf + len, avail, &got) || got == 0) break;
        len += (int)got;
    }
    buf[len] = 0;
    WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);

    *out = buf;
    *out_len = len;
    return true;
}

// --------------------------------------------------------------- paths

void vs_exe_dir(char *out, int cap) {
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *slash = strrchr(exe, '\\');
    if (slash) *slash = 0;
    snprintf(out, cap, "%s", exe);
}

// Can this directory actually be written to? On Windows that cannot be
// answered by looking at the directory: Program Files denies writes through
// an ACL rather than the read-only attribute, so _access happily reports it
// as writable. Creating a file is the only honest test. DELETE_ON_CLOSE
// means no probe file is left behind even if the process dies between the
// two calls.
static bool dir_writable(const char *dir) {
    char probe[VS_PATH_MAX];
    snprintf(probe, sizeof probe, "%s\\vodsync.write-test", dir);
    HANDLE h = CreateFileA(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// VODSYNC_STATE_DIR moves everything the app writes - sessions, the saved
// channel list, chat logs - somewhere else. This used to ignore it and
// always answer with the exe's own folder, while the posix build honoured
// it; so every test that set the variable was in fact writing beside the
// exe, and one of them emptied a real saved channel list.
//
// Beside the exe when that is writable, which is how every copy so far has
// worked and keeps a folder you unzipped self-contained. An installed copy
// under Program Files is not writable for a standard user, and this used to
// hand back that folder anyway: the autosave then failed silently, Save
// said "save failed" with no reason, and a saved channel vanished on
// restart. The per-user directory is the same answer the posix build gives
// for a read-only install.
void vs_state_dir(char *out, int cap) {
    const char *env = getenv("VODSYNC_STATE_DIR");
    if (env && env[0]) {
        snprintf(out, cap, "%s", env);
        CreateDirectoryA(out, NULL);    // already there is not an error here
        return;
    }
    // Resolved once per process: the probe writes a file, and this is asked
    // several times a session.
    static char cached[VS_PATH_MAX];
    if (!cached[0]) {
        char exe[VS_PATH_MAX];
        vs_exe_dir(exe, sizeof exe);
        const char *lad = getenv("LOCALAPPDATA");
        if (dir_writable(exe) || !lad || !lad[0]) {
            snprintf(cached, sizeof cached, "%s", exe);
        } else {
            snprintf(cached, sizeof cached, "%s\\vodsync", lad);
            CreateDirectoryA(cached, NULL);
        }
    }
    snprintf(out, cap, "%s", cached);
}

void vs_temp_dir(char *out, int cap) {
    char dir[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, dir)) snprintf(dir, sizeof dir, ".\\");
    snprintf(out, cap, "%s", dir);
}

char vs_path_sep(void) { return '\\'; }

// --------------------------------------------------------------- files

bool vs_file_write(const char *path, const void *data, int len, bool create) {
    HANDLE h = CreateFileA(path,
                           create ? GENERIC_WRITE : FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, create ? CREATE_ALWAYS : OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, data, (DWORD)len, &wrote, NULL);
    CloseHandle(h);
    return ok && (int)wrote == len;
}

bool vs_dir_create(const char *path) {
    if (CreateDirectoryA(path, NULL)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
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

void vs_file_delete(const char *path) { DeleteFileA(path); }

// ---------------------------------------------------------------- time

double vs_now_unix(void) { return (double)_time64(NULL); }

void vs_gmtime(double unix_s, struct tm *out) {
    __time64_t t = (__time64_t)unix_s;
    _gmtime64_s(out, &t);
}

void vs_localtime(double unix_s, struct tm *out) {
    __time64_t t = (__time64_t)unix_s;
    _localtime64_s(out, &t);
}

char *vs_strdup(const char *s) { return _strdup(s); }

int vs_stricmp(const char *a, const char *b) { return _stricmp(a, b); }

// Forward slashes on purpose: the Win32 file APIs accept them, and it keeps
// this list free of escapes.
bool vs_find_font(char *out, int cap) {
    char p[VS_PATH_MAX];
    char dir[VS_PATH_MAX];
    vs_exe_dir(dir, sizeof dir);
    static const char *bundled[] = {
        "DejaVuSans.ttf", "NotoSans-Regular.ttf", "LiberationSans-Regular.ttf",
    };
    static const char *fixed[] = {
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/tahoma.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/verdana.ttf",
    };
    for (int i = 0; i < (int)(sizeof bundled / sizeof bundled[0]); i++) {
        snprintf(p, sizeof p, "%s/%s", dir, bundled[i]);
        FILE *f = fopen(p, "rb");
        if (f) { fclose(f); snprintf(out, cap, "%s", p); return true; }
    }
    for (int i = 0; i < (int)(sizeof fixed / sizeof fixed[0]); i++) {
        FILE *f = fopen(fixed[i], "rb");
        if (f) { fclose(f); snprintf(out, cap, "%s", fixed[i]); return true; }
    }
    if (cap > 0) out[0] = 0;
    return false;
}

void vs_sleep_ms(int ms) { Sleep((DWORD)ms); }

// Closing the window is already a clean exit here, and a console
// handler would not help a windowed app, so this is a no-op.
void vs_install_quit_handler(void) {}
bool vs_quit_requested(void) { return false; }
