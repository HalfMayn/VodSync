// vodsync - the application mark, drawn procedurally.
//
// Deliberately NOT the Twitch or Kick logos. Both brands' guidelines prohibit
// incorporating their marks into another product's logo, and doing so would
// imply an endorsement that does not exist. The mark instead uses their two
// brand colours, which is the normal way a third-party tool signals which
// platforms it works with.
//
// It is generated in code rather than shipped as an asset: no binary blob in
// the repo, and it renders crisply at any size including the window icon.
#ifndef VS_LOGO_H
#define VS_LOGO_H
#include "raylib.h"

// Draws the mark into a square of side `size` at (x, y).
// `badge` draws the rounded backing plate; pass false to draw only the symbol.
void  vs_logo_draw(float x, float y, float size, bool badge);

// Renders the mark to an RGBA image of `px` square. Caller calls UnloadImage.
// Must be called after InitWindow: it uses a render texture.
Image vs_logo_image(int px);

// Applies the mark as the window icon at the sizes Windows asks for.
void  vs_logo_set_window_icon(void);

#endif
