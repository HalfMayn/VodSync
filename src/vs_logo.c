// vodsync - the application mark.
//
// Two play triangles in the two platform colours sharing one timeline: two
// streams on one clock, which is what the app does. See vs_logo.h for why
// this is an original mark rather than the platforms' own logos.
//
// There are two renderers for the same geometry. The toolbar uses raylib's
// drawing calls inside the normal frame. The window icon is rasterised on the
// CPU instead: building it with a render texture at start-up left the GPU in
// a state where the SECOND mpv instance could not bring up its decoder, so
// one pane stayed black. Restoring GL state afterwards did not help, so the
// icon path avoids the GPU altogether.
#include "vs_logo.h"
#include "vs_theme.h"
#include <math.h>
#include <stdlib.h>

// Shared geometry, as fractions of the mark's side.
#define LG_RADIUS   0.26f   // badge corner radius
#define LG_TRI_CY   0.43f   // triangle centre line
#define LG_TRI_R    0.205f  // triangle circumradius
#define LG_TRI_X1   0.40f   // purple centre
#define LG_TRI_X2   0.60f   // green centre
#define LG_BAR_X    0.20f
#define LG_BAR_W    0.60f
#define LG_BAR_Y    0.73f
#define LG_BAR_H    0.075f
#define LG_HEAD_X   0.47f
#define LG_HEAD_W   0.07f

static const Color LG_BADGE  = { 22, 22, 28, 255 };
static const Color LG_BORDER = { 58, 60, 72, 255 };
static const Color LG_BAR    = { 150, 154, 170, 255 };
static const Color LG_HEAD   = { 240, 242, 250, 255 };

static Color rl(Clay_Color c) {
    return (Color){ (unsigned char)c.r, (unsigned char)c.g,
                    (unsigned char)c.b, (unsigned char)c.a };
}

// ------------------------------------------------------------- GPU path

void vs_logo_draw(float x, float y, float s, bool badge) {
    if (s < 4) return;

    if (badge) {
        DrawRectangleRounded((Rectangle){ x, y, s, s }, LG_RADIUS, 10, LG_BADGE);
        DrawRectangleRoundedLinesEx((Rectangle){ x, y, s, s }, LG_RADIUS, 10,
                                    s * 0.035f, LG_BORDER);
    }

    // DrawPoly takes a centre and a rotation, side-stepping DrawTriangle's
    // counter-clockwise winding rule.
    float cy = y + s * LG_TRI_CY;
    float r  = s * LG_TRI_R;
    DrawPoly((Vector2){ x + s * LG_TRI_X1, cy }, 3, r, 0.0f, rl(VS_TWITCH));
    DrawPoly((Vector2){ x + s * LG_TRI_X2, cy }, 3, r, 0.0f, rl(VS_KICK));

    float bh = s * LG_BAR_H;
    if (bh < 1) bh = 1;
    DrawRectangleRounded(
        (Rectangle){ x + s * LG_BAR_X, y + s * LG_BAR_Y, s * LG_BAR_W, bh },
        1.0f, 4, LG_BAR);
    float tw = s * LG_HEAD_W;
    if (tw < 1) tw = 1;
    DrawRectangleRounded(
        (Rectangle){ x + s * LG_HEAD_X, y + s * LG_BAR_Y - s * 0.045f,
                     tw, bh + s * 0.09f },
        1.0f, 4, LG_HEAD);
}

// ------------------------------------------------------------- CPU path

// Signed distance to a rounded rectangle; <= 0 is inside.
static float sd_rrect(float px, float py, float cx, float cy,
                      float hw, float hh, float r) {
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    float qx = fabsf(px - cx) - (hw - r);
    float qy = fabsf(py - cy) - (hh - r);
    float ax = qx > 0 ? qx : 0, ay = qy > 0 ? qy : 0;
    float outside = sqrtf(ax * ax + ay * ay);
    float inner = qx > qy ? qx : qy;
    float inside = inner < 0 ? inner : 0;
    return outside + inside - r;
}

static bool in_tri(float px, float py, float cx, float cy, float r) {
    // Equilateral, pointing right: vertices at 0, 120 and 240 degrees.
    const float k = 0.8660254f;   // sin 120
    float x0 = cx + r,        y0 = cy;
    float x1 = cx - r * 0.5f, y1 = cy + r * k;
    float x2 = cx - r * 0.5f, y2 = cy - r * k;
    float d0 = (px - x1) * (y0 - y1) - (x0 - x1) * (py - y1);
    float d1 = (px - x2) * (y1 - y2) - (x1 - x2) * (py - y2);
    float d2 = (px - x0) * (y2 - y0) - (x2 - x0) * (py - y0);
    bool neg = (d0 < 0) || (d1 < 0) || (d2 < 0);
    bool pos = (d0 > 0) || (d1 > 0) || (d2 > 0);
    return !(neg && pos);
}

Image vs_logo_image(int px) {
    if (px < 8) px = 8;
    const int ss = 4;                 // supersample, then resize for smooth edges
    int n = px * ss;
    float s = (float)n;

    Image img = GenImageColor(n, n, BLANK);
    Color *p = (Color *)img.data;
    if (!p) return img;

    float half = s * 0.5f;
    float rad  = s * LG_RADIUS;
    float bw   = s * 0.035f;          // border thickness
    float triR = s * LG_TRI_R;
    float triY = s * LG_TRI_CY;
    float barH = s * LG_BAR_H;
    float headH = barH + s * 0.09f;

    for (int y = 0; y < n; y++) {
        float fy = y + 0.5f;
        for (int x = 0; x < n; x++) {
            float fx = x + 0.5f;
            float d = sd_rrect(fx, fy, half, half, half, half, rad);
            if (d > 0) continue;                       // outside the badge

            Color c = (d > -bw) ? LG_BORDER : LG_BADGE;

            if (in_tri(fx, fy, s * LG_TRI_X1, triY, triR)) c = rl(VS_TWITCH);
            if (in_tri(fx, fy, s * LG_TRI_X2, triY, triR)) c = rl(VS_KICK);

            if (sd_rrect(fx, fy, s * (LG_BAR_X + LG_BAR_W * 0.5f),
                         s * LG_BAR_Y + barH * 0.5f,
                         s * LG_BAR_W * 0.5f, barH * 0.5f, barH * 0.5f) <= 0)
                c = LG_BAR;

            if (sd_rrect(fx, fy, s * (LG_HEAD_X + LG_HEAD_W * 0.5f),
                         s * LG_BAR_Y - s * 0.045f + headH * 0.5f,
                         s * LG_HEAD_W * 0.5f, headH * 0.5f,
                         s * LG_HEAD_W * 0.5f) <= 0)
                c = LG_HEAD;

            p[y * n + x] = c;
        }
    }

    ImageResize(&img, px, px);        // bicubic: gives the edges their antialiasing
    return img;
}

void vs_logo_set_window_icon(void) {
    // Rasterised at each size rather than downscaled from one, so the small
    // ones stay sharp.
    static const int sizes[] = { 16, 24, 32, 48, 64, 256 };
    const int n = (int)(sizeof sizes / sizeof sizes[0]);
    Image icons[6];
    for (int i = 0; i < n; i++) icons[i] = vs_logo_image(sizes[i]);
    SetWindowIcons(icons, n);
    for (int i = 0; i < n; i++) UnloadImage(icons[i]);
}
