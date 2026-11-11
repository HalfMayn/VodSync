// vodsync - the only places the app touches the operating system.
//
// Everything else in the app is portable C plus raylib, Clay and libmpv. The
// Windows-only code was never more than four concerns - fetching a URL,
// running yt-dlp and reading its output, starting a thread, and finding a
// path - so they live here, implemented once per platform:
//
//   vs_platform_win32.c   WinHTTP, CreateProcess, CreateThread, Interlocked
//   vs_platform_posix.c   libcurl, posix_spawn, pthreads, C11 atomics
//
// The Windows implementation is the existing code moved, not rewritten: it
// is the version in daily use and there is nothing to gain by disturbing it.
#ifndef VS_PLATFORM_H
#define VS_PLATFORM_H
#include <stdbool.h>
#include <time.h>

#define VS_PATH_MAX 512

// ---- threads ------------------------------------------------------------
// One shape covers every background job in the app: start it, let it set a
// flag when it is finished, and poll that flag from the UI thread.
typedef void (*vs_thread_fn)(void *arg);
bool vs_thread_start(vs_thread_fn fn, void *arg);

// A flag written by a worker and read by the UI thread. Not a mutex: the
// worker fills its job struct, then sets the flag last, and the reader
// touches the struct only once the flag reads true.
typedef struct { volatile long v; } vs_flag;
void vs_flag_set(vs_flag *f);
bool vs_flag_get(const vs_flag *f);
// The same word used as a counter rather than a flag: the channel browser
// publishes how many timestamp lookups have finished as it goes.
void vs_flag_set_value(vs_flag *f, long v);
long vs_flag_value(const vs_flag *f);

// A counter that is safe to call from any thread; used for unique filenames.
long vs_next_id(void);

// How to invoke yt-dlp. On Windows it is the module in the Python on PATH;
// elsewhere, and on the Deck, the standalone binary shipped next to the app.
// VODSYNC_YTDLP overrides it outright.
const char *vs_ytdlp_cmd(void);

// ---- subprocess ---------------------------------------------------------
// Runs a command line with no console window and captures stdout+stderr.
// The line is split on spaces, honouring double quotes, and executed
// directly - no shell is involved, so a URL carrying shell metacharacters
// cannot run anything. Caller frees *out.
bool vs_proc_capture(const char *cmdline, char **out, int *out_len,
                     int timeout_ms, char *err, int errcap);
// Same, and reports the child's exit status, which the chat exporter uses to
// tell "no exporter installed" (2) from an export that genuinely failed.
bool vs_proc_capture_code(const char *cmdline, char **out, int *out_len,
                          int *exit_code, int timeout_ms,
                          char *err, int errcap);

// The interpreter for the bundled helper scripts. VODSYNC_PYTHON overrides.
const char *vs_python_cmd(void);

// ---- long-running subprocess -------------------------------------------
// The capture calls above wait for the child and return its whole output,
// which suits a yt-dlp lookup that takes a second. A download does not fit
// that shape: it runs for minutes, its progress matters while it happens
// rather than afterwards, and it has to be possible to stop. Same no-shell,
// no-console rules as vs_proc_capture.
typedef struct vs_proc vs_proc;
vs_proc *vs_proc_start(const char *cmdline, char *err, int errcap);
// Whatever the child has written since the last call, NUL-terminated. 0 when
// it has written nothing yet, which is the usual answer and not an error.
// Never blocks, so this is safe to call every frame.
int  vs_proc_read(vs_proc *p, char *buf, int cap);
// True once the child has exited; *exit_code is meaningful from then on.
// Output can still be waiting in the pipe when this first reads true, so
// drain vs_proc_read until it returns 0 before deciding what happened.
bool vs_proc_done(vs_proc *p, int *exit_code);
void vs_proc_kill(vs_proc *p);
void vs_proc_free(vs_proc *p);      // kills it first if it is still running

// ---- disk ---------------------------------------------------------------
// Free bytes on the volume holding `path`, or 0 when it cannot be told.
// `path` must exist, so create a download folder before asking about it.
double vs_disk_free(const char *path);

// Where downloaded VODs go unless the user says otherwise: their own videos
// folder. Deliberately NOT the state directory - that holds sessions and
// chat logs, the uninstaller steps carefully around it, and it is the wrong
// home for tens of gigabytes of video. VODSYNC_DOWNLOAD_DIR overrides it,
// which is also how the tests keep their downloads off the real folder.
void vs_videos_dir(char *out, int cap);

// ---- http ---------------------------------------------------------------
// GET into a NUL-terminated buffer the caller frees. Used for HLS manifests,
// which are small; there is no streaming interface here on purpose.
bool vs_http_get(const char *url, char **out, int *out_len,
                 char *err, int errcap);

// ---- paths --------------------------------------------------------------
void vs_exe_dir(char *out, int cap);    // folder holding the executable
// Where saved channels, sessions and chat logs are written. The exe's
// own folder when that is writable, which keeps a portable copy
// self-contained; otherwise the platform's per-user data directory.
void vs_state_dir(char *out, int cap);

// A readable UI font, or false if none was found. Asked for by NAME rather
// than by path: the previous hardcoded list of five paths matched nothing
// at all on Fedora, which files fonts under the package that provided them
// (/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf), and the app then
// fell back to raylib's built-in 10px bitmap font without saying so. No
// amount of text scaling makes that readable.
bool vs_find_font(char *out, int cap);
void vs_temp_dir(char *out, int cap);   // with a trailing separator
char vs_path_sep(void);

// ---- files --------------------------------------------------------------
// Append in one write, or create the file. One write matters: mpv re-reads
// the live playlist while the app extends it, and a reader must never see
// half a line. See the live streams section of NOTES.md.
bool vs_file_write(const char *path, const void *data, int len, bool create);
void vs_file_delete(const char *path);
// Make one directory. True if it exists afterwards, however it got there, so
// the caller does not have to tell "created" from "was already there".
bool vs_dir_create(const char *path);
// Read a whole file. Binary safe, NUL-terminated one past the end for the
// convenience of callers that want text. Caller frees *out.
bool vs_file_read(const char *path, unsigned char **out, int *out_len);

// ---- time ---------------------------------------------------------------
double vs_now_unix(void);
void   vs_gmtime(double unix_s, struct tm *out);
void   vs_localtime(double unix_s, struct tm *out);
char  *vs_strdup(const char *s);
// Case-insensitive compare: _stricmp on Windows, strcasecmp elsewhere.
int    vs_stricmp(const char *a, const char *b);
void   vs_sleep_ms(int ms);     // only the headless tests poll

// SIGTERM is how a session manager - Steam's Game Mode included - asks
// an application to stop. Caught, so the frame loop can end normally
// and the session is written; uncaught, everything since the last
// autosave is lost.
void   vs_install_quit_handler(void);
bool   vs_quit_requested(void);

#endif
