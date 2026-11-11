// vodsync - save and restore a set of VODs, their offsets and bookmarks.
#ifndef VS_SESSION_H
#define VS_SESSION_H
#include "vs_app.h"

// Plain-text key/value format so a session file stays hand-editable.
bool vs_session_save(const vs_app *a, const char *path);
bool vs_session_load(vs_app *a, const char *path);

// Default location next to the executable.
void vs_session_default_path(char *out, int cap);
// Where the state is kept between runs, separate from the manual Save
// slot so an autosave never clobbers something you saved on purpose.
void vs_session_autosave_path(char *out, int cap);

#endif
