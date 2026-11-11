// vodsync - Clay front end.
#ifndef VS_UI_H
#define VS_UI_H
#include "vs_app.h"
#include "clay.h"
#include "raylib.h"

void                   vs_ui_init(Font *fonts, const char *font_path);
// Re-rasterises every font at the new scale. Text is never drawn at a
// size its atlas was not built at, so this reloads rather than stretches.
void                   vs_ui_set_text_scale(vs_app *a, float scale);
// Display scaling, multiplied with the text size. Not part of a session.
void                   vs_ui_set_dpi(vs_app *a, float dpi);
// Selftest readout: stage geometry and which theater chrome is up.
void                   vs_ui_probe_boxes(char *out, int cap);
// Drives one synthetic PiP resize with no mouse, for --pip-drag: latch at
// the first point, then apply at the second. The point is to measure that
// the latched corner does not move, rather than assert it.
void                   vs_ui_pip_drag_begin(vs_app *a, float mx, float my);
void                   vs_ui_pip_drag_to(vs_app *a, float mx, float my);
float                  vs_ui_dpi(void);
// Solo the pane with chat, or restore the previous layout.
void                   vs_ui_toggle_solo(vs_app *a, int pane);
// Exchange the two panels of the split view - left and right, or top and
// bottom when it is stacked. The sources do not move, only which panel each
// is drawn in, so the master, the focus and the chat binding are untouched.
void                   vs_ui_swap_split(vs_app *a);
// Handles input, declares the layout and returns the render commands.
Clay_RenderCommandArray vs_ui_frame(vs_app *a, float dt);
// Called by the renderer for CLAY_RENDER_COMMAND_TYPE_CUSTOM.
void                   vs_ui_draw_custom(void *customData, Clay_BoundingBox box, float radius);
// Renders every active mpv pane into its FBO at that pane's measured size.
// Must be called before BeginDrawing so mpv cannot disturb raylib's batch.
void                   vs_ui_render_video(vs_app *a);
// Opens the channel browser on `channel`, targeting pane `slot`.
void                   vs_ui_browse(const char *channel, int slot);
// Opens it targeting the CHAT instead: the VOD picked binds the chat rather
// than filling a pane. `channel` may be NULL, in which case it seeds itself
// from a loaded Twitch pane if there is one.
void                   vs_ui_browse_chat(vs_app *a, const char *channel);
// Test hook: loads browser row `i` from each platform, exactly as a click
// would. Returns false if the listing is not ready yet.
bool                   vs_ui_browse_pick(vs_app *a, int i);
// Test hook: commits `url` into the chat panel's "chat from another VOD"
// field, as typing it and pressing Enter does.
void                   vs_ui_chat_vod_commit(vs_app *a, const char *url);
// The whole reset: app state, the saved channel list, and the UI state that
// lives in vs_ui.c. Declines, with a toast, while a chat export is running.
void                   vs_ui_reset(vs_app *a);
// How many channels are in the saved list - for the --reset readout, which
// has to be able to see that they went.
int                    vs_ui_channels_n(void);

#endif
