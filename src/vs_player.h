// vodsync - libmpv playback into a raylib render texture.
#ifndef VS_PLAYER_H
#define VS_PLAYER_H
#include "vs_app.h"
#include "raylib.h"

bool vs_player_open(vs_source *s);
void vs_player_close(vs_source *s);
void vs_player_poll(vs_source *s);                 // drain events, read time-pos
void vs_player_render(vs_source *s, int w, int h); // render a frame into the FBO
void vs_player_seek(vs_source *s, double pos);
// Re-open the pane's manifest and resume at `at`. For a sectioned VOD whose
// files have been rewritten: ffmpeg's concat demuxer reads its input list
// once, so a timeline that has grown is invisible until it opens again.
void vs_player_reload(vs_source *s, double at);
void vs_player_set_speed(vs_source *s, double speed);
void vs_player_set_volume(vs_source *s, float vol01);
void vs_player_set_paused(vs_source *s, bool paused);

// The texture to draw for this pane, or NULL until the first frame lands.
Texture2D *vs_player_texture(vs_source *s);
bool       vs_player_has_frame(const vs_source *s);
void       vs_player_debug(vs_source *s, char *out, int cap);

#endif
