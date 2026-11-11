// vodsync - Clay front end: layout modes, transport, timelines, chat.
//
// Interaction model: Clay hit-tests against last frame's boxes, so all input is
// handled at the top of the frame with Clay_PointerOver plus raylib's mouse and
// keyboard state. Nothing is passed to Clay_OnHover, which sidesteps the
// userData-lifetime trap entirely.
#include "vs_ui.h"
#include "vs_emote_img.h"
#include "vs_emote_sets.h"
#include "vs_platform.h"
#include "vs_theme.h"
#include "vs_player.h"
#include "rlgl.h"
#include "vs_chat.h"
#include "vs_session.h"
#include "vs_browse.h"
#include "vs_logo.h"
#include "vs_resolve.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <math.h>

extern void (*Clay_Raylib_CustomHandler)(void *customData, Clay_BoundingBox box, float radius);

// ------------------------------------------------------- per-frame strings

#define VS_STRBUF 96000
static char g_str[VS_STRBUF];
static int  g_stroff;

static Clay_String sfmt(const char *fmt, ...) {
    char *dst = g_str + g_stroff;
    int room = VS_STRBUF - g_stroff - 1;
    if (room < 64) { Clay_String e = { .isStaticallyAllocated = true, .length = 0, .chars = "" }; return e; }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, room, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n > room) n = room;
    g_stroff += n + 1;
    Clay_String s = { .isStaticallyAllocated = false, .length = n, .chars = dst };
    return s;
}

// --------------------------------------------------------- custom payload

typedef enum { VS_CUSTOM_VIDEO = 1, VS_CUSTOM_LOGO,
               VS_CUSTOM_EMOTE, VS_CUSTOM_AVATAR } vs_custom_kind;
typedef struct { vs_custom_kind kind; int index; } vs_custom;
static vs_custom g_custom[VS_MAX_SOURCES];

// Emotes are drawn as custom elements rather than Clay images because an
// animated one needs a SOURCE RECTANGLE - its frames are a vertical strip in
// one texture - and Clay's image command always draws the whole thing.
//
// One arena entry per emote on screen, refilled every frame. Clay keeps the
// pointer it is given until the frame is rendered, so these have to outlive
// layout, and a per-frame arena is the simplest thing that does.
#define VS_EM_DRAW_MAX 768
typedef struct { int emote; unsigned char size; } vs_em_draw;
static vs_em_draw g_em_draw[VS_EM_DRAW_MAX];
static vs_custom  g_em_custom[VS_EM_DRAW_MAX];
static int        g_em_draw_n;
// The emote under the pointer this frame, for the hover.
static int        g_em_hover = -1;
// One per chat source. Unlike the emote arena these are not per frame: an
// image handle is stable for the life of the image.
static vs_custom  g_av_custom[VS_CHAT_SOURCES];
static vs_custom g_logo_custom = { VS_CUSTOM_LOGO, 0 };

// Base size of one picture-in-picture pane, before pip_sx / pip_sy and
// before sc(). The height was 224, which makes a 1.34:1 box - so every
// 16:9 stream was letterboxed with bars top and bottom in the PiP and
// nowhere else. Those bars were the pane's shape, not the video's.
#define VS_PIP_W 300.0f
#define VS_PIP_H 169.0f    // 16:9 at VS_PIP_W, to within half a pixel

static char  g_font_path[320];
static int   g_px[3] = { VS_SIZE_BODY, VS_SIZE_SMALL, VS_SIZE_MICRO };
static float g_scale = 1.0f;   // what the UI is actually drawn at
// The display's own scaling, kept apart from the user's text size so the
// two multiply: 125% text on a 4K screen at 150% is 1.875, and the number
// shown in the toolbar stays the user's 125%. It is never saved in a
// session either - the next machine may well be a different screen.
static float g_dpi = 1.0f;

// Scales a fixed chrome dimension with the text, so buttons and bars
// keep containing their labels.
static int sc(int v) { return (int)(v * g_scale + 0.5f); }

// An overlay panel's width. These were raw pixel constants, which was fine
// while the text size stopped at 300%: the text inside them grew with the
// text scale and the panel did not, so at the top of the range a 430px help
// box was narrower than one of its own lines. Scaled, then capped against
// the window - a FIXED element cannot shrink, so without the cap a large
// text size on a small window would push the panel off the edge, which is
// exactly how the chat column used to disappear.
static float overlay_w(int unscaled) {
    float w   = (float)sc(unscaled);
    float cap = (float)GetScreenWidth() - (float)sc(24);
    if (cap < 160) cap = 160;
    return w > cap ? cap : w;
}

// What a VOD loaded into each slot becomes: a video pane, or the chat
// source (resolved for its timeline anchor, but never given a player).

static vs_browser  g_browser;   // channel browser state
static vs_channels g_channels;  // saved channel list

// Reset throws away every loaded VOD, the chat log and the saved channel
// list. The autosave follows about twenty seconds later and the channel
// file is rewritten at once, so it is effectively permanent, and one stray
// click next to Save and Load is not an acceptable way to reach it. The
// first click arms, the second within the window does it, and the label
// says which state it is in rather than relying on a toast nobody reads.
// Not a modal, because this is not worth a modal.
static double g_reset_armed_until;
#define VS_RESET_ARM_S 4.0
static vs_app *g_app;          // for the custom draw callback
static Font   *g_fonts;
// Last measured size of each pane's video area, used to size the mpv FBOs.
static int     g_pw[VS_MAX_SOURCES], g_ph[VS_MAX_SOURCES];

// ------------------------------------------------------------- hit boxes

static Clay_BoundingBox box_of(Clay_ElementId id, bool *found) {
    Clay_ElementData d = Clay_GetElementData(id);
    if (found) *found = d.found;
    return d.boundingBox;
}

static bool over(Clay_ElementId id) { return Clay_PointerOver(id); }

// The pointer, with a test override: reveals depend on where the mouse
// really is, which a screenshot run cannot arrange.
static Vector2 ui_mouse(void) {
    Vector2 m = GetMousePosition();
    const char *fx = getenv("VODSYNC_FAKE_MOUSE_X");
    const char *fy = getenv("VODSYNC_FAKE_MOUSE_Y");
    if (fx) m.x = (float)atof(fx);
    if (fy) m.y = (float)atof(fy);
    return m;
}

// Is the pointer on the picture-in-picture strip (or just off its edge,
// where its own grips are)? The PiP floats over the stage, so it overlaps
// whatever reveal band it happens to be parked in - near the bottom, by
// default. Everything that reveals on proximity asks this first, so hovering
// the PiP brings up the PiP's own chrome and nothing else.
static bool pointer_over_pip(const vs_app *a) {
    if (a->layout != VS_LAYOUT_PIP) return false;
    bool f;
    Clay_BoundingBox b = box_of(CLAY_ID("PipStrip"), &f);
    if (!f) return false;
    Vector2 m = ui_mouse();
    return m.x >= b.x - 10 && m.x <= b.x + b.width + 10 &&
           m.y >= b.y - 10 && m.y <= b.y + b.height + 10;
}

// Is the who-said-what popup up? Theater mode needs to know - chrome must
// not hide while a panel is open over it - and runs before that state is
// declared, so this is the one thing it asks for ahead of time.
static bool who_is_open(void);

// ---- theater mode ----------------------------------------------------
// The chrome is not dimmed, it is simply not declared, so the video takes the
// space back. Each piece reappears when the pointer is near where it lives.
static bool g_th_top = true, g_th_bottom = true;
// Chrome lingers briefly after the pointer leaves its band. Without it, the
// edge of the band is a knife edge: a pixel of movement there flickers the
// bar on and off while you are trying to aim at it.
#define VS_TH_LINGER 0.6
static double g_th_top_until = 0, g_th_bot_until = 0;

static void theater_update(vs_app *a) {
    if (!a->theater) {
        g_th_top = g_th_bottom = true;
        g_th_top_until = g_th_bot_until = 0;
        return;
    }

    // Anything mid-interaction pins its chrome open. Without this, opening
    // the layout dropdown and moving down to an option takes the pointer out
    // of the reveal band, the toolbar stops being declared, and the menu --
    // which is attached to a button inside it -- vanishes before the click
    // lands. The same applies to a scrubber drag near the bottom edge.
    // Pointing at a bar keeps it up - but not through the PiP. The PiP sits
    // in the bottom corner by default, under where the bar is drawn, and
    // without this the bar raised on the first frame (before the PiP had been
    // laid out and could be tested against) would then hold itself up for as
    // long as the pointer stayed on the PiP.
    bool on_pip = pointer_over_pip(a);
    bool busy_top    = a->layout_menu || a->editing >= 0 || a->show_help ||
                       who_is_open() ||
                       (!on_pip && over(CLAY_ID("TheaterTop")));
    bool busy_bottom = a->drag == 1 ||
                       (a->drag >= 10 && a->drag < 10 + VS_MAX_SOURCES) ||
                       (!on_pip && over(CLAY_ID("TheaterBottom")));

    Vector2 m = ui_mouse();
    // On the PiP, only the PiP answers. Parked in its default corner it sits
    // inside the bottom band, so reaching for it used to raise the timeline
    // and the transport as well - chrome the pointer was never heading for.
    bool near_top    = !on_pip && m.y < (float)sc(78);
    bool near_bottom = !on_pip &&
                       m.y > (float)GetScreenHeight() - (float)sc(180);
    if (near_top    || busy_top)    g_th_top_until = a->now + VS_TH_LINGER;
    if (near_bottom || busy_bottom) g_th_bot_until = a->now + VS_TH_LINGER;
    // Moving onto the PiP drops the linger as well, so the bars go now
    // rather than hanging over it for another half second.
    if (on_pip) { g_th_top_until = 0; g_th_bot_until = 0; }
    g_th_top    = busy_top    || a->now < g_th_top_until;
    g_th_bottom = busy_bottom || a->now < g_th_bot_until;
}

static bool browser_row_clicked(void);

static bool clicked(Clay_ElementId id) {
    return Clay_PointerOver(id) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

// ---------------------------------------------------------------- helpers

// A pane's own header/footer: revealed only near that pane's edge, the way
// a video player does it, rather than whenever the pointer is anywhere over
// the picture.
static bool pane_chrome(vs_app *a, int i, bool top) {
    if (!a->theater) return true;
    if (pointer_over_pip(a)) return false;   // the PiP is on top of this pane
    bool f;
    Clay_BoundingBox b = box_of(CLAY_IDI("Pane", i), &f);
    if (!f) return true;
    Vector2 m = ui_mouse();
    if (m.x < b.x - 4 || m.x > b.x + b.width + 4) return false;
    if (top) return m.y >= b.y - 6 && m.y <= b.y + (float)sc(66);
    float bot = b.y + b.height;
    return m.y <= bot + 6 && m.y >= bot - (float)sc(66);
}

static bool pip_chrome(vs_app *a) {
    if (!a->theater) return true;
    bool f;
    box_of(CLAY_ID("PipStrip"), &f);
    if (!f) return true;          // not laid out yet: show it
    return pointer_over_pip(a);
}

static Clay_Color plat_color(vs_platform p) {
    return p == VS_PLAT_TWITCH ? VS_TWITCH : (p == VS_PLAT_KICK ? VS_KICK : VS_TEXT_DIM);
}

static const char *layout_name(vs_layout l) {
    switch (l) {
        case VS_LAYOUT_SPLIT: return "Split";
        case VS_LAYOUT_GRID:  return "Grid";
        case VS_LAYOUT_PIP:   return "Focus + PiP";
        case VS_LAYOUT_SOLO:  return "Solo + chat";
        case VS_LAYOUT_TRI:   return "2 + 1";
        default:              return "?";
    }
}

// Double-clicking a pane solos it with chat; doing it again restores the
// layout that was in use before.
void vs_ui_toggle_solo(vs_app *a, int pane) {
    if (pane >= 0 && pane < VS_MAX_SOURCES) a->focus = pane;
    if (a->layout != VS_LAYOUT_SOLO) {
        a->prev_layout = a->layout;
        a->layout = VS_LAYOUT_SOLO;
    } else {
        a->layout = a->prev_layout;
    }
}

// Rebuilds the atlases for text size x display scale, if that has moved.
static void apply_scale(vs_app *a) {
    float scale = (a ? a->text_scale : 1.0f) * g_dpi;
    if (scale < 0.5f) scale = 0.5f;
    // The ceiling is text scale TIMES display scale, so on a high-DPI screen
    // it is reached long before the slider runs out: at 250% a text scale of
    // 2.0 asks for 5.0 and used to be cut to 4.0, giving 1.6x the baseline
    // when the slider said 2x. Raised so the whole slider range is usable on
    // a 4K panel rather than silently flattening near the top.
    if (scale > 6.0f) scale = 6.0f;
    if (fabsf(scale - g_scale) < 0.001f && g_px[0]) return;
    g_scale = scale;

    // Rebuild each atlas at the size it will actually be drawn at. Scaling a
    // glyph atlas is what made the text mush the first time round, so the
    // fonts are re-rasterised rather than stretched.
    const int base[3] = { VS_SIZE_BODY, VS_SIZE_SMALL, VS_SIZE_MICRO };
    Font def = GetFontDefault();
    for (int i = 0; i < 3; i++) {
        int px = (int)(base[i] * scale + 0.5f);
        if (px < 8)  px = 8;
        // Was 64, which body text hit exactly at scale 4.0 - so the largest
        // sizes were being clamped twice over, and the top of the range did
        // nothing at all on a high-DPI display.
        if (px > 96) px = 96;
        g_px[i] = px;
        if (!g_font_path[0]) continue;
        Font nf = LoadFontEx(g_font_path, px, 0, 400);
        if (nf.texture.id == 0) continue;
        SetTextureFilter(nf.texture, TEXTURE_FILTER_POINT);
        if (g_fonts[i].texture.id != def.texture.id) UnloadFont(g_fonts[i]);
        g_fonts[i] = nf;
    }
    Clay_ResetMeasureTextCache();   // cached widths refer to the old atlases
}

void vs_ui_set_text_scale(vs_app *a, float scale) {
    if (scale < 0.7f) scale = 0.7f;
    // 2.0 was not enough on its own even at 1x. apply_scale still bounds
    // text scale x display scale, so this cannot run away on a high-DPI
    // screen - it just stops being the binding limit on an ordinary one.
    //
    // Raised again to 5.0, from across a living room. A TV over HDMI
    // commonly reports a display scale of 1.0, so the product apply_scale
    // bounds is never reached and this clamp was the whole limit: 300% of a
    // 16px body font is 48px, which is small at three metres. The ceiling
    // in vs_session.c has to move with it, or a saved 4.0 is rejected on
    // load and silently becomes the default.
    if (scale > 5.0f) scale = 5.0f;
    if (a) a->text_scale = scale;
    apply_scale(a);
}

// Called once at startup with what the window system reports, so a 4K screen
// at 150% does not render the chrome at a third of the size it was designed
// at. Everything the UI draws goes through sc() or font_px_for(), so this one
// number carries the whole layout.
void vs_ui_set_dpi(vs_app *a, float dpi) {
    if (dpi < 0.5f) dpi = 0.5f;
    if (dpi > 4.0f) dpi = 4.0f;
    g_dpi = dpi;
    apply_scale(a);
}

float vs_ui_dpi(void) { return g_dpi; }

// ------------------------------------------------------------ chat search
//
// Filtering what is on screen would be searching sixty messages: the list is
// pinned to the playhead, so the drawn window is a keyhole onto the log. The
// search runs over the whole log instead, and the hits replace the list.
//
// Each hit carries the moment it was said, which is the point: a hit you
// cannot go to is trivia. Clicking one seeks every pane to it.
#define VS_FIND_MAX 500

static struct {
    char query[64];
    int  hits[VS_FIND_MAX];
    int  nhits;
    bool truncated;        // the log had more matches than we kept
    char built_for[64];    // the query these hits were built from, and
    int  built_n;          // the log length at the time
} g_find;

// Case-insensitive substring. ASCII folding only, which is what vs_stricmp
// does on Windows anyway, and enough for the names and words people look
// for; a non-ASCII query still matches exactly.
static const char *find_stristr(const char *hay, const char *needle) {
    if (!needle[0]) return hay;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++; n++;
        }
        if (!*n) return hay;
    }
    return NULL;
}

// Only when the question changed or the log grew. A five hour chat runs to
// tens of thousands of lines, and this would otherwise run every frame.
static void find_rebuild(const vs_chat *c) {
    if (g_find.built_n == c->n &&
        strcmp(g_find.built_for, g_find.query) == 0) return;
    snprintf(g_find.built_for, sizeof g_find.built_for, "%s", g_find.query);
    g_find.built_n   = c->n;
    g_find.nhits     = 0;
    g_find.truncated = false;
    if (!g_find.query[0] || !c->msgs) return;
    for (int i = 0; i < c->n; i++) {
        if (!find_stristr(c->msgs[i].text, g_find.query) &&
            !find_stristr(c->msgs[i].user, g_find.query)) continue;
        if (g_find.nhits >= VS_FIND_MAX) { g_find.truncated = true; break; }
        g_find.hits[g_find.nhits++] = i;
    }
}

// A word-wrapped text still has a minimum width: its longest word. Chat
// supplies words nothing can break - a pasted URL, or thirty emotes typed
// with no spaces - and Clay answers a minimum it cannot meet by widening
// the parent, silently. Measured on a real log that put a 479px list at
// 3643px, which is how a long message ends up with its tail outside the
// column: drawn, correctly, somewhere nobody can see.
//
// So give the wrapper a seam. A space every VS_CHAT_BREAK characters, but
// only inside a run that has none of its own, and only on a UTF-8 boundary
// so a multi-byte glyph is never cut in half.
#define VS_CHAT_BREAK 26

static Clay_String chat_wrappable(const char *src) {
    // vs_chat_msg.text is 512 bytes and a seam costs one byte per 26
    // characters, so 768 cannot be reached, let alone passed.
    char buf[768];
    int o = 0, run = 0;
    for (int i = 0; src[i] && o < (int)sizeof buf - 2; i++) {
        unsigned char c = (unsigned char)src[i];
        bool continuation = (c & 0xC0) == 0x80;
        if (c == ' ' || c == '\t') run = 0;
        else if (!continuation) run++;
        if (run > VS_CHAT_BREAK && !continuation) {
            buf[o++] = ' ';
            run = 1;
        }
        buf[o++] = (char)c;
    }
    buf[o] = 0;
    return sfmt("%s", buf);
}

// ------------------------------------------------ one person's messages
//
// Clicking a name answers "who is this, and what have they been saying?"
// without losing your place. Bounded by the playhead: it shows what they
// had said by the moment you are watching, not what they say later in the
// VOD, which would be a spoiler delivered by the chat panel.
#define VS_WHO_MAX 300

static struct {
    bool open;
    char who[48];
    int  idx[VS_WHO_MAX];    // their messages, ascending, whole log
    int  n;
    bool truncated;          // they said more than we kept
    int  built_n;            // log length this was built from
} g_who;

static bool who_is_open(void) { return g_who.open; }

// How many of their messages the last frame drew, for the probe.
static int g_who_shown;

static void who_close(void) {
    g_who.open = false;
    g_who.who[0] = 0;
    g_who.n = 0;
}

// Built once per name, and again only if the log grows - not per frame.
// The playhead bound is applied when drawing, which costs one comparison
// per message of theirs rather than a scan of the whole log every frame.
static void who_rebuild(const vs_chat *c) {
    if (!g_who.open || g_who.built_n == c->n) return;
    g_who.built_n = c->n;
    g_who.n = 0;
    g_who.truncated = false;
    if (!c->msgs || !g_who.who[0]) return;
    for (int i = 0; i < c->n; i++) {
        if (vs_stricmp(c->msgs[i].user, g_who.who) != 0) continue;
        if (g_who.n < VS_WHO_MAX) {
            g_who.idx[g_who.n++] = i;
            continue;
        }
        // Past the cap, keep the most recent: when a name catches your eye
        // it is what they have been saying lately that you want.
        memmove(g_who.idx, g_who.idx + 1,
                (VS_WHO_MAX - 1) * sizeof g_who.idx[0]);
        g_who.idx[VS_WHO_MAX - 1] = i;
        g_who.truncated = true;
    }
}

static void who_open(const vs_chat *c, const char *user) {
    if (!user || !user[0]) return;
    snprintf(g_who.who, sizeof g_who.who, "%s", user);
    g_who.open = true;
    g_who.built_n = -1;      // force a rebuild for the new name
    who_rebuild(c);
}

// Which message each drawn row shows. A click is handled against the
// PREVIOUS frame's layout, so the mapping has to be recorded as the rows
// are declared rather than worked out again afterwards.
#define VS_ROW_MAX 512
static int g_row_msg[VS_ROW_MAX];
static int g_row_n;

static void find_clear(void) {
    g_find.query[0] = 0;
    g_find.nhits = 0;
    g_find.truncated = false;
    g_find.built_for[0] = 0;
    g_find.built_n = -1;
}

// Where the stage and the PiP ended up, and which chrome is showing. The
// point of theater mode is that revealing chrome does not move the stage, and
// that is a measurement, not an impression: these boxes have to be identical
// with the chrome up and down.
void vs_ui_probe_boxes(char *out, int cap) {
    bool fb, fp, fs;
    Clay_BoundingBox b = box_of(CLAY_ID("Body"), &fb);
    Clay_BoundingBox p = box_of(CLAY_IDI("Pane", 0), &fp);
    Clay_BoundingBox s = box_of(CLAY_ID("PipStrip"), &fs);
    bool fv, fc, fl;
    Clay_BoundingBox v = box_of(CLAY_IDI("Video", 0), &fv);
    // The chat column and its message list, because "the chat disappeared"
    // needs a number to chase: a fixed-width column can be pushed past the
    // right edge without shrinking, and a zero-height list looks the same
    // on screen as no chat at all.
    Clay_BoundingBox c = box_of(CLAY_ID("ChatCol"), &fc);
    Clay_BoundingBox l = box_of(CLAY_ID("ChatList"), &fl);
    bool fg;
    Clay_BoundingBox g = box_of(CLAY_ID("ChatGrip"), &fg);
    // The first row of whichever list is showing. "The list is too wide" is
    // not actionable; "the row is too wide" says where to look.
    bool fr;
    Clay_BoundingBox r = box_of(CLAY_IDI("ChatMsg", 0), &fr);
    if (!fr) r = box_of(CLAY_IDI("ChatHit", 0), &fr);
    // The popup and its first row: a row taller than one line is the only
    // evidence from here that the text inside it wrapped.
    bool fw2, fwr;
    Clay_BoundingBox w2 = box_of(CLAY_ID("WhoList"), &fw2);
    Clay_BoundingBox wr = box_of(CLAY_IDI("WhoMsg", 0), &fwr);
    snprintf(out, cap,
             "body %s(%.0f,%.0f %.0fx%.0f)  pane0 %s(%.0f,%.0f %.0fx%.0f)  "
             "pip %s(%.0f,%.0f %.0fx%.0f)  pipscale %.2fx%.2f  "
             "video0 %s(%.0f,%.0f %.0fx%.0f)  "
             "chat %s(%.0f,%.0f %.0fx%.0f)  chatlist %s(%.0f,%.0f %.0fx%.0f)  "
             "grip %s(%.0f,%.0f %.0fx%.0f)  "
             "row0 %s(%.0f,%.0f %.0fx%.0f)  "
             "find [%s] hits %d%s  "
             "who %s[%s] shown %d (%.0fx%.0f) row %.0fx%.0f  "
             "screen %dx%d scale %.2f  px %d/%d/%d  chrome top=%d bottom=%d",
             fb ? "" : "? ", b.x, b.y, b.width, b.height,
             fp ? "" : "? ", p.x, p.y, p.width, p.height,
             fs ? "" : "? ", s.x, s.y, s.width, s.height,
             (double)(g_app ? g_app->pip_sx : 0.0f),
             (double)(g_app ? g_app->pip_sy : 0.0f),
             fv ? "" : "? ", v.x, v.y, v.width, v.height,
             fc ? "" : "? ", c.x, c.y, c.width, c.height,
             fl ? "" : "? ", l.x, l.y, l.width, l.height,
             fg ? "" : "? ", g.x, g.y, g.width, g.height,
             fr ? "" : "? ", r.x, r.y, r.width, r.height,
             g_find.query, g_find.nhits, g_find.truncated ? "+" : "",
             fw2 ? "" : "? ", g_who.who, g_who_shown,
             w2.width, w2.height, wr.width, wr.height,
             GetScreenWidth(), GetScreenHeight(), g_scale,
             g_px[0], g_px[1], g_px[2],
             g_th_top ? 1 : 0, g_th_bottom ? 1 : 0);
}

void vs_ui_browse(const char *channel, int slot) {
    vs_browse_open(&g_browser, slot);
    vs_browse_begin(&g_browser, channel);
}

void vs_ui_browse_chat(vs_app *a, const char *channel) {
    vs_browse_open(&g_browser, 0);
    g_browser.chat_target = true;
    // Seeded from a Twitch pane when no channel is named: the chat you want
    // is usually the same streamer, so the listing is already there instead
    // of waiting on a name you have to retype.
    if (channel && channel[0]) {
        vs_browse_begin(&g_browser, channel);
    } else if (!g_browser.query[0]) {
        for (int i = 0; i < VS_MAX_SOURCES; i++) {
            vs_source *s = &a->src[i];
            if (s->active && s->platform == VS_PLAT_TWITCH && s->channel[0]) {
                vs_browse_begin(&g_browser, s->channel);
                break;
            }
        }
    }
}

bool vs_ui_browse_pick(vs_app *a, int i) {
    if (g_browser.state != VS_LOAD_READY) return false;
    if (g_browser.chat_target) {
        // The same thing clicking a Twitch row does. Kick rows are not
        // offered here for the reason the click handler gives.
        if (i >= g_browser.ntw) return false;
        char u[VS_URL_MAX];
        snprintf(u, sizeof u, "%s", g_browser.tw[i].url);
        vs_browse_close(&g_browser);
        vs_ui_chat_vod_commit(a, u);
        return true;
    }
    if (i < g_browser.ntw) vs_add_source_at(a, g_browser.tw[i].url, VS_ROLE_VIDEO, 0);
    if (i < g_browser.nkk) vs_add_source_at(a, g_browser.kk[i].url, VS_ROLE_VIDEO, 1);
    vs_browse_close(&g_browser);
    return true;
}

void vs_ui_render_video(vs_app *a) {
    for (int i = 0; i < VS_MAX_SOURCES; i++) {
        vs_source *s = &a->src[i];
        if (!s->active || s->role != VS_ROLE_VIDEO || !s->gpu) continue;
        int w = g_pw[i], h = g_ph[i];
        if (w < 16 || h < 16) continue;           // pane not laid out yet
        if (w > 3840) w = 3840;
        if (h > 2160) h = 2160;
        vs_player_render(s, w, h);
    }
}

void vs_ui_init(Font *fonts, const char *font_path) {
    g_fonts = fonts;
    snprintf(g_font_path, sizeof g_font_path, "%s", font_path ? font_path : "");
    vs_channels_load(&g_channels);
    Clay_Raylib_CustomHandler = vs_ui_draw_custom;
    for (int i = 0; i < VS_MAX_SOURCES; i++) { g_pw[i] = 0; g_ph[i] = 0; }
}

// ------------------------------------------------------- custom draw hook

void vs_ui_draw_custom(void *customData, Clay_BoundingBox box, float radius) {
    (void)radius;
    vs_custom *c = (vs_custom *)customData;
    if (!c || !g_app) return;
    if (c->kind == VS_CUSTOM_LOGO) {
        float s = box.width < box.height ? box.width : box.height;
        vs_logo_draw(box.x + (box.width - s) * 0.5f,
                     box.y + (box.height - s) * 0.5f, s, true);
        return;
    }
    if (c->kind == VS_CUSTOM_AVATAR) {
        const vs_emote_img *im = vs_img_get(c->index);
        if (!im || im->tex.id == 0) return;
        // Avatars are square; anything else is centre-cropped rather than
        // squashed, since a stretched face looks broken and a cropped one
        // does not.
        float side = (float)(im->fw < im->fh ? im->fw : im->fh);
        Rectangle src = { ((float)im->fw - side) * 0.5f,
                          ((float)im->fh - side) * 0.5f, side, side };
        Rectangle dst = { box.x, box.y, box.width, box.height };
        DrawTexturePro(im->tex, src, dst, (Vector2){ 0, 0 }, 0.0f, WHITE);
        return;
    }
    if (c->kind == VS_CUSTOM_EMOTE) {
        if (c->index < 0 || c->index >= g_em_draw_n) return;
        vs_em_draw *d = &g_em_draw[c->index];
        const vs_emote_img *im =
            vs_emote_img_get(d->emote, (vs_emote_size)d->size);
        if (!im || im->tex.id == 0) return;
        // GetTime, not the VOD clock: an emote loops on its own, and tying
        // it to playback would freeze every emote whenever playback paused.
        Rectangle src = vs_emote_img_frame(im, GetTime());
        Rectangle dst = { box.x, box.y, box.width, box.height };
        DrawTexturePro(im->tex, src, dst, (Vector2){ 0, 0 }, 0.0f, WHITE);
        return;
    }
    if (c->kind != VS_CUSTOM_VIDEO) return;
    vs_source *s = &g_app->src[c->index];

    Texture2D *tex = vs_player_texture(s);
    if (!tex || tex->id == 0) return;

    float bw = box.width, bh = box.height;
    if (bw < 2 || bh < 2) return;
    float tw = (float)tex->width, th = (float)tex->height;
    if (tw < 2 || th < 2) return;

    // Negative source height flips the sample: raylib render textures are
    // stored bottom-up, and mpv has already applied its own FLIP_Y, so
    // without this the two cancel out and the picture comes through upside
    // down. That holds for both branches below.
    Rectangle src, dst;
    if (s->fill) {
        // Cover the pane: read the largest centred sub-rectangle of the
        // frame that matches the pane's shape. The overflow is never
        // sampled, so this crops without needing a scissor, and bars baked
        // into the stream go with it - reshaping the pane alone cannot do
        // that, since fit would only letterbox inside the new shape.
        float want = bw / bh;
        float sw = tw, sh = th;
        if (tw / th > want) sw = th * want;   // too wide: trim the sides
        else                sh = tw / want;   // too tall: trim top and bottom
        src = (Rectangle){ (tw - sw) * 0.5f, (th - sh) * 0.5f, sw, -sh };
        dst = (Rectangle){ box.x, box.y, bw, bh };
    } else {
        // Fit: letterbox inside the pane, preserving aspect.
        float scale = fminf(bw / tw, bh / th);
        float dw = tw * scale, dh = th * scale;
        src = (Rectangle){ 0, 0, tw, -th };
        dst = (Rectangle){ box.x + (bw - dw) * 0.5f,
                           box.y + (bh - dh) * 0.5f, dw, dh };
    }

    // mpv does not write meaningful alpha into the target FBO - it is
    // rendering for a window, where alpha is ignored. Blending the result
    // would multiply the video away. Draw it opaque.
    rlDrawRenderBatchActive();
    rlDisableColorBlend();
    DrawTexturePro(*tex, src, dst, (Vector2){ 0, 0 }, 0.0f, WHITE);
    rlDrawRenderBatchActive();
    rlEnableColorBlend();
}

// ---------------------------------------------------------- small widgets

// Requested sizes snap to the nearest rasterised font so nothing is scaled.
static uint16_t font_id_for(int size) {
    return size >= 15 ? VS_FONT_BODY : (size >= 12 ? VS_FONT_SMALL : VS_FONT_MICRO);
}
static uint16_t font_px_for(int size) {
    return (uint16_t)g_px[font_id_for(size)];
}

// Returns true while hovered, so callers can colour themselves.
static bool button(Clay_ElementId id, Clay_String label, bool on, bool wide) {
    bool hot = over(id);
    CLAY(id, {
        .layout = { .sizing = { wide ? CLAY_SIZING_GROW(0) : CLAY_SIZING_FIT(0),
                                CLAY_SIZING_FIXED((float)sc(26)) },
                    .padding = { (uint16_t)sc(10), (uint16_t)sc(10), 0, 0 },
                    .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER } },
        .backgroundColor = on ? VS_ACCENT : (hot ? VS_PANEL_HI : VS_PANEL),
        .cornerRadius = CLAY_CORNER_RADIUS(VS_RADIUS),
    }) {
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({
            .textColor = on ? (Clay_Color){ 12, 14, 20, 255 } : VS_TEXT,
            .fontId = VS_FONT_SMALL, .fontSize = font_px_for(VS_SIZE_SMALL),
            .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
    return hot;
}

static void label_text(Clay_String s, Clay_Color col, int size) {
    CLAY_TEXT(s, CLAY_TEXT_CONFIG({ .textColor = col,
                                    .fontId = font_id_for(size),
                                    .fontSize = font_px_for(size),
                                    .wrapMode = CLAY_TEXT_WRAP_NONE }));
}

static void spacer(void) {
    CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) } } }) {}
}

// ------------------------------------------------------------------ panes

// ------------------------------------------------------- channel browsing

#define VS_EDIT_CHAT    (VS_MAX_SOURCES)
#define VS_EDIT_CHANNEL (VS_MAX_SOURCES + 1)
#define VS_EDIT_CHATFIND (VS_MAX_SOURCES + 2)
// Chat from a Twitch VOD that is not on screen - what the old "LOAD AS /
// Chat log" toggle in the VOD picker was for. A chat-only source resolves
// for its PROGRAM-DATE-TIME and never opens a player (see vs_player.c), so
// it costs a slot but no decoding.
#define VS_EDIT_CHATVOD  (VS_MAX_SOURCES + 3)


// One-line text field. `slot` is the a->editing value that owns it.
static void text_field(vs_app *a, Clay_ElementId id, int slot,
                       Clay_String placeholder) {
    bool editing = (a->editing == slot);
    CLAY(id, {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED((float)sc(30)) },
                    .padding = { (uint16_t)sc(10), (uint16_t)sc(10), 0, 0 },
                    .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER },
                    .childGap = 2 },
        .backgroundColor = editing ? VS_PANEL_HI : VS_PANEL,
        .cornerRadius = CLAY_CORNER_RADIUS(VS_RADIUS),
        .border = { .color = editing ? VS_ACCENT : VS_LINE,
                    .width = CLAY_BORDER_OUTSIDE(1) },
    }) {
        if (editing) {
            if (a->edit_len) label_text(sfmt("%s", a->edit_buf), VS_TEXT, 14);
            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(1),
                                                   CLAY_SIZING_FIXED((float)sc(16)) } },
                           .backgroundColor = VS_ACCENT }) {}
        } else {
            label_text(placeholder, VS_TEXT_DIM, 14);
        }
    }
}

static void url_field(vs_app *a, int slot) {
    text_field(a, CLAY_IDI("UrlField", slot), slot,
               CLAY_STRING("paste a twitch.tv or kick.com VOD url"));
}

static void empty_pane(vs_app *a, int slot) {
    CLAY(CLAY_IDI("Pane", slot), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                    .padding = CLAY_PADDING_ALL(16), .childGap = 8,
                    .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER },
                    .layoutDirection = CLAY_TOP_TO_BOTTOM },
        .backgroundColor = VS_STAGE,
        .cornerRadius = CLAY_CORNER_RADIUS(VS_RADIUS),
        .border = { .color = VS_LINE, .width = CLAY_BORDER_OUTSIDE(1) },
    }) {
        label_text(sfmt("slot %d empty", slot + 1), VS_TEXT_DIM, 15);

        CLAY(CLAY_IDI("ChanBox", slot), {
            .layout = { .sizing = { CLAY_SIZING_PERCENT(0.9f), CLAY_SIZING_FIT(0) },
                        .childGap = 6, .layoutDirection = CLAY_TOP_TO_BOTTOM },
        }) {
            label_text(CLAY_STRING("CHANNEL"), VS_TEXT_DIM, 11);
            text_field(a, CLAY_IDI("ChanField", slot), VS_EDIT_CHANNEL,
                       CLAY_STRING("type a channel name"));
            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                                   CLAY_SIZING_FIT(0) },
                                       .childGap = 6 } }) {
                button(CLAY_IDI("FindBtn", slot), CLAY_STRING("Find VODs"),
                       false, true);
                button(CLAY_IDI("UrlToggle", slot), CLAY_STRING("URL"),
                       a->editing == slot, false);
            }
            if (a->editing == slot) url_field(a, slot);

            if (g_channels.n > 0) {
                label_text(CLAY_STRING("SAVED"), VS_TEXT_DIM, 11);
                int show = g_channels.n > 6 ? 6 : g_channels.n;
                for (int i = 0; i < show; i++) {
                    CLAY(CLAY_IDI("ChipRow", slot * VS_MAX_SAVED + i), {
                        .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                                CLAY_SIZING_FIT(0) },
                                    .childGap = 4 },
                    }) {
                        button(CLAY_IDI("Chip", slot * VS_MAX_SAVED + i),
                               sfmt("%s", g_channels.names[i]), false, true);
                        button(CLAY_IDI("ChipDel", slot * VS_MAX_SAVED + i),
                               CLAY_STRING("x"), false, false);
                    }
                }
            }
        }
    }
}

// True when this VOD has a counterpart on the other platform at the same
// wall-clock time - that is, the two are the same simulcast session.
static bool has_pair(const vs_vod *v, const vs_vod *other, int nother) {
    if (v->ts <= 0) return false;
    for (int i = 0; i < nother; i++) {
        if (other[i].ts <= 0) continue;
        double dt = other[i].ts - v->ts;
        if (dt < 0) dt = -dt;
        if (dt < 120.0) return true;
    }
    return false;
}

static void live_row(Clay_ElementId id, bool twitch, const char *channel) {
    bool hot = over(id);
    CLAY(id, {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                    .padding = { 8, 8, 6, 6 }, .childGap = 8,
                    .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER } },
        .backgroundColor = hot ? VS_PANEL_HI : VS_PANEL,
        .cornerRadius = CLAY_CORNER_RADIUS(VS_RADIUS),
        .border = { .color = VS_ERR, .width = CLAY_BORDER_OUTSIDE(1) },
    }) {
        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(8),
                                               CLAY_SIZING_FIXED(8) } },
                       .backgroundColor = VS_ERR,
                       .cornerRadius = CLAY_CORNER_RADIUS(4) }) {}
        label_text(CLAY_STRING("LIVE NOW"), VS_ERR, 13);
        label_text(sfmt("%s on %s", channel, twitch ? "Twitch" : "Kick"),
                   VS_TEXT_DIM, 12);
    }
}

static void vod_row(const vs_vod *v, Clay_ElementId id, bool paired) {
    bool hot = over(id);
    CLAY(id, {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                    .padding = { 8, 8, 6, 6 }, .childGap = 4,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM },
        .backgroundColor = hot ? VS_PANEL_HI : VS_PANEL,
        .cornerRadius = CLAY_CORNER_RADIUS(VS_RADIUS),
        .border = { .color = paired ? VS_OK : VS_LINE,
                    .width = CLAY_BORDER_OUTSIDE(1) },
    }) {
        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                               CLAY_SIZING_FIT(0) },
                                   .childGap = 8,
                                   .childAlignment = { CLAY_ALIGN_X_LEFT,
                                                       CLAY_ALIGN_Y_CENTER } } }) {
            if (v->ts > 0) {
                char d[24], c[24];
                vs_fmt_date(v->ts, d, sizeof d);
                vs_fmt_clock(v->ts, c, sizeof c);
                label_text(sfmt("%s  %s", d, c), VS_TEXT, 13);
            } else {
                label_text(CLAY_STRING("date ..."), VS_TEXT_DIM, 13);
            }
            char dur[24];
            vs_fmt_dur(v->dur, dur, sizeof dur);
            label_text(sfmt("%s", dur), VS_TEXT_DIM, 11);
            spacer();
            if (paired) label_text(CLAY_STRING("PAIR"), VS_OK, 11);
        }
        CLAY_TEXT(sfmt("%s", v->title),
                  CLAY_TEXT_CONFIG({ .textColor = VS_TEXT_DIM,
                                     .fontId = VS_FONT_MICRO,
                                     .fontSize = font_px_for(VS_SIZE_MICRO),
                                     .wrapMode = CLAY_TEXT_WRAP_WORDS }));
    }
}

static void vod_column(bool twitch, const vs_vod *list, int n,
                       const vs_vod *other, int nother) {
    CLAY(twitch ? CLAY_ID("ColTw") : CLAY_ID("ColKk"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                    .childGap = 6, .layoutDirection = CLAY_TOP_TO_BOTTOM },
    }) {
        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                               CLAY_SIZING_FIT(0) },
                                   .childGap = 6,
                                   .childAlignment = { CLAY_ALIGN_X_LEFT,
                                                       CLAY_ALIGN_Y_CENTER } } }) {
            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(8),
                                                   CLAY_SIZING_FIXED(8) } },
                           .backgroundColor = twitch ? VS_TWITCH : VS_KICK,
                           .cornerRadius = CLAY_CORNER_RADIUS(4) }) {}
            label_text(sfmt("%s  (%d)", twitch ? "Twitch" : "Kick", n),
                       VS_TEXT, 14);
        }
        CLAY(twitch ? CLAY_ID("VodListTw") : CLAY_ID("VodListKk"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                        .padding = CLAY_PADDING_ALL(6), .childGap = 5,
                        .layoutDirection = CLAY_TOP_TO_BOTTOM },
            .backgroundColor = VS_BG,
            .cornerRadius = CLAY_CORNER_RADIUS(VS_RADIUS),
            .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() },
        }) {
            // Asked for outright rather than guessed from the newest VOD:
            // an in-progress Kick VOD reports a duration of zero, so there is
            // nothing in the listing that says the channel is live.
            // Not in chat mode: a live channel's chat comes from its
            // in-progress VOD, found by a separate lookup, so this button
            // would have nothing useful to do here.
            if (!g_browser.chat_target &&
                (twitch ? g_browser.tw_live : g_browser.kk_live))
                live_row(twitch ? CLAY_ID("LiveTw") : CLAY_ID("LiveKk"),
                         twitch, g_browser.query);
            if (n == 0) {
                label_text(CLAY_STRING("nothing here"), VS_TEXT_DIM, 12);
            } else {
                for (int i = 0; i < n; i++) {
                    Clay_ElementId id = twitch ? CLAY_IDI("VodTw", i)
                                               : CLAY_IDI("VodKk", i);
                    vod_row(&list[i], id, has_pair(&list[i], other, nother));
                }
            }
        }
    }
}

// Was any of the browser's result rows clicked this frame?
static bool browser_row_clicked(void) {
    for (int i = 0; i < g_browser.ntw; i++)
        if (clicked(CLAY_IDI("VodTw", i))) return true;
    for (int i = 0; i < g_browser.nkk; i++)
        if (clicked(CLAY_IDI("VodKk", i))) return true;
    return false;
}

static void browser_overlay(vs_app *a) {
    CLAY(CLAY_ID("Browser"), {
        .layout = { .sizing = { CLAY_SIZING_PERCENT(0.86f),
                                CLAY_SIZING_PERCENT(0.84f) },
                    .padding = CLAY_PADDING_ALL(14), .childGap = 10,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM },
        .backgroundColor = VS_PANEL,
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .floating = { .zIndex = 30,
                      .attachPoints = { CLAY_ATTACH_POINT_CENTER_CENTER,
                                        CLAY_ATTACH_POINT_CENTER_CENTER },
                      .attachTo = CLAY_ATTACH_TO_ROOT },
        .border = { .color = VS_ACCENT, .width = CLAY_BORDER_OUTSIDE(1) },
    }) {
        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                               CLAY_SIZING_FIT(0) },
                                   .childGap = 8,
                                   .childAlignment = { CLAY_ALIGN_X_LEFT,
                                                       CLAY_ALIGN_Y_CENTER } } }) {
            label_text(CLAY_STRING("Channel"), VS_TEXT, 16);
            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(overlay_w(300)),
                                                   CLAY_SIZING_FIT(0) } } }) {
                text_field(a, CLAY_ID("BrowserField"), VS_EDIT_CHANNEL,
                           CLAY_STRING("type a channel name"));
            }
            button(CLAY_ID("BrowserFind"), CLAY_STRING("Find"), false, false);
            button(CLAY_ID("BrowserSave"), CLAY_STRING("Save"), false, false);
            if (g_browser.chat_target) {
                label_text(CLAY_STRING("-> chat"), VS_WARN, 12);
                button(CLAY_ID("BrowserPasteUrl"),
                       CLAY_STRING("Paste a URL instead"), false, false);
            } else {
                label_text(sfmt("-> slot %d", g_browser.slot + 1),
                           VS_TEXT_DIM, 12);
            }
            spacer();
            label_text(sfmt("%s", g_browser.status),
                       g_browser.state == VS_LOAD_FAILED ? VS_ERR : VS_TEXT_DIM, 12);
            button(CLAY_ID("BrowserClose"), CLAY_STRING("Close"), false, false);
        }

        if (g_channels.n > 0) {
            CLAY(CLAY_ID("BrowserSaved"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                            .childGap = 5,
                            .childAlignment = { CLAY_ALIGN_X_LEFT,
                                                CLAY_ALIGN_Y_CENTER } },
            }) {
                label_text(CLAY_STRING("SAVED"), VS_TEXT_DIM, 11);
                for (int i = 0; i < g_channels.n; i++)
                    button(CLAY_IDI("BChip", i), sfmt("%s", g_channels.names[i]),
                           vs_stricmp(g_channels.names[i], g_browser.query) == 0,
                           false);
            }
        }

        CLAY(CLAY_ID("BrowserCols"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                        .childGap = 10 },
        }) {
            vod_column(true,  g_browser.tw, g_browser.ntw,
                       g_browser.kk, g_browser.nkk);
            vod_column(false, g_browser.kk, g_browser.nkk,
                       g_browser.tw, g_browser.ntw);
        }
        label_text(CLAY_STRING("Click a VOD to load it into the slot. PAIR marks "
                               "the same session found on both platforms."),
                   VS_TEXT_DIM, 11);
    }
}

static void pane_header(vs_app *a, int i) {
    vs_source *s = &a->src[i];
    bool is_master = (a->master == i);
    CLAY(CLAY_IDI("PaneHead", i), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED((float)sc(24)) },
                    .padding = { 8, 8, 0, 0 }, .childGap = 8,
                    .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER } },
        .backgroundColor = VS_PANEL,
    }) {
        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(8),
                                               CLAY_SIZING_FIXED(8) } },
                       .backgroundColor = plat_color(s->platform),
                       .cornerRadius = CLAY_CORNER_RADIUS(4) }) {}
        label_text(sfmt("%s", s->channel[0] ? s->channel : "loading"), VS_TEXT, 14);
        if (s->live)
            label_text(s->growing ? CLAY_STRING("LIVE") : CLAY_STRING("OFFLINE"),
                       s->growing ? VS_ERR : VS_TEXT_DIM, 11);
        if (is_master) label_text(CLAY_STRING("MASTER"), VS_ACCENT, 11);
        if (fabs(s->offset) > 0.0005)
            label_text(sfmt("%+.2fs", s->offset), VS_WARN, 12);
        if (s->state == VS_LOAD_READY && !vs_covers(s, a->wall))
            label_text(CLAY_STRING("no coverage here"), VS_ERR, 12);
        spacer();
        if (fabs(s->speed - 1.0) > 0.005)
            label_text(sfmt("x%.3f", s->speed), VS_TEXT_DIM, 11);
        label_text(sfmt("%s", vs_platform_name(s->platform)), VS_TEXT_DIM, 11);
    }
}

static void pane_footer(vs_app *a, int i) {
    vs_source *s = &a->src[i];
    CLAY(CLAY_IDI("PaneFoot", i), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED((float)sc(28)) },
                    .padding = { 8, 8, 0, 0 }, .childGap = 6,
                    .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER } },
        .backgroundColor = VS_PANEL,
    }) {
        button(CLAY_IDI("Solo", i), CLAY_STRING("S"), s->solo, false);
        // volume track
        CLAY(CLAY_IDI("Vol", i), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(84), CLAY_SIZING_FIXED(16) },
                        .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER } },
            .backgroundColor = VS_BG,
            .cornerRadius = CLAY_CORNER_RADIUS(3),
        }) {
            CLAY_AUTO_ID({ .layout = { .sizing = {
                               CLAY_SIZING_PERCENT(s->volume < 0.02f ? 0.02f :
                                                   (s->volume > 1 ? 1 : s->volume)),
                               CLAY_SIZING_FIXED(16) } },
                           .backgroundColor = over(CLAY_IDI("Vol", i)) ? VS_ACCENT_HI : VS_ACCENT,
                           .cornerRadius = CLAY_CORNER_RADIUS(3) }) {}
        }
        label_text(sfmt("%d%%", (int)(s->volume * 100 + 0.5f)), VS_TEXT_DIM, 11);
        spacer();
        button(CLAY_IDI("NudgeL", i), CLAY_STRING("-"), false, false);
        button(CLAY_IDI("NudgeR", i), CLAY_STRING("+"), false, false);
        if (a->master != i) button(CLAY_IDI("MkMaster", i), CLAY_STRING("Master"), false, false);
        // Crop to the pane instead of letterboxing inside it. Per source,
        // because the bars belong to the stream, not to the layout.
        button(CLAY_IDI("PaneFill", i), CLAY_STRING("Crop"), s->fill, false);
        button(CLAY_IDI("Close", i), CLAY_STRING("x"), false, false);
    }
}

static void video_pane(vs_app *a, int i) {
    vs_source *s = &a->src[i];
    bool focused = (a->focus == i);
    g_custom[i].kind  = VS_CUSTOM_VIDEO;
    g_custom[i].index = i;

    CLAY(CLAY_IDI("Pane", i), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                    .layoutDirection = CLAY_TOP_TO_BOTTOM },
        .backgroundColor = VS_STAGE,
        .cornerRadius = CLAY_CORNER_RADIUS(VS_RADIUS),
        .border = { .color = focused ? VS_ACCENT : VS_LINE,
                    .width = CLAY_BORDER_OUTSIDE(focused ? 2 : 1) },
    }) {
        // Outside theater mode the header and footer are part of the pane's
        // column and the picture gets what is left. Inside it they float on
        // top, so revealing them does not resize the video underneath - the
        // frame would otherwise shift under the pointer the moment it
        // arrived, which is what made the chrome hard to hit.
        if (!a->theater && pane_chrome(a, i, true)) pane_header(a, i);
        // No .backgroundColor here: Clay emits a RECTANGLE *after* the CUSTOM
        // command for the same element, which would paint over the video every
        // frame. The pane parent already supplies the stage colour.
        CLAY(CLAY_IDI("Video", i), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                        .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER } },
            .custom = { .customData = &g_custom[i] },
        }) {
            if (!vs_player_has_frame(s)) {
                const char *msg =
                    s->state == VS_LOAD_FAILED ? s->status :
                    s->state == VS_LOAD_READY   ? "buffering..." : s->status;
                label_text(sfmt("%s", msg && msg[0] ? msg : "loading..."),
                           s->state == VS_LOAD_FAILED ? VS_ERR : VS_TEXT_DIM, 14);
            }
        }
        if (!a->theater && pane_chrome(a, i, false)) pane_footer(a, i);

        // Theater mode: the same two rows, floated over the picture. Width
        // comes from last frame's box, since a floating element has no
        // parent to grow into; on the very first frame there is none yet and
        // the chrome simply appears a frame later.
        if (a->theater) {
            bool fb;
            Clay_BoundingBox pb = box_of(CLAY_IDI("Pane", i), &fb);
            float w = fb ? pb.width : 0;
            if (w > 32 && pane_chrome(a, i, true)) {
                CLAY(CLAY_IDI("PaneHeadFloat", i), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(w),
                                            CLAY_SIZING_FIT(0) },
                                .layoutDirection = CLAY_TOP_TO_BOTTOM },
                    .floating = { .zIndex = 8,
                                  .attachPoints = { CLAY_ATTACH_POINT_LEFT_TOP,
                                                    CLAY_ATTACH_POINT_LEFT_TOP },
                                  .attachTo = CLAY_ATTACH_TO_PARENT },
                }) {
                    pane_header(a, i);
                }
            }
            if (w > 32 && pane_chrome(a, i, false)) {
                CLAY(CLAY_IDI("PaneFootFloat", i), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(w),
                                            CLAY_SIZING_FIT(0) },
                                .layoutDirection = CLAY_TOP_TO_BOTTOM },
                    .floating = { .zIndex = 8,
                                  .attachPoints = { CLAY_ATTACH_POINT_LEFT_BOTTOM,
                                                    CLAY_ATTACH_POINT_LEFT_BOTTOM },
                                  .attachTo = CLAY_ATTACH_TO_PARENT },
                }) {
                    pane_footer(a, i);
                }
            }
        }
    }
}

// A chat-source pane has no picture: it exists to pin the chat log to a
// particular Twitch VOD, which is what makes chat replay work for a VOD whose
// video is muted or that you are not watching at all.
static void chat_source_pane(vs_app *a, int i) {
    vs_source *s = &a->src[i];
    CLAY(CLAY_IDI("Pane", i), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                    .layoutDirection = CLAY_TOP_TO_BOTTOM },
        .backgroundColor = VS_STAGE,
        .cornerRadius = CLAY_CORNER_RADIUS(VS_RADIUS),
        .border = { .color = VS_WARN, .width = CLAY_BORDER_OUTSIDE(1) },
    }) {
        CLAY(CLAY_IDI("PaneHead", i), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                    CLAY_SIZING_FIXED((float)sc(24)) },
                        .padding = { 8, 8, 0, 0 }, .childGap = 8,
                        .childAlignment = { CLAY_ALIGN_X_LEFT,
                                            CLAY_ALIGN_Y_CENTER } },
            .backgroundColor = VS_PANEL,
        }) {
            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(8),
                                                   CLAY_SIZING_FIXED(8) } },
                           .backgroundColor = plat_color(s->platform),
                           .cornerRadius = CLAY_CORNER_RADIUS(4) }) {}
            label_text(sfmt("%s", s->channel[0] ? s->channel : "loading"),
                       VS_TEXT, 14);
            label_text(CLAY_STRING("CHAT SOURCE"), VS_WARN, 11);
            spacer();
            button(CLAY_IDI("Close", i), CLAY_STRING("x"), false, false);
        }
        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                               CLAY_SIZING_GROW(0) },
                                   .padding = CLAY_PADDING_ALL(14),
                                   .childGap = 8,
                                   .childAlignment = { CLAY_ALIGN_X_CENTER,
                                                       CLAY_ALIGN_Y_CENTER },
                                   .layoutDirection = CLAY_TOP_TO_BOTTOM } }) {
            if (s->state == VS_LOAD_FAILED) {
                label_text(sfmt("%s", s->status), VS_ERR, 13);
            } else if (s->state != VS_LOAD_READY) {
                label_text(sfmt("%s", s->status), VS_TEXT_DIM, 13);
            } else {
                char d[24], c2[24], dur[24];
                vs_fmt_date(s->pdt_start, d, sizeof d);
                vs_fmt_clock(s->pdt_start, c2, sizeof c2);
                vs_fmt_dur(s->duration, dur, sizeof dur);
                label_text(sfmt("%s  %s", d, c2), VS_TEXT, 14);
                label_text(sfmt("%s  %s", dur, s->vid), VS_TEXT_DIM, 12);
                CLAY_TEXT(sfmt("%s", s->title),
                          CLAY_TEXT_CONFIG({ .textColor = VS_TEXT_DIM,
                                             .fontId = VS_FONT_MICRO,
                                             .fontSize = font_px_for(11),
                                             .wrapMode = CLAY_TEXT_WRAP_WORDS }));
                CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIT(0),
                                                       CLAY_SIZING_FIT(0) },
                                           .childGap = 6 } }) {
                    button(CLAY_IDI("PaneChatSync", i),
                           vs_chat_sync_busy(&a->chat) ? CLAY_STRING("...")
                                                       : CLAY_STRING("Update now"),
                           false, false);
                    button(CLAY_IDI("PaneChatAuto", i), CLAY_STRING("Auto"),
                           a->chat.auto_sync, false);
                }
                if (a->chat.sync_status[0])
                    label_text(sfmt("%s", a->chat.sync_status),
                               a->chat.sync_state == VS_LOAD_FAILED ? VS_ERR
                                                                    : VS_OK, 11);
            }
        }
    }
}

static void pane_or_empty(vs_app *a, int slot) {
    if (a->src[slot].active && a->src[slot].role == VS_ROLE_VIDEO)
        video_pane(a, slot);
    else if (a->src[slot].active && a->src[slot].role == VS_ROLE_CHAT)
        chat_source_pane(a, slot);
    else
        empty_pane(a, slot);
}

// ------------------------------------------------------------------ stage

void vs_ui_swap_split(vs_app *a) {
    a->split_swap = !a->split_swap;
    // Named for what moved on screen, which depends on the orientation: one
    // bool, read as top/bottom when stacked and left/right in columns.
    vs_toast(a, a->split_vertical ? "swapped top and bottom"
                                  : "swapped left and right");
}

static void stage_split(vs_app *a) {
    float r = a->split_ratio;
    if (r < 0.15f) r = 0.15f;
    if (r > 0.85f) r = 0.85f;
    CLAY(CLAY_ID("Stage"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                    .childGap = 0,
                    .layoutDirection = a->split_vertical ? CLAY_TOP_TO_BOTTOM
                                                         : CLAY_LEFT_TO_RIGHT },
    }) {
        CLAY(CLAY_ID("SplitA"), {
            .layout = { .sizing = a->split_vertical
                ? (Clay_Sizing){ CLAY_SIZING_GROW(0), CLAY_SIZING_PERCENT(r) }
                : (Clay_Sizing){ CLAY_SIZING_PERCENT(r), CLAY_SIZING_GROW(0) } },
        }) { pane_or_empty(a, a->split_swap ? 1 : 0); }

        CLAY(CLAY_ID("Divider"), {
            .layout = { .sizing = a->split_vertical
                ? (Clay_Sizing){ CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(8) }
                : (Clay_Sizing){ CLAY_SIZING_FIXED(8), CLAY_SIZING_GROW(0) } },
            .backgroundColor = (over(CLAY_ID("Divider")) || a->drag == 0)
                               ? VS_ACCENT : VS_BG,
        }) {}

        CLAY(CLAY_ID("SplitB"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
        }) { pane_or_empty(a, a->split_swap ? 0 : 1); }
    }
}

static void stage_grid(vs_app *a) {
    CLAY(CLAY_ID("Stage"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                    .childGap = VS_GAP, .layoutDirection = CLAY_TOP_TO_BOTTOM },
    }) {
        for (int row = 0; row < 2; row++) {
            CLAY(CLAY_IDI("GridRow", row), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                            .childGap = VS_GAP },
            }) {
                for (int col = 0; col < 2; col++) pane_or_empty(a, row * 2 + col);
            }
        }
    }
}

// Which pane fills the stage behind the strip. The resize drag has to
// agree with the layout about this, or it counts the wrong number of slots.
static int pip_big(const vs_app *a) {
    int big = a->focus;
    if (big < 0 || big >= VS_MAX_SOURCES ||
        !a->src[big].active || a->src[big].role != VS_ROLE_VIDEO) {
        int f = vs_nth_active_video(a, 0);
        big = f < 0 ? 0 : f;
    }
    return big;
}

// How many panes the strip is stacking: every active video but the big one.
static int pip_slot_count(const vs_app *a) {
    int big = pip_big(a), n = 0;
    for (int i = 0; i < VS_MAX_SOURCES; i++) {
        if (i == big) continue;
        if (a->src[i].active && a->src[i].role == VS_ROLE_VIDEO) n++;
    }
    return n;
}

// The PiP pane's size in real pixels. This used to be the raw constant
// times one scale factor, which made the strip the only part of the UI that
// ignored sc() - on a 4K screen at 200% every other element doubled, down
// to the strip's own grab bar at sc(15), and the panes did not. The 2.5x
// cap was then standing in for "do not let the PiP eat the stage", which is
// a bound better expressed against the stage itself (see the resize drag).
static float pip_pane_w(const vs_app *a) {
    return (float)sc((int)VS_PIP_W) * (a->pip_sx > 0 ? a->pip_sx : 1.0f);
}
static float pip_pane_h(const vs_app *a) {
    return (float)sc((int)VS_PIP_H) * (a->pip_sy > 0 ? a->pip_sy : 1.0f);
}

// The resize drag, in two halves, taking the pointer explicitly so the
// selftest can drive exactly the code the mouse drives - the arithmetic here
// is the whole of the fix, and a test against a copy of it would prove
// nothing about what ships.
//
// What was wrong before: the scale came from
//     s2 = (m.x - pb.x) / VS_PIP_W
// measuring from the strip's LIVE left edge while the grip being dragged sits
// at the bottom-RIGHT. pip_x is a fraction of the space the strip does NOT
// occupy, so growing the strip shrank that space and moved the left edge -
// the origin depended on the size being measured from it. pb was also last
// frame's box, so each correction landed a frame late and the error
// oscillated instead of settling. Hence the jumping.
//
// Now the size is measured against the top-left latched at mousedown, and
// pip_x/pip_y are then re-solved to keep that corner where it was. The box
// grows from the corner under the pointer and the origin no longer moves
// with it.
static void pip_resize_latch(vs_app *a, Vector2 m) {
    bool f1, f2;
    Clay_BoundingBox st = box_of(CLAY_ID("Stage"), &f1);
    Clay_BoundingBox pb = box_of(CLAY_ID("PipStrip"), &f2);
    int n = pip_slot_count(a);
    a->drag_aux = n < 1 ? 1 : n;
    a->pip_ax = a->pip_ay = 0;
    a->drag_dx = a->drag_dy = 0;
    a->pip_oh = 0;
    if (f1 && f2) {
        a->pip_ax = pb.x - st.x;      // the corner to hold still
        a->pip_ay = pb.y - st.y;
        // Grab offset from the corner being dragged, so the box does not
        // snap to the pointer the instant the grip is taken.
        a->drag_dx = m.x - (pb.x + pb.width);
        a->drag_dy = m.y - (pb.y + pb.height);
        // Whatever the strip is besides its slots - grab bar, padding,
        // gaps - so a vertical drag can be turned back into a per-slot
        // height whatever those happen to add.
        a->pip_oh = pb.height - (float)a->drag_aux * pip_pane_h(a);
        if (a->pip_oh < 0) a->pip_oh = 0;
    }
}

static void pip_resize_apply(vs_app *a, Vector2 m) {
    bool f1;
    Clay_BoundingBox st = box_of(CLAY_ID("Stage"), &f1);
    if (!f1 || st.width <= 1 || st.height <= 1) return;

    float basew = (float)sc((int)VS_PIP_W);
    float baseh = (float)sc((int)VS_PIP_H);
    if (basew < 1) basew = 1;
    if (baseh < 1) baseh = 1;
    int   n  = a->drag_aux > 0 ? a->drag_aux : 1;
    float oh = a->pip_oh > 0 ? a->pip_oh : 0;

    float w = (m.x - a->drag_dx) - (st.x + a->pip_ax);
    float h = (m.y - a->drag_dy) - (st.y + a->pip_ay);

    // The real ceiling: the stage. That means the same thing on a 1280x800
    // Deck and on a 4K panel, which a fixed multiplier of an unscaled
    // constant never did.
    float maxw = st.width  * 0.9f;
    float maxh = st.height * 0.9f;
    if (w > maxw) w = maxw;
    if (h > maxh) h = maxh;

    float sx = w / basew;
    float sy = ((h - oh) / (float)n) / baseh;
    if (sx < 0.30f) sx = 0.30f;
    if (sx > 8.00f) sx = 8.00f;
    if (sy < 0.30f) sy = 0.30f;
    if (sy > 8.00f) sy = 8.00f;
    a->pip_sx = sx;
    a->pip_sy = sy;

    // Predict next frame's strip box: its width is FIXED(pw) exactly, and
    // its height is the slots plus the chrome latched at drag start.
    float neww = basew * sx;
    float newh = oh + (float)n * baseh * sy;
    float availw = st.width  - neww;
    float availh = st.height - newh;
    if (availw > 1) {
        float t = a->pip_ax / availw;
        a->pip_x = t < 0 ? 0 : (t > 1 ? 1 : t);
    }
    if (availh > 1) {
        float t = a->pip_ay / availh;
        a->pip_y = t < 0 ? 0 : (t > 1 ? 1 : t);
    }
}

// Dev hooks: one synthetic resize, so "the corner stays put" can be measured
// rather than asserted. See --pip-drag.
void vs_ui_pip_drag_begin(vs_app *a, float mx, float my) {
    pip_resize_latch(a, (Vector2){ mx, my });
    a->drag = 41;
}
void vs_ui_pip_drag_to(vs_app *a, float mx, float my) {
    pip_resize_apply(a, (Vector2){ mx, my });
}

static void stage_pip(vs_app *a) {
    int big = pip_big(a);
    float pw = pip_pane_w(a);
    float ph = pip_pane_h(a);

    // Position comes from last frame's stage box, which is all Clay can give
    // us here; one frame of lag is invisible while dragging.
    bool haveStage = false;
    Clay_BoundingBox st = box_of(CLAY_ID("Stage"), &haveStage);
    bool havePip = false;
    Clay_BoundingBox pb = box_of(CLAY_ID("PipStrip"), &havePip);
    float stripW = havePip && pb.width  > 1 ? pb.width  : pw;
    float stripH = havePip && pb.height > 1 ? pb.height : ph;
    float availw = (haveStage && st.width  > stripW) ? st.width  - stripW : 0;
    float availh = (haveStage && st.height > stripH) ? st.height - stripH : 0;
    float ox = a->pip_x * availw;
    float oy = a->pip_y * availh;

    CLAY(CLAY_ID("Stage"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
    }) {
        pane_or_empty(a, big);

        CLAY(CLAY_ID("PipStrip"), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(pw), CLAY_SIZING_FIT(0) },
                        .padding = CLAY_PADDING_ALL((uint16_t)sc(4)),
                        .childGap = (uint16_t)sc(4),
                        .layoutDirection = CLAY_TOP_TO_BOTTOM },
            .backgroundColor = VS_PANEL,
            .cornerRadius = CLAY_CORNER_RADIUS(VS_RADIUS),
            .floating = { .offset = { ox, oy }, .zIndex = 4,
                          .attachPoints = { CLAY_ATTACH_POINT_LEFT_TOP,
                                            CLAY_ATTACH_POINT_LEFT_TOP },
                          .attachTo = CLAY_ATTACH_TO_PARENT },
            .border = { .color = (a->drag == 40 || a->drag == 41) ? VS_ACCENT : VS_LINE,
                        .width = CLAY_BORDER_OUTSIDE(1) },
        }) {
            // Grab bar: dragging the pane body itself would fight the pane's
            // own controls, so moving gets its own strip.
            bool pipc = pip_chrome(a);
            if (pipc)
            CLAY(CLAY_ID("PipDrag"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                        CLAY_SIZING_FIXED((float)sc(15)) },
                            .padding = { (uint16_t)sc(6), (uint16_t)sc(6), 0, 0 },
                            .childGap = (uint16_t)sc(6),
                            .childAlignment = { CLAY_ALIGN_X_LEFT,
                                                CLAY_ALIGN_Y_CENTER } },
                .backgroundColor = (over(CLAY_ID("PipDrag")) || a->drag == 40)
                                   ? VS_ACCENT : VS_PANEL_HI,
                .cornerRadius = CLAY_CORNER_RADIUS(3),
            }) {
                label_text(CLAY_STRING("= = ="), VS_TEXT_DIM, 11);
                spacer();
                label_text(sfmt("%dx%d%%", (int)(a->pip_sx * 100 + 0.5f),
                                (int)(a->pip_sy * 100 + 0.5f)),
                           VS_TEXT_DIM, 11);
            }

            for (int i = 0; i < VS_MAX_SOURCES; i++) {
                if (i == big) continue;
                if (!a->src[i].active || a->src[i].role != VS_ROLE_VIDEO) continue;
                CLAY(CLAY_IDI("PipSlot", i), {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                            CLAY_SIZING_FIXED(ph) } },
                }) { video_pane(a, i); }
            }

            if (pipc)
            CLAY(CLAY_ID("PipGrip"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED((float)sc(16)),
                                        CLAY_SIZING_FIXED((float)sc(16)) } },
                .backgroundColor = (over(CLAY_ID("PipGrip")) || a->drag == 41)
                                   ? VS_ACCENT : VS_LINE,
                .cornerRadius = CLAY_CORNER_RADIUS(3),
                .floating = { .zIndex = 6,
                              .attachPoints = { CLAY_ATTACH_POINT_RIGHT_BOTTOM,
                                                CLAY_ATTACH_POINT_RIGHT_BOTTOM },
                              .attachTo = CLAY_ATTACH_TO_PARENT },
            }) {}
        }
    }
}

// --------------------------------------------------------------- toolbar

// One pane plus the chat column. The chat panel is forced visible in this
// layout even with no log loaded, since that is the point of choosing it.
static void stage_solo(vs_app *a) {
    int big = a->focus;
    if (big < 0 || big >= VS_MAX_SOURCES || !a->src[big].active ||
        a->src[big].role != VS_ROLE_VIDEO) {
        int f = vs_nth_active_video(a, 0);
        big = f < 0 ? 0 : f;
    }
    CLAY(CLAY_ID("Stage"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
    }) {
        pane_or_empty(a, big);
    }
}

// Two across the top, one along the bottom.
static void stage_tri(vs_app *a) {
    CLAY(CLAY_ID("Stage"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                    .childGap = VS_GAP, .layoutDirection = CLAY_TOP_TO_BOTTOM },
    }) {
        CLAY(CLAY_ID("TriTop"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                        .childGap = VS_GAP },
        }) {
            pane_or_empty(a, 0);
            pane_or_empty(a, 1);
        }
        CLAY(CLAY_ID("TriBot"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
        }) {
            pane_or_empty(a, 2);
        }
    }
}

static void toolbar(vs_app *a) {
    // Everything in here is scaled, so on a 4K screen at 200% the row needs
    // twice the width to hold the same controls. When the window cannot give
    // it that, the words go and the buttons stay: a label reading "Layout"
    // next to a button that already says "Split" is the first thing that can
    // be spared, and losing the help button off the right edge is not.
    bool narrow = GetScreenWidth() < sc(1480);

    CLAY(CLAY_ID("Toolbar"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED((float)sc(VS_BAR_H)) },
                    .padding = { VS_PAD, VS_PAD, 0, 0 }, .childGap = VS_GAP,
                    .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER } },
        .backgroundColor = VS_PANEL,
        // Dropping the labels above is not enough on its own: the buttons
        // that remain still need about 626 unscaled pixels, which at 250%
        // display scale and a text scale of 2.0 is 3130 - more than the
        // window. None of it wraps or shrinks, so this row set a minimum
        // width for the whole window, and the layout grew past the right
        // edge rather than fitting. What lives on that edge is the chat
        // column, so the chat silently disappeared while the toolbar itself
        // looked fine - and resizing appeared to fix it because it changed
        // which part of the oversized layout you could see.
        //
        // Measured with --theater, which takes the toolbar out of the
        // column: the body went from 3130 back to exactly the window width
        // and the chat returned. Clipping keeps the row from dictating the
        // layout; it scrolls horizontally instead, so the controls pushed
        // past the edge are still reachable rather than unreachable.
        .clip = { .horizontal = true, .childOffset = Clay_GetScrollOffset() },
    }) {
        CLAY(CLAY_ID("Logo"), {
            .layout = { .sizing = { CLAY_SIZING_FIXED((float)sc(26)),
                                    CLAY_SIZING_FIXED((float)sc(26)) } },
            .custom = { .customData = &g_logo_custom },
        }) {}
        label_text(CLAY_STRING("vodsync"), VS_TEXT, 17);

        button(CLAY_ID("PlayBtn"),
               a->playing ? CLAY_STRING("Pause") : CLAY_STRING("Play"),
               a->playing, false);

        // With a live source loaded, say whether the playhead is at the edge
        // and offer the way back to it. Scrubbing into the past is a normal
        // thing to do here - everything watched so far stays seekable - so
        // this doubles as the readout of how far back you are.
        if (vs_any_live(a)) {
            double behind = vs_live_ceiling(a) - a->wall;
            if (behind < 0) behind = 0;
            if (a->live_mode) {
                button(CLAY_ID("LiveBtn"), CLAY_STRING("LIVE"), true, false);
            } else {
                char b[32];
                vs_fmt_dur(behind, b, sizeof b);
                button(CLAY_ID("LiveBtn"), sfmt("GO LIVE  -%s", b), false, false);
            }
        }

        // Where the playhead sits, in both local time and UTC. VOD
        // timelines come from PROGRAM-DATE-TIME, which is UTC, so showing
        // both makes it obvious what the number is being compared against.
        char clk[32], dt[24], utc[32];
        vs_fmt_clock(a->wall, clk, sizeof clk);
        vs_fmt_date(a->wall, dt, sizeof dt);
        vs_fmt_clock_utc(a->wall, utc, sizeof utc);
        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIT(0),
                                               CLAY_SIZING_FIXED((float)sc(26)) },
                                   .padding = { (uint16_t)sc(10), (uint16_t)sc(10), 0, 0 },
                                   .childGap = 6,
                                   .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER } },
                       .backgroundColor = VS_BG,
                       .cornerRadius = CLAY_CORNER_RADIUS(VS_RADIUS) }) {
            label_text(sfmt("%s", clk), VS_TEXT, 16);
            if (!narrow) label_text(sfmt("%s", dt), VS_TEXT_DIM, 12);
            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(1),
                                                   CLAY_SIZING_FIXED((float)sc(14)) } },
                           .backgroundColor = VS_LINE }) {}
            label_text(CLAY_STRING("UTC"), VS_TEXT_DIM, 11);
            label_text(sfmt("%s", utc), VS_TEXT, 13);
        }

        if (!narrow) label_text(CLAY_STRING("Layout"), VS_TEXT_DIM, 12);
        button(CLAY_ID("LayoutBtn"), sfmt("%s  v", layout_name(a->layout)),
               a->layout_menu, false);
        if (a->layout == VS_LAYOUT_SPLIT) {
            button(CLAY_ID("OrientBtn"),
                   a->split_vertical ? CLAY_STRING("Stacked") : CLAY_STRING("Columns"),
                   false, false);
            button(CLAY_ID("SwapBtn"), CLAY_STRING("Swap"),
                   a->split_swap, false);
        }

        spacer();

        button(CLAY_ID("BmAdd"),  CLAY_STRING("Mark"), false, false);
        button(CLAY_ID("BmPrev"), CLAY_STRING("<"),    false, false);
        button(CLAY_ID("BmNext"), CLAY_STRING(">"),    false, false);
        if (a->nbm) label_text(sfmt("%d", a->nbm), VS_TEXT_DIM, 12);
        button(CLAY_ID("SaveBtn"), CLAY_STRING("Save"), false, false);
        button(CLAY_ID("LoadBtn"), CLAY_STRING("Load"), false, false);
        {
            bool armed = g_reset_armed_until > 0 && a->now < g_reset_armed_until;
            button(CLAY_ID("ResetBtn"),
                   armed ? CLAY_STRING("Sure?") : CLAY_STRING("Reset"),
                   armed, false);
        }
        if (!narrow) label_text(CLAY_STRING("Text"), VS_TEXT_DIM, 11);
        button(CLAY_ID("TextDec"), CLAY_STRING("A-"), false, false);
        label_text(sfmt("%d%%", (int)(a->text_scale * 100 + 0.5f)),
                   VS_TEXT_DIM, 11);
        button(CLAY_ID("TextInc"), CLAY_STRING("A+"), false, false);
        button(CLAY_ID("TheaterBtn"),
               narrow ? (a->theater ? CLAY_STRING("[x] Th") : CLAY_STRING("[ ] Th"))
                      : (a->theater ? CLAY_STRING("[x] Theater")
                                    : CLAY_STRING("[ ] Theater")),
               a->theater, false);
        button(CLAY_ID("FullBtn"),
               narrow ? CLAY_STRING("[ ]")
                      : (a->fullscreen ? CLAY_STRING("Windowed")
                                       : CLAY_STRING("Fullscreen")),
               a->fullscreen, false);
        button(CLAY_ID("HelpBtn"), CLAY_STRING("?"), a->show_help, false);

        if (a->layout_menu) {
            CLAY(CLAY_ID("LayoutMenu"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(overlay_w(170)),
                                        CLAY_SIZING_FIT(0) },
                            .padding = CLAY_PADDING_ALL(6), .childGap = 4,
                            .layoutDirection = CLAY_TOP_TO_BOTTOM },
                .backgroundColor = VS_PANEL_HI,
                .cornerRadius = CLAY_CORNER_RADIUS(VS_RADIUS),
                .floating = { .offset = { 0, 4 }, .parentId = CLAY_ID("LayoutBtn").id,
                              .zIndex = 10,
                              .attachPoints = { CLAY_ATTACH_POINT_LEFT_TOP,
                                                CLAY_ATTACH_POINT_LEFT_BOTTOM },
                              .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID },
                .border = { .color = VS_LINE, .width = CLAY_BORDER_OUTSIDE(1) },
            }) {
                for (int l = 0; l < VS_LAYOUT_COUNT; l++)
                    button(CLAY_IDI("LayoutOpt", l), sfmt("%s", layout_name((vs_layout)l)),
                           a->layout == (vs_layout)l, true);
            }
        }
    }
}

// -------------------------------------------------------------- timelines

// Unified wall-clock bar: one axis of real time-of-day with per-source coverage.
static void unified_bar(vs_app *a) {
    CLAY(CLAY_ID("UniWrap"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                    .childGap = 4, .layoutDirection = CLAY_TOP_TO_BOTTOM },
    }) {
        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                                   .childGap = 8 } }) {
            label_text(CLAY_STRING("WALL CLOCK"), VS_TEXT_DIM, 10);
            if (a->have_window) {
                char x[32], y[32];
                vs_fmt_clock(a->wall_lo, x, sizeof x);
                vs_fmt_clock(a->wall_hi, y, sizeof y);
                label_text(sfmt("%s - %s", x, y), VS_TEXT_DIM, 10);
                label_text(sfmt("span %.0f min", (a->wall_hi - a->wall_lo) / 60.0),
                           VS_TEXT_DIM, 10);
                // A live timeline starts where the app tuned in, not where the
                // stream did, so say so rather than letting it read as a VOD.
                if (vs_any_live(a))
                    label_text(a->live_mode ? CLAY_STRING("AT THE LIVE EDGE")
                                            : CLAY_STRING("BEHIND LIVE"),
                               a->live_mode ? VS_ERR : VS_WARN, 10);
            } else {
                label_text(CLAY_STRING("no sources loaded"), VS_TEXT_DIM, 10);
            }
        }

        CLAY(CLAY_ID("UniBar"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(30) },
                        .layoutDirection = CLAY_TOP_TO_BOTTOM },
            .backgroundColor = VS_BG,
            .cornerRadius = CLAY_CORNER_RADIUS(3),
            .border = { .color = VS_LINE, .width = CLAY_BORDER_OUTSIDE(1) },
        }) {
            // No early return here: CLAY() is a for-loop macro, so returning
            // out of its body skips the matching close.
            if (a->have_window) {
            double span = a->wall_hi - a->wall_lo;
            if (span < 1) span = 1;

            // One coverage lane per active source.
            for (int i = 0; i < VS_MAX_SOURCES; i++) {
                vs_source *s = &a->src[i];
                if (!s->active || s->state != VS_LOAD_READY) continue;
                double x0 = (s->pdt_start - s->offset - a->wall_lo) / span;
                double x1 = x0 + s->duration / span;
                if (x0 < 0) x0 = 0;
                if (x1 > 1) x1 = 1;
                if (x1 <= x0) continue;
                CLAY(CLAY_IDI("Lane", i), {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(5) } },
                }) {
                    CLAY_AUTO_ID({ .layout = { .sizing = {
                                       CLAY_SIZING_PERCENT((float)x0),
                                       CLAY_SIZING_FIXED(5) } } }) {}
                    CLAY_AUTO_ID({ .layout = { .sizing = {
                                       CLAY_SIZING_PERCENT((float)(x1 - x0)),
                                       CLAY_SIZING_FIXED(5) } },
                                   .backgroundColor = plat_color(s->platform) }) {}
                }
            }
            // Chat lane
            if (a->chat.state == VS_LOAD_READY && a->chat.n > 0 && a->chat.pdt_start > 0) {
                double cb = a->chat.pdt_start - a->chat.offset - a->wall_lo;
                double c0 = (cb + a->chat.msgs[0].t) / span;
                double c1 = (cb + a->chat.msgs[a->chat.n - 1].t) / span;
                if (c0 < 0) c0 = 0;
                if (c1 > 1) c1 = 1;
                if (c1 > c0) {
                    CLAY(CLAY_ID("ChatLane"), {
                        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(4) } },
                    }) {
                        CLAY_AUTO_ID({ .layout = { .sizing = {
                                           CLAY_SIZING_PERCENT((float)c0),
                                           CLAY_SIZING_FIXED(4) } } }) {}
                        CLAY_AUTO_ID({ .layout = { .sizing = {
                                           CLAY_SIZING_PERCENT((float)(c1 - c0)),
                                           CLAY_SIZING_FIXED(4) } },
                                       .backgroundColor = VS_WARN }) {}
                    }
                }
            }

            // Playhead + bookmarks float over the lanes.
            double ph = (a->wall - a->wall_lo) / span;
            if (ph < 0) ph = 0;
            if (ph > 1) ph = 1;
            CLAY(CLAY_ID("PlayheadRow"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
                .floating = { .zIndex = 2,
                              .attachPoints = { CLAY_ATTACH_POINT_LEFT_TOP,
                                                CLAY_ATTACH_POINT_LEFT_TOP },
                              .attachTo = CLAY_ATTACH_TO_PARENT,
                              .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH },
            }) {
                CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_PERCENT((float)ph),
                                                       CLAY_SIZING_FIXED(28) } } }) {}
                CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(2),
                                                       CLAY_SIZING_FIXED(28) } },
                               .backgroundColor = VS_TEXT }) {}
            }
            }   // end if (a->have_window)
        }
    }
}

// Per-source scrubber over that source's own duration.
static void source_bars(vs_app *a) {
    for (int i = 0; i < VS_MAX_SOURCES; i++) {
        vs_source *s = &a->src[i];
        if (!s->active) continue;
        CLAY(CLAY_IDI("SrcRow", i), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(20) },
                        .childGap = 8,
                        .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER } },
        }) {
            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(6),
                                                   CLAY_SIZING_FIXED(6) } },
                           .backgroundColor = plat_color(s->platform),
                           .cornerRadius = CLAY_CORNER_RADIUS(3) }) {}
            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(96),
                                                   CLAY_SIZING_FIT(0) } } }) {
                label_text(sfmt("%s", s->channel[0] ? s->channel : "..."),
                           VS_TEXT_DIM, 12);
            }
            double pos = s->state == VS_LOAD_READY ? vs_pos_for_wall(s, a->wall) : 0;
            double frac = (s->duration > 0) ? pos / s->duration : 0;
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            CLAY(CLAY_IDI("SrcBar", i), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(12) } },
                .backgroundColor = VS_BG,
                .cornerRadius = CLAY_CORNER_RADIUS(3),
            }) {
                CLAY_AUTO_ID({ .layout = { .sizing = {
                                   CLAY_SIZING_PERCENT((float)frac),
                                   CLAY_SIZING_FIXED(12) } },
                               .backgroundColor = (a->master == i) ? VS_ACCENT : VS_PANEL_HI,
                               .cornerRadius = CLAY_CORNER_RADIUS(3) }) {}
            }
            char pt[24], dt2[24];
            vs_fmt_dur(pos, pt, sizeof pt);
            vs_fmt_dur(s->duration, dt2, sizeof dt2);
            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(120),
                                                   CLAY_SIZING_FIT(0) } } }) {
                label_text(sfmt("%s / %s", pt, dt2), VS_TEXT_DIM, 11);
            }
        }
    }
}

static void timelines(vs_app *a) {
    CLAY(CLAY_ID("Timelines"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                    .padding = CLAY_PADDING_ALL(VS_PAD), .childGap = 6,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM },
        .backgroundColor = VS_PANEL,
    }) {
        unified_bar(a);
        source_bars(a);
    }
}

// ------------------------------------------------------------------- chat

// How wide the chat column may be.
//
// It used to be CLAY_SIZING_FIXED(sc(VS_CHAT_W)), and a fixed width cannot
// shrink. Text scale and display scale multiply - 2.0 x 2.0 is reachable on
// a high-DPI screen - so the column would ask for more than the window had.
// Clay cannot take space from a FIXED element, so it widened the parent
// instead and the column, being last, was pushed past the right edge:
// measured at 1320px wide sitting at x=1124 in a 1600px window. The video
// pane stayed, the chat was simply gone, and no error was raised - which is
// why it looked random rather than like a threshold.
//
// Two changes make it degrade instead of vanish. The preferred width is
// capped against the window, and GROW carries a floor, so Clay shrinks this
// column when space is short rather than overflowing. Worst case the chat
// is narrow and wraps more, which is still chat.
// The width the chat column is asking for, in real pixels.
static int chat_col_px(const vs_app *a) {
    int want = sc((a && a->chat_w > 0) ? a->chat_w : VS_CHAT_W);
    int cap  = (int)(GetScreenWidth() * 0.45f);
    if (cap  < VS_CHAT_W_MIN) cap  = VS_CHAT_W_MIN;
    if (want > cap)           want = cap;
    if (want < VS_CHAT_W_MIN) want = VS_CHAT_W_MIN;
    const char *ov = getenv("VODSYNC_CHAT_W");   // dev: pin it, to bisect
    if (ov) want = atoi(ov);
    static int said;
    if (getenv("VODSYNC_LAYOUT_LOG") && !said) {
        said = 1;
        fprintf(stderr, "[layout] chat want=%d cap=%d screen=%d scale=%.2f\n",
                want, cap, GetScreenWidth(), (double)g_scale);
    }
    return want;
}

static Clay_SizingAxis chat_col_width(const vs_app *a) {
    return CLAY_SIZING_GROW(VS_CHAT_W_MIN, (float)chat_col_px(a));
}

// How wide one message row may be. A row inside the list cannot work this
// out for itself: the list clips vertically, which makes it a scroll
// container, and Clay measures a scroll container's children against an
// UNBOUNDED width. With no limit to wrap at, CLAY_TEXT_WRAP_WORDS never
// wraps - every message lays out as one long line and runs off to the
// right, behind the column's clip. Measured: a row 1258px wide and exactly
// one line tall, 20px, in a 479px list.
//
// So the limit has to be handed down. The column's own width, less the
// padding between it and the text.
// Width of a slice of text at the chat font. MeasureTextEx rather than
// Clay's own measure function because this is line breaking Clay is not
// doing; the two agree closely, and a pixel either way only shifts where a
// line happens to break.
static float chat_text_w(const char *src, int len, int px) {
    char buf[640];
    if (len < 0) len = (int)strlen(src);
    if (len > (int)sizeof buf - 1) len = (int)sizeof buf - 1;
    memcpy(buf, src, (size_t)len);
    buf[len] = 0;
    if (!g_fonts) return (float)len * (float)px * 0.5f;
    return MeasureTextEx(g_fonts[VS_FONT_SMALL], buf, (float)px, 0.0f).x;
}

// One piece of a message: a run of text, or an emote.
typedef struct {
    int         emote;      // -1 for text
    const char *s;
    int         len;        // bytes of text (also the emote's own word)
    float       w, h;
} vs_chat_tok;

#define VS_TOK_MAX  48
#define VS_LINE_MAX 16

// Splits a message into tokens. An emote is either a span the log recorded -
// which is the only way a Twitch or Kick emote may appear - or a bare word
// that matches an overlay set. A span wins: it is explicit.
static int chat_tokens(vs_app *a, const vs_chat_msg *m, int px, float eh,
                       float avail, vs_chat_tok *out) {
    int n = 0;
    int tlen = (int)strlen(m->text);
    int i = 0;
    while (i < tlen && n < VS_TOK_MAX) {
        if (m->text[i] == ' ') { i++; continue; }

        int e = -1, len = 0;
        for (int sp = 0; sp < m->nspan; sp++) {
            const vs_emote_span *es = &a->chat.spans[m->span_at + sp];
            if ((int)es->start == i) { e = es->emote; len = es->len; break; }
        }
        if (e < 0) {
            int j = i;
            while (j < tlen && m->text[j] != ' ') j++;
            len = j - i;
            e = vs_emote_find_name(m->text + i, len);
        }
        if (len <= 0) break;

        vs_chat_tok *t = &out[n];
        t->s = m->text + i;
        t->len = len;
        t->emote = -1;
        if (e >= 0) {
            const vs_emote_img *im = vs_emote_img_get(e, VS_EMSZ_INLINE);
            // Not here yet: show the word. Asking is what started the
            // fetch, so it will be a picture on a later frame.
            if (im && im->fw > 0 && im->fh > 0) {
                t->emote = e;
                t->h = eh;
                t->w = eh * (float)im->fw / (float)im->fh;
                // Emotes are not all square - Kick serves native size, and
                // one measured 320x29 - so a wide one is scaled to fit the
                // column rather than cropped or stretched.
                if (t->w > avail && avail > 1.0f) {
                    t->w = avail;
                    t->h = avail * (float)im->fh / (float)im->fw;
                }
            }
        }
        if (t->emote < 0) {
            t->w = chat_text_w(t->s, t->len, px);
            t->h = (float)px;
        }
        n++;
        i += len;
    }
    return n;
}

// Defined after the chat list, because they are only called from there and
// read better next to what they draw.
static void chat_body(vs_app *a, const vs_chat_msg *m, int k);
static void chat_source_tiles(vs_app *a);
static void chat_add_from_browser(vs_app *a, const vs_vod *v);

static float chat_row_px(const vs_app *a) {
    float w = (float)chat_col_px(a) - (float)(sc(VS_PAD) * 2) - (float)(sc(6) * 2);
    if (w < 60.0f) w = 60.0f;
    return w;
}

static void chat_panel(vs_app *a) {
    // Folded away: a strip just wide enough for the button that brings it
    // back. Kept in the layout rather than removed so there is always
    // somewhere to click - a panel you cannot get back is not collapsed,
    // it is gone - and so the stage does not jump a second time when it
    // returns.
    if (a->chat_collapsed) {
        CLAY(CLAY_ID("ChatCol"), {
            .layout = { .sizing = { CLAY_SIZING_FIXED((float)sc(26)),
                                    CLAY_SIZING_GROW(0) },
                        .padding = { 0, 0, (uint16_t)sc(6), 0 }, .childGap = 4,
                        .childAlignment = { CLAY_ALIGN_X_CENTER,
                                            CLAY_ALIGN_Y_TOP },
                        .layoutDirection = CLAY_TOP_TO_BOTTOM },
            .backgroundColor = VS_PANEL,
            .clip = { .horizontal = true },
        }) {
            button(CLAY_ID("ChatExpand"), CLAY_STRING("<"), false, false);
        }
        return;
    }

    // The grab bar, on the column's left edge. Same idea as the split
    // divider: a thin sibling that lights up under the pointer, so the drag
    // does not fight the panel's own scrolling and clicking.
    CLAY(CLAY_ID("ChatGrip"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED((float)sc(6)),
                                CLAY_SIZING_GROW(0) } },
        .backgroundColor = (over(CLAY_ID("ChatGrip")) || a->drag == 50)
                           ? VS_ACCENT : VS_BG,
    }) {}

    CLAY(CLAY_ID("ChatCol"), {
        .layout = { .sizing = { chat_col_width(a), CLAY_SIZING_GROW(0) },
                    .padding = CLAY_PADDING_ALL(VS_PAD), .childGap = 6,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM },
        .backgroundColor = VS_PANEL,
        // Clip horizontally so a cramped header loses its right-hand edge
        // rather than widening the column.
        .clip = { .horizontal = true },
    }) {
        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                                   .childGap = 6,
                                   .childAlignment = { CLAY_ALIGN_X_LEFT,
                                                       CLAY_ALIGN_Y_CENTER } } }) {
            label_text(sfmt("%s", a->chat.label[0] ? a->chat.label : "Twitch chat"),
                       VS_TEXT, 14);
            spacer();
            if (fabs(a->chat.offset) > 0.0005)
                label_text(sfmt("%+.1fs", a->chat.offset), VS_WARN, 11);
            button(CLAY_ID("ChatNudgeL"), CLAY_STRING("-"), false, false);
            button(CLAY_ID("ChatNudgeR"), CLAY_STRING("+"), false, false);
            button(CLAY_ID("ChatCollapse"), CLAY_STRING(">"), false, false);
        }

        // Which Twitch VOD the log comes from. Listed here rather than
        // requiring a pane, because the chat you want is often not the video
        // you are watching.
        {
            int ntw = 0;
            (void)ntw;
            label_text(CLAY_STRING("SOURCES"), VS_TEXT_DIM, 11);
            chat_source_tiles(a);
        }

        // Export controls. "Auto" keeps topping the log up while the VOD is
        // still being recorded; a finished VOD is exported once.
        CLAY(CLAY_ID("ChatSyncRow"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                        .childGap = 6,
                        .childAlignment = { CLAY_ALIGN_X_LEFT,
                                            CLAY_ALIGN_Y_CENTER } },
        }) {
            bool busy = vs_chat_sync_busy(&a->chat);
            button(CLAY_ID("ChatSync"),
                   busy ? CLAY_STRING("...") : CLAY_STRING("Update now"),
                   busy, false);
            button(CLAY_ID("ChatAuto"), CLAY_STRING("Auto"),
                   a->chat.auto_sync, false);
            // How far the panel has got is how far its furthest source
            // has: the others are topped up to the same edge.
            double done = 0;
            for (int k = 0; k < a->chat.nsources; k++)
                if (a->chat.sources[k].exported_to > done)
                    done = a->chat.sources[k].exported_to;
            if (done > 0) {
                char e[24];
                vs_fmt_dur(done, e, sizeof e);
                label_text(sfmt("to %s", e), VS_TEXT_DIM, 11);
            }
        }
        if (a->chat.sync_status[0])
            label_text(sfmt("%s", a->chat.sync_status),
                       a->chat.sync_state == VS_LOAD_FAILED ? VS_ERR : VS_OK, 11);

        if (a->chat.state != VS_LOAD_READY) {
            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                                       .padding = CLAY_PADDING_ALL(8), .childGap = 8,
                                       .layoutDirection = CLAY_TOP_TO_BOTTOM },
                           .backgroundColor = VS_BG,
                           .cornerRadius = CLAY_CORNER_RADIUS(VS_RADIUS) }) {
                label_text(CLAY_STRING("No chat log yet."), VS_TEXT_DIM, 13);
                if (a->chat.nsources > 0) {
                    label_text(CLAY_STRING("Press Update now to export it"),
                               VS_TEXT_DIM, 12);
                    label_text(CLAY_STRING("from the loaded Twitch VOD."),
                               VS_TEXT_DIM, 12);
                } else {
                    label_text(CLAY_STRING("Load a Twitch VOD, drop a chat"),
                               VS_TEXT_DIM, 12);
                    label_text(CLAY_STRING(".json on the window, or press C."),
                               VS_TEXT_DIM, 12);
                }
                if (a->chat.state == VS_LOAD_FAILED)
                    label_text(sfmt("%s", a->chat.status), VS_ERR, 12);
                spacer();
                label_text(CLAY_STRING("Chat syncs on its own offset, so it"),
                           VS_TEXT_DIM, 11);
                label_text(CLAY_STRING("works even with no video for that VOD."),
                           VS_TEXT_DIM, 11);
            }
        } else {

        // Find. It sits with the list rather than up in the header
        // because it acts on the list: with a query up, the hits are what
        // the list shows.
        CLAY(CLAY_ID("ChatFindRow"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                        .childGap = 6,
                        .childAlignment = { CLAY_ALIGN_X_LEFT,
                                            CLAY_ALIGN_Y_CENTER } },
        }) {
            text_field(a, CLAY_ID("ChatFind"), VS_EDIT_CHATFIND,
                       g_find.query[0] ? sfmt("%s", g_find.query)
                                       : CLAY_STRING("search chat"));
            if (g_find.query[0])
                button(CLAY_ID("ChatFindClear"), CLAY_STRING("x"), false, false);
        }

        // The list follows the field as it is typed in, so there is no Enter
        // to forget and no stale result sitting under a changed query.
        if (a->editing == VS_EDIT_CHATFIND)
            snprintf(g_find.query, sizeof g_find.query, "%s", a->edit_buf);
        // Dev: a headless run has no keyboard, so let the environment ask
        // the question once. Set at startup only, so clearing still works.
        {
            static bool from_env;
            const char *fv = getenv("VODSYNC_FIND");
            if (!from_env && fv && fv[0]) {
                snprintf(g_find.query, sizeof g_find.query, "%s", fv);
                from_env = true;
            }
        }
        find_rebuild(&a->chat);
        // Dev: open the who-said-what popup for a name, since a headless
        // run cannot click one.
        {
            static bool who_env_done;
            const char *wv = getenv("VODSYNC_WHO");
            if (!who_env_done && wv && wv[0]) {
                who_env_done = true;
                who_open(&a->chat, wv);
            }
        }

        int first = 0, count = 0;
        if (!g_find.query[0]) vs_chat_window(&a->chat, 60, &first, &count);
        // Dev: force rows into the ordinary list. It normally draws only
        // what the playhead has passed, which needs a loaded video, so
        // without this the list cannot be measured headlessly at all -
        // and it was an unmeasurable list that hid the wrapping bug for
        // as long as it did.
        {
            const char *fw = getenv("VODSYNC_FAKEWIN");
            if (fw && !g_find.query[0]) {
                first = 0;
                count = atoi(fw);
                if (count > a->chat.n) count = a->chat.n;
            }
        }

        // Pin the list to its newest message rather than carrying a free
        // scroll offset. The offset is absolute, while the content height
        // changes every time a message arrives or the text size changes, so
        // a stale value can park the viewport past the end and the panel
        // looks empty. Chat is tied to the playhead anyway: scrubbing is how
        // you move through it, so there is nothing to lose by anchoring it.
        Clay_Vector2 chat_off = { 0, 0 };
        if (g_find.query[0]) {
            // Hits are not tied to the playhead, so this list scrolls like
            // any other. Pinning it would drag you away from the match you
            // were reading the moment another message arrived.
            chat_off = Clay_GetScrollOffset();
        } else {
            Clay_ScrollContainerData sd =
                Clay_GetScrollContainerData(CLAY_ID("ChatList"));
            if (sd.found) {
                float over = sd.contentDimensions.height -
                             sd.scrollContainerDimensions.height;
                if (over > 0) chat_off.y = -over;
            }
        }

        CLAY(CLAY_ID("ChatList"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                        .padding = CLAY_PADDING_ALL(6), .childGap = 4,
                        .layoutDirection = CLAY_TOP_TO_BOTTOM,
                        .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP } },
            .backgroundColor = VS_BG,
            .cornerRadius = CLAY_CORNER_RADIUS(VS_RADIUS),
            .clip = { .vertical = true, .childOffset = chat_off },
        }) {
            for (int k = 0; k < g_find.nhits; k++) {
                int idx = g_find.hits[k];
                vs_chat_msg *m = &a->chat.msgs[idx];
                Clay_Color uc = { (float)m->r, (float)m->g, (float)m->b, 255 };
                char ts[24];
                vs_fmt_dur(m->t, ts, sizeof ts);
                CLAY(CLAY_IDI("ChatHit", k), {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0, chat_row_px(a)),
                                            CLAY_SIZING_FIT(0) },
                                .padding = { (uint16_t)sc(3), (uint16_t)sc(3),
                                             (uint16_t)sc(2), (uint16_t)sc(2) },
                                .childGap = 5 },
                    .backgroundColor = over(CLAY_IDI("ChatHit", k))
                                       ? VS_PANEL_HI : VS_BG,
                    .cornerRadius = CLAY_CORNER_RADIUS(VS_RADIUS),
                }) {
                    CLAY_TEXT(sfmt("%s", ts),
                              CLAY_TEXT_CONFIG({ .textColor = VS_ACCENT,
                                                 .fontId = VS_FONT_MICRO, .fontSize = font_px_for(VS_SIZE_MICRO),
                                                 .wrapMode = CLAY_TEXT_WRAP_NONE }));
                    CLAY(CLAY_IDI("ChatHitUser", k), {
                        .layout = { .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) } },
                        .backgroundColor = over(CLAY_IDI("ChatHitUser", k))
                                           ? VS_PANEL_HI : VS_CLEAR,
                        .cornerRadius = CLAY_CORNER_RADIUS(3),
                    }) {
                        CLAY_TEXT(sfmt("%s", m->user),
                                  CLAY_TEXT_CONFIG({ .textColor = uc,
                                                     .fontId = VS_FONT_SMALL, .fontSize = font_px_for(VS_SIZE_SMALL),
                                                     .wrapMode = CLAY_TEXT_WRAP_NONE }));
                    }
                    CLAY_TEXT(chat_wrappable(m->text),
                              CLAY_TEXT_CONFIG({ .textColor = VS_TEXT,
                                                 .fontId = VS_FONT_SMALL, .fontSize = font_px_for(VS_SIZE_SMALL),
                                                 .wrapMode = CLAY_TEXT_WRAP_WORDS }));
                }
            }
            if (g_find.query[0] && g_find.nhits == 0)
                label_text(CLAY_STRING("no matches"), VS_TEXT_DIM, 12);

            g_row_n = 0;
            for (int k = 0; k < count; k++) {
                int idx = first + k;
                vs_chat_msg *m = &a->chat.msgs[idx];
                if (k < VS_ROW_MAX) { g_row_msg[k] = idx; g_row_n = k + 1; }
                Clay_Color uc = { (float)m->r, (float)m->g, (float)m->b, 255 };
                CLAY(CLAY_IDI("ChatMsg", k), {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0, chat_row_px(a)),
                                            CLAY_SIZING_FIT(0) },
                                .childGap = 5,
                                .childAlignment = { CLAY_ALIGN_X_LEFT,
                                                    CLAY_ALIGN_Y_TOP } },
                }) {
                    // Which chat this line came from, in that platform's own
                    // colour. A dot rather than either brand's logo, which
                    // is the same choice the app mark makes and for the same
                    // reason. Absent on a version 1 log, which records no
                    // source rather than guessing it was Twitch.
                    if (m->src != VS_SRC_NONE && m->src < a->chat.nsources) {
                        vs_platform mp = a->chat.sources[m->src].platform;
                        CLAY(CLAY_IDI("ChatDot", k), {
                            .layout = { .sizing = {
                                CLAY_SIZING_FIXED((float)sc(6)),
                                CLAY_SIZING_FIXED((float)sc(6)) } },
                            .backgroundColor = plat_color(mp),
                            .cornerRadius = CLAY_CORNER_RADIUS((float)sc(3)),
                        }) {}
                    }
                    CLAY(CLAY_IDI("ChatUser", k), {
                        .layout = { .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) } },
                        .backgroundColor = over(CLAY_IDI("ChatUser", k))
                                           ? VS_PANEL_HI : VS_CLEAR,
                        .cornerRadius = CLAY_CORNER_RADIUS(3),
                    }) {
                        CLAY_TEXT(sfmt("%s", m->user),
                                  CLAY_TEXT_CONFIG({ .textColor = uc,
                                                     .fontId = VS_FONT_SMALL, .fontSize = font_px_for(VS_SIZE_SMALL),
                                                     .wrapMode = CLAY_TEXT_WRAP_NONE }));
                    }
                    chat_body(a, m, k);
                }
            }
        }
        if (g_find.query[0])
            label_text(sfmt("%d match%s%s", g_find.nhits,
                            g_find.nhits == 1 ? "" : "es",
                            g_find.truncated ? ", showing the first 500" : ""),
                       VS_TEXT_DIM, 10);
        else
            label_text(sfmt("%d / %d messages", a->chat.cursor, a->chat.n), VS_TEXT_DIM, 10);
        }   // end else (chat ready)
    }
}

// Adds a browser row as a chat source and starts its export.
//
// The anchor decides where every message lands, so a loaded pane's
// PROGRAM-DATE-TIME is preferred over the listing's timestamp: the two were
// measured 2.4s apart, which is small enough to look like nothing and large
// enough to see in chat.
static void chat_add_from_browser(vs_app *a, const vs_vod *v) {
    double anchor = v->ts;
    for (int k = 0; k < VS_MAX_SOURCES; k++) {
        vs_source *s = &a->src[k];
        if (s->active && s->pdt_start > 0 && s->page_url[0] &&
            strcmp(s->page_url, v->url) == 0) {
            anchor = s->pdt_start;
            break;
        }
    }
    int i = vs_chat_source_add(&a->chat, v->platform, g_browser.query, "",
                               v->url, anchor, v->dur);
    vs_browse_close(&g_browser);
    if (i < 0) {
        vs_toast(a, "that chat is already in the panel (or there is no room)");
        return;
    }
    vs_chat_merge(&a->chat);              // show whatever is already exported
    vs_chat_sync_source(&a->chat, i, a->chat.sources[i].exported_to);
    vs_toast(a, "added %s chat", vs_platform_name(v->platform));
}

// One tile per chat source: picture, platform-coloured border, and an x.
static void chat_source_tiles(vs_app *a) {
    CLAY(CLAY_ID("ChatTiles"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                    .childGap = 5,
                    .childAlignment = { CLAY_ALIGN_X_LEFT,
                                        CLAY_ALIGN_Y_CENTER } },
    }) {
        for (int i = 0; i < a->chat.nsources; i++) {
            vs_chat_source *s = &a->chat.sources[i];
            const char *plat = (s->platform == VS_PLAT_KICK) ? "kick" : "twitch";

            // Asked for lazily, the way emotes are: the sets request is
            // idempotent, and the avatar turns up in those same documents.
            if (s->user_id[0]) {
                vs_emote_sets_request(plat, s->user_id);
                if (s->avatar < 0) {
                    const char *av = vs_emote_sets_avatar(plat, s->user_id);
                    if (av[0]) {
                        char key[64];
                        snprintf(key, sizeof key, "av_%s_%s", s->tag,
                                 s->user_id);
                        s->avatar = vs_img_url(key, av);
                    }
                }
            }
            const vs_emote_img *im =
                (s->avatar >= 0) ? vs_img_get(s->avatar) : NULL;

            CLAY(CLAY_IDI("ChatTile", i), {
                .layout = { .sizing = { CLAY_SIZING_FIT(0),
                                        CLAY_SIZING_FIT(0) },
                            .padding = CLAY_PADDING_ALL((uint16_t)sc(3)),
                            .childGap = (uint16_t)sc(4),
                            .childAlignment = { CLAY_ALIGN_X_LEFT,
                                                CLAY_ALIGN_Y_CENTER } },
                .backgroundColor = VS_PANEL,
                .cornerRadius = CLAY_CORNER_RADIUS((float)sc(5)),
                .border = { .color = plat_color(s->platform),
                            .width = CLAY_BORDER_OUTSIDE(2) },
            }) {
                float px = (float)sc(24);
                if (im) {
                    g_av_custom[i].kind  = VS_CUSTOM_AVATAR;
                    g_av_custom[i].index = s->avatar;
                    CLAY(CLAY_IDI("ChatAv", i), {
                        .layout = { .sizing = { CLAY_SIZING_FIXED(px),
                                                CLAY_SIZING_FIXED(px) } },
                        .custom = { .customData = &g_av_custom[i] },
                    }) {}
                } else {
                    // No picture: the channel's initial on the platform's
                    // colour. Kick serves avatars only as WebP, so this is
                    // the usual case there rather than an error state.
                    char ini[8];
                    char c0 = s->channel[0] ? s->channel[0] : '?';
                    if (c0 >= 'a' && c0 <= 'z') c0 -= 32;
                    snprintf(ini, sizeof ini, "%c", c0);
                    CLAY(CLAY_IDI("ChatAvL", i), {
                        .layout = { .sizing = { CLAY_SIZING_FIXED(px),
                                                CLAY_SIZING_FIXED(px) },
                                    .childAlignment = { CLAY_ALIGN_X_CENTER,
                                                        CLAY_ALIGN_Y_CENTER } },
                        .backgroundColor = plat_color(s->platform),
                        .cornerRadius = CLAY_CORNER_RADIUS(px * 0.5f),
                    }) {
                        CLAY_TEXT(sfmt("%s", ini),
                                  CLAY_TEXT_CONFIG({
                                      .textColor = VS_BG,
                                      .fontId = VS_FONT_SMALL,
                                      .fontSize = font_px_for(VS_SIZE_SMALL),
                                      .wrapMode = CLAY_TEXT_WRAP_NONE }));
                    }
                }
                label_text(sfmt("%s", s->channel[0] ? s->channel : "chat"),
                           VS_TEXT, 11);
                if (s->sync_state == VS_LOAD_RESOLVING)
                    label_text(CLAY_STRING("..."), VS_WARN, 11);
                else if (s->sync_state == VS_LOAD_FAILED)
                    label_text(CLAY_STRING("!"), VS_ERR, 11);
                button(CLAY_IDI("ChatTileX", i), CLAY_STRING("x"), false,
                       false);
            }
        }
        button(CLAY_ID("ChatTileAdd"), CLAY_STRING("+"), false, false);
    }
}

// Draws a message's body: words, and pictures where an emote belongs.
//
// Falls back to one wrapping CLAY_TEXT when there is nothing to inline,
// which is every message in a version 1 log and most messages in any log -
// so the common case keeps Clay's own word wrapping and costs nothing.
static void chat_body(vs_app *a, const vs_chat_msg *m, int k) {
    int px = font_px_for(VS_SIZE_SMALL);
    float eh = (float)px * 1.45f;
    // What the body has to fit in: the row, less the name and the gaps
    // already spent on it. Approximate by a few pixels, which only moves
    // where a line breaks.
    float avail = chat_row_px(a) - chat_text_w(m->user, -1, px) - (float)sc(18);
    if (avail < 40.0f) avail = 40.0f;

    vs_chat_tok tok[VS_TOK_MAX];
    int nt = chat_tokens(a, m, px, eh, avail, tok);

    bool any_emote = false;
    for (int i = 0; i < nt; i++) if (tok[i].emote >= 0) any_emote = true;
    if (!any_emote) {
        CLAY_TEXT(chat_wrappable(m->text),
                  CLAY_TEXT_CONFIG({ .textColor = VS_TEXT,
                                     .fontId = VS_FONT_SMALL,
                                     .fontSize = font_px_for(VS_SIZE_SMALL),
                                     .wrapMode = CLAY_TEXT_WRAP_WORDS }));
        return;
    }

    float space_w = chat_text_w(" ", 1, px);
    // Break first, emit second. Clay's element macro opens a scope, so rows
    // can be produced one after another but their number cannot be decided
    // part way through nesting them.
    int first[VS_LINE_MAX], count[VS_LINE_MAX], nl = 0;
    float x = 0;
    first[0] = 0; count[0] = 0;
    for (int i = 0; i < nt; i++) {
        float need = (count[nl] > 0 ? space_w : 0.0f) + tok[i].w;
        if (count[nl] > 0 && x + need > avail && nl + 1 < VS_LINE_MAX) {
            nl++;
            first[nl] = i;
            count[nl] = 0;
            x = 0;
            need = tok[i].w;
        }
        count[nl]++;
        x += need;
    }
    nl++;

    CLAY(CLAY_IDI("ChatBody", k), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                    .layoutDirection = CLAY_TOP_TO_BOTTOM },
    }) {
        for (int L = 0; L < nl; L++) {
            CLAY(CLAY_IDI("ChatLine", k * VS_LINE_MAX + L), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                        CLAY_SIZING_FIT(0) },
                            .childGap = (uint16_t)(space_w + 0.5f),
                            .childAlignment = { CLAY_ALIGN_X_LEFT,
                                                CLAY_ALIGN_Y_CENTER } },
            }) {
                for (int i = first[L]; i < first[L] + count[L]; i++) {
                    if (tok[i].emote < 0) {
                        CLAY_TEXT(sfmt("%.*s", tok[i].len, tok[i].s),
                                  CLAY_TEXT_CONFIG({
                                      .textColor = VS_TEXT,
                                      .fontId = VS_FONT_SMALL,
                                      .fontSize = (uint16_t)px,
                                      .wrapMode = CLAY_TEXT_WRAP_NONE }));
                        continue;
                    }
                    if (g_em_draw_n >= VS_EM_DRAW_MAX) continue;
                    int slot = g_em_draw_n++;
                    g_em_draw[slot].emote = tok[i].emote;
                    g_em_draw[slot].size  = (unsigned char)VS_EMSZ_INLINE;
                    g_em_custom[slot].kind  = VS_CUSTOM_EMOTE;
                    g_em_custom[slot].index = slot;
                    Clay_ElementId eid =
                        CLAY_IDI("EmTok", k * VS_TOK_MAX + i);
                    if (over(eid)) g_em_hover = tok[i].emote;
                    CLAY(eid, {
                        .layout = { .sizing = {
                            CLAY_SIZING_FIXED(tok[i].w),
                            CLAY_SIZING_FIXED(tok[i].h) } },
                        .custom = { .customData = &g_em_custom[slot] },
                    }) {}
                }
            }
        }
    }
}

// ------------------------------------------------------------------- help

// -------------------------------------------------- who-said-what overlay

static void who_overlay(vs_app *a) {
    who_rebuild(&a->chat);

    // Fixed width, which is also what gives the rows a maximum to wrap
    // inside - the list clips, and a clipped container hands its children
    // an unbounded width. See "Three bugs under the chat list" in NOTES.
    float pw   = (float)sc(420);
    float pad  = (float)sc(12);
    float roww = pw - pad * 2;

    int shown = 0;
    for (int k = 0; k < g_who.n; k++)
        if (g_who.idx[k] <= a->chat.cursor) shown++;
    g_who_shown = shown;

    Clay_Color uc = VS_TEXT;
    if (g_who.n > 0) {
        vs_chat_msg *m0 = &a->chat.msgs[g_who.idx[0]];
        uc = (Clay_Color){ (float)m0->r, (float)m0->g, (float)m0->b, 255 };
    }

    CLAY(CLAY_ID("WhoPop"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(pw), CLAY_SIZING_FIT(0) },
                    .padding = CLAY_PADDING_ALL((uint16_t)pad), .childGap = (uint16_t)sc(6),
                    .layoutDirection = CLAY_TOP_TO_BOTTOM },
        .backgroundColor = VS_PANEL_HI,
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .floating = { .zIndex = 21,
                      .attachPoints = { CLAY_ATTACH_POINT_CENTER_CENTER,
                                        CLAY_ATTACH_POINT_CENTER_CENTER },
                      .attachTo = CLAY_ATTACH_TO_ROOT },
        .border = { .color = VS_ACCENT, .width = CLAY_BORDER_OUTSIDE(1) },
    }) {
        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                                   .childGap = (uint16_t)sc(6),
                                   .childAlignment = { CLAY_ALIGN_X_LEFT,
                                                       CLAY_ALIGN_Y_CENTER } } }) {
            CLAY_TEXT(sfmt("%s", g_who.who),
                      CLAY_TEXT_CONFIG({ .textColor = uc,
                                         .fontId = VS_FONT_BODY, .fontSize = font_px_for(VS_SIZE_BODY),
                                         .wrapMode = CLAY_TEXT_WRAP_NONE }));
            spacer();
            button(CLAY_ID("WhoClose"), CLAY_STRING("x"), false, false);
        }

        label_text(sfmt("%d message%s so far%s", shown, shown == 1 ? "" : "s",
                        g_who.truncated ? " (last 300 kept)" : ""),
                   VS_TEXT_DIM, 11);

        CLAY(CLAY_ID("WhoList"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                    CLAY_SIZING_GROW(0, (float)sc(360)) },
                        .padding = CLAY_PADDING_ALL((uint16_t)sc(6)),
                        .childGap = (uint16_t)sc(5),
                        .layoutDirection = CLAY_TOP_TO_BOTTOM,
                        .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP } },
            .backgroundColor = VS_BG,
            .cornerRadius = CLAY_CORNER_RADIUS(VS_RADIUS),
            .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() },
        }) {
            int drawn = 0;
            for (int k = 0; k < g_who.n; k++) {
                int idx = g_who.idx[k];
                if (idx > a->chat.cursor) break;   // ascending: nothing later
                vs_chat_msg *m = &a->chat.msgs[idx];
                char ts[24];
                vs_fmt_dur(m->t, ts, sizeof ts);
                drawn++;
                CLAY(CLAY_IDI("WhoMsg", k), {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0, roww - (float)sc(12)),
                                            CLAY_SIZING_FIT(0) },
                                .childGap = (uint16_t)sc(5) },
                }) {
                    CLAY_TEXT(sfmt("%s", ts),
                              CLAY_TEXT_CONFIG({ .textColor = VS_ACCENT,
                                                 .fontId = VS_FONT_MICRO, .fontSize = font_px_for(VS_SIZE_MICRO),
                                                 .wrapMode = CLAY_TEXT_WRAP_NONE }));
                    CLAY_TEXT(chat_wrappable(m->text),
                              CLAY_TEXT_CONFIG({ .textColor = VS_TEXT,
                                                 .fontId = VS_FONT_SMALL, .fontSize = font_px_for(VS_SIZE_SMALL),
                                                 .wrapMode = CLAY_TEXT_WRAP_WORDS }));
                }
            }
            if (drawn == 0)
                label_text(CLAY_STRING("nothing from them yet at this point"),
                           VS_TEXT_DIM, 12);
        }

        label_text(CLAY_STRING("up to the playhead - scrub on to see more"),
                   VS_TEXT_DIM, 10);
    }
}

static void help_overlay(void) {
    static const char *rows[] = {
        "Space        play / pause",
        "Left/Right   seek 5s   (+Shift 30s)",
        "1 - 4        focus pane",
        "[ / ]        nudge focused pane -/+ 1 frame (~0.033s)",
        "- / =        nudge focused pane -/+ 1 second",
        "Shift+[ ]    nudge chat offset",
        "M            make focused pane the master",
        "S            solo focused pane",
        "B            add bookmark      , / .  prev / next bookmark",
        "G            jump to the live edge (live streams)",
        "L            cycle layout      V  toggle split orientation",
        "X            swap the split panels (left/right, or top/bottom)",
        "F11          borderless fullscreen (or the toolbar button)",
        "T            theater mode - chrome hides until the mouse nears it",
        "Ctrl +/- /0  text size",
        "backslash    chat panel: full, a strip, hidden",
        "drag its left edge   resize the chat panel",
        "double-click a pane   solo + chat, and back again",
        "F            find VODs by channel name",
        "Ctrl+F       search the chat log; click a hit to jump there",
        "click a name  their messages up to the playhead (Esc closes)",
        "U            update chat now (export from the Twitch VOD)",
        "C            load chat log (paste path, or drop a .json)",
        "chat tab     'Chat from another VOD' - chat you are not watching",
        "Crop         pane footer: fill the pane instead of letterboxing",
        "drag the PiP corner   resize it; both axes, from that corner",
        "Reset        toolbar: clears VODs, chat, marks and channels",
        "Ctrl+S / O   save / load session",
        "F12          Clay debug inspector",
    };
    CLAY(CLAY_ID("Help"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(overlay_w(430)),
                                CLAY_SIZING_FIT(0) },
                    .padding = CLAY_PADDING_ALL(14), .childGap = 6,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM },
        .backgroundColor = VS_PANEL_HI,
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .floating = { .zIndex = 20,
                      .attachPoints = { CLAY_ATTACH_POINT_CENTER_CENTER,
                                        CLAY_ATTACH_POINT_CENTER_CENTER },
                      .attachTo = CLAY_ATTACH_TO_ROOT },
        .border = { .color = VS_ACCENT, .width = CLAY_BORDER_OUTSIDE(1) },
    }) {
        label_text(CLAY_STRING("Keys"), VS_TEXT, 16);
        for (int i = 0; i < (int)(sizeof rows / sizeof rows[0]); i++)
            label_text(sfmt("%s", rows[i]), VS_TEXT_DIM, 13);
    }
}

// ------------------------------------------------------------ status line

static void status_line(vs_app *a) {
    CLAY(CLAY_ID("Status"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED((float)sc(22)) },
                    .padding = { VS_PAD, VS_PAD, 0, 0 }, .childGap = 10,
                    .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER } },
        .backgroundColor = VS_BG,
    }) {
        if (a->toast[0]) label_text(sfmt("%s", a->toast), VS_OK, 12);
        else if (a->editing >= 0)
            label_text(CLAY_STRING("typing a url - Enter to load, Esc to cancel"),
                       VS_ACCENT, 12);
        else label_text(CLAY_STRING("? for keys"), VS_TEXT_DIM, 12);
        spacer();
        int ready = 0;
        for (int i = 0; i < VS_MAX_SOURCES; i++)
            if (a->src[i].active && a->src[i].state == VS_LOAD_READY) ready++;
        label_text(sfmt("%d source%s", ready, ready == 1 ? "" : "s"), VS_TEXT_DIM, 12);
        label_text(sfmt("%d fps", GetFPS()), VS_TEXT_DIM, 12);
    }
}

// ------------------------------------------------------------------ input

static void commit_edit(vs_app *a) {
    if (a->editing < 0) return;
    int slot = a->editing;
    char buf[VS_URL_MAX];
    snprintf(buf, sizeof buf, "%s", a->edit_buf);
    a->editing = -1;
    a->edit_buf[0] = 0;
    a->edit_len = 0;
    if (!buf[0]) return;

    if (slot == VS_EDIT_CHANNEL) {           // channel lookup
        vs_browse_open(&g_browser, g_browser.slot);
        if (!vs_browse_begin(&g_browser, buf))
            vs_toast(a, "could not start channel lookup");
        return;
    }
    if (slot == VS_EDIT_CHAT) {              // chat path
        if (a->chat.pdt_start <= 0) {
            // Anchor to any Twitch source we already have.
            for (int i = 0; i < VS_MAX_SOURCES; i++)
                if (a->src[i].active && a->src[i].platform == VS_PLAT_TWITCH &&
                    a->src[i].state == VS_LOAD_READY) {
                    vs_chat_set_anchor(&a->chat, a->src[i].pdt_start);
                    break;
                }
        }
        if (vs_chat_load(&a->chat, buf)) {
            vs_recompute_window(a);
            vs_chat_reseek(&a->chat, a->wall);
            vs_toast(a, "chat: %s", a->chat.status);
        } else {
            vs_toast(a, "chat failed: %s", a->chat.status);
        }
        return;
    }
    if (slot == VS_EDIT_CHATVOD) {           // chat from an unwatched VOD
        // Added as a chat-only source rather than bound here: binding needs
        // the VOD's PROGRAM-DATE-TIME, which is not known until it
        // resolves. vs_tick's binder already prefers a chat-only source, so
        // clearing the manual binding hands it over the moment it is ready.
        int added = vs_add_source(a, buf, VS_ROLE_CHAT);
        if (added >= 0) {
            a->chat.manual = false;
            a->chat_vis = VS_CHAT_SHOW;
            vs_toast(a, "chat source loading in slot %d", added + 1);
        }
        return;
    }
    if (slot == VS_EDIT_CHATFIND) {   // the list already follows the field
        snprintf(g_find.query, sizeof g_find.query, "%s", buf);
        return;
    }
    vs_add_source_at(a, buf, VS_ROLE_VIDEO, slot);
}

// Test hook: types a url into the chat panel's "chat from another VOD"
// field and presses Enter, without a keyboard. Goes through commit_edit, so
// it exercises the shipped branch rather than a copy of it.
void vs_ui_chat_vod_commit(vs_app *a, const char *url) {
    a->editing = VS_EDIT_CHATVOD;
    snprintf(a->edit_buf, sizeof a->edit_buf, "%s", url ? url : "");
    a->edit_len = (int)strlen(a->edit_buf);
    commit_edit(a);
}

static void handle_text_entry(vs_app *a) {
    int ch;
    while ((ch = GetCharPressed()) != 0) {
        if (ch >= 32 && ch < 127 && a->edit_len < (int)sizeof a->edit_buf - 1) {
            a->edit_buf[a->edit_len++] = (char)ch;
            a->edit_buf[a->edit_len] = 0;
        }
    }
    if (IsKeyPressed(KEY_BACKSPACE) && a->edit_len > 0) {
        if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) {
            a->edit_len = 0;
        } else {
            a->edit_len--;
        }
        a->edit_buf[a->edit_len] = 0;
    }
    if (IsKeyPressed(KEY_V) && (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))) {
        const char *clip = GetClipboardText();
        if (clip) {
            snprintf(a->edit_buf, sizeof a->edit_buf, "%s", clip);
            a->edit_len = (int)strlen(a->edit_buf);
        }
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) commit_edit(a);
    if (IsKeyPressed(KEY_ESCAPE)) {
        // Leaving the find field with the query still filtering the list
        // would be a search you cannot see the field for. Escape means
        // "never mind", so it takes the search with it.
        if (a->editing == VS_EDIT_CHATFIND) find_clear();
        a->editing = -1;
        a->edit_buf[0] = 0;
        a->edit_len = 0;
        if (g_browser.open) vs_browse_close(&g_browser);
    }
}

// Is there a chat column this frame? The reasons it appears on its own -
// something loaded, something failing, an export running, or solo, where
// there is room for it anyway - with the user's own choice on top.
static bool chat_auto_worthy(const vs_app *a) {
    return (a->chat.state == VS_LOAD_READY) ||
           (a->editing == VS_EDIT_CHAT) ||
           (a->editing == VS_EDIT_CHATVOD) ||
           (a->chat.state == VS_LOAD_FAILED) ||
           a->chat.auto_sync || a->chat.nsources > 0 ||
           vs_chat_sync_busy(&a->chat) ||
           a->layout == VS_LAYOUT_SOLO;
}

static bool chat_visible(const vs_app *a) {
    if (a->chat_vis == VS_CHAT_SHOW) return true;
    if (a->chat_vis == VS_CHAT_HIDE) return false;
    return chat_auto_worthy(a);
}

static void handle_keys(vs_app *a) {
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    bool ctrl  = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);

    if (ctrl && IsKeyPressed(KEY_S)) {
        char p[512];
        vs_session_default_path(p, sizeof p);
        vs_toast(a, vs_session_save(a, p) ? "session saved" : "save failed");
        return;
    }
    if (ctrl && IsKeyPressed(KEY_F)) {
        // A folded chat panel has nowhere to show the field or the hits.
        if (a->chat_collapsed) a->chat_collapsed = false;
        a->editing = VS_EDIT_CHATFIND;
        snprintf(a->edit_buf, sizeof a->edit_buf, "%s", g_find.query);
        a->edit_len = (int)strlen(a->edit_buf);
        return;
    }
    if (ctrl && IsKeyPressed(KEY_O)) {
        char p[512];
        vs_session_default_path(p, sizeof p);
        if (!vs_session_load(a, p)) vs_toast(a, "no saved session");
        return;
    }
    if (IsKeyPressed(KEY_SPACE)) vs_toggle_play(a);
    if (IsKeyPressed(KEY_RIGHT)) vs_seek_wall(a, a->wall + (shift ? 30 : 5));
    if (IsKeyPressed(KEY_LEFT))  vs_seek_wall(a, a->wall - (shift ? 30 : 5));
    for (int i = 0; i < VS_MAX_SOURCES; i++)
        if (IsKeyPressed(KEY_ONE + i)) a->focus = i;

    double frame = 1.0 / 30.0;
    if (IsKeyPressed(KEY_LEFT_BRACKET)) {
        if (shift) { a->chat.offset -= 0.5; vs_chat_reseek(&a->chat, a->wall);
                     vs_toast(a, "chat offset %+.1fs", a->chat.offset); }
        else vs_nudge(a, a->focus, -frame);
    }
    if (IsKeyPressed(KEY_RIGHT_BRACKET)) {
        if (shift) { a->chat.offset += 0.5; vs_chat_reseek(&a->chat, a->wall);
                     vs_toast(a, "chat offset %+.1fs", a->chat.offset); }
        else vs_nudge(a, a->focus, frame);
    }
    // Ctrl + these resizes the UI text; bare keys still nudge the offset.
    if (IsKeyPressed(KEY_MINUS)) {
        if (ctrl) vs_ui_set_text_scale(a, a->text_scale - 0.1f);
        else      vs_nudge(a, a->focus, -1.0);
    }
    if (IsKeyPressed(KEY_EQUAL)) {
        if (ctrl) vs_ui_set_text_scale(a, a->text_scale + 0.1f);
        else      vs_nudge(a, a->focus,  1.0);
    }
    if (ctrl && IsKeyPressed(KEY_ZERO)) vs_ui_set_text_scale(a, 1.0f);
    if (IsKeyPressed(KEY_BACKSLASH)) {
        // Three states on one key: full, a strip, gone. Two was not enough
        // - with the panel absent there was nothing to collapse, and the
        // toast said "chat collapsed" about a panel that was never there.
        if (!chat_visible(a)) {
            a->chat_vis = VS_CHAT_SHOW;
            a->chat_collapsed = false;
            vs_toast(a, a->chat.state == VS_LOAD_READY
                        ? "chat panel shown"
                        : "chat panel shown - press U to export a log, or C to load one");
        } else if (!a->chat_collapsed) {
            a->chat_vis = VS_CHAT_SHOW;
            a->chat_collapsed = true;
            vs_toast(a, "chat collapsed to a strip");
        } else {
            a->chat_vis = VS_CHAT_HIDE;
            a->chat_collapsed = false;
            vs_toast(a, "chat hidden");
        }
    }
    if (IsKeyPressed(KEY_F11)) {
        ToggleBorderlessWindowed();
        a->fullscreen = !a->fullscreen;
    }

    if (IsKeyPressed(KEY_M)) vs_set_master(a, a->focus);
    if (IsKeyPressed(KEY_S) && !ctrl) {
        if (a->src[a->focus].active) {
            a->src[a->focus].solo = !a->src[a->focus].solo;
            vs_apply_audio(a);
        }
    }
    if (IsKeyPressed(KEY_B)) vs_bookmark_add(a);
    if (IsKeyPressed(KEY_G)) vs_go_live(a);
    if (IsKeyPressed(KEY_COMMA))  vs_bookmark_jump(a, -1);
    if (IsKeyPressed(KEY_PERIOD)) vs_bookmark_jump(a, +1);
    if (IsKeyPressed(KEY_L)) a->layout = (vs_layout)((a->layout + 1) % VS_LAYOUT_COUNT);
    if (IsKeyPressed(KEY_V)) a->split_vertical = !a->split_vertical;
    if (IsKeyPressed(KEY_X)) vs_ui_swap_split(a);
    if (IsKeyPressed(KEY_T)) {
        a->theater = !a->theater;
        vs_toast(a, a->theater ? "theater mode on" : "theater mode off");
        return;
    }
    if (IsKeyPressed(KEY_U)) {
        vs_chat_sync_all(&a->chat);
        return;
    }
    if (IsKeyPressed(KEY_C)) {
        a->editing = VS_EDIT_CHAT;
        a->edit_buf[0] = 0;
        a->edit_len = 0;
        vs_toast(a, "paste a chat .json or .tsv path, then Enter");
    }
    if (IsKeyPressed(KEY_SLASH) || IsKeyPressed(KEY_H)) a->show_help = !a->show_help;
    if (IsKeyPressed(KEY_ESCAPE) && g_browser.open) vs_browse_close(&g_browser);
    if (IsKeyPressed(KEY_ESCAPE) && g_who.open) who_close();
    if (!ctrl && IsKeyPressed(KEY_F)) {
        int slot = vs_nth_active_video(a, 0) < 0 ? 0 : a->focus;
        for (int i = 0; i < VS_MAX_SOURCES; i++)
            if (!a->src[i].active) { slot = i; break; }
        vs_browse_open(&g_browser, slot);
        a->editing = VS_EDIT_CHANNEL;
        a->edit_buf[0] = 0;
        a->edit_len = 0;
    }
}

// The whole reset: the app state, the saved channel list, and the UI state
// that lives up here and would otherwise survive as a search over an empty
// log or a popup about someone who is no longer loaded.
//
// It is one function rather than a sequence at the click site so that the
// --reset hook drives the path a click drives. It was a sequence at the
// click site, and the test then proved less than it looked like it did.
void vs_ui_reset(vs_app *a) {
    if (!vs_app_reset(a)) return;   // declined: leave everything else alone
    // The emotes belong to the chat that referred to them, so they go with
    // it. The textures are dropped first and the table second: the table is
    // what the image layer reads ids out of.
    vs_emote_img_shutdown();
    vs_emote_reset();
    vs_channels_clear(&g_channels);
    find_clear();
    who_close();
    vs_browse_close(&g_browser);
    Clay_ResetMeasureTextCache();
    vs_toast(a, "reset: VODs, chat, marks and saved channels cleared");
}

int vs_ui_channels_n(void) { return g_channels.n; }

static void handle_mouse(vs_app *a) {
    Vector2 m = GetMousePosition();
    bool down = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    bool press = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);

    // --- active drags -----------------------------------------------------
    if (a->drag >= 0) {
        if (!down) {
            a->drag = -1;
        } else if (a->drag == 0) {                         // split divider
            bool f;
            Clay_BoundingBox st = box_of(CLAY_ID("Stage"), &f);
            if (f && st.width > 20 && st.height > 20) {
                float r = a->split_vertical ? (m.y - st.y) / st.height
                                            : (m.x - st.x) / st.width;
                if (r < 0.15f) r = 0.15f;
                if (r > 0.85f) r = 0.85f;
                a->split_ratio = r;
            }
            return;
        } else if (a->drag == 50) {                        // chat width
            // Measured from the window's right edge to the pointer, rather
            // than accumulated from a start offset, so the column cannot
            // drift away from the cursor over a long drag. Stored unscaled;
            // chat_col_width still caps it against the window.
            bool f;
            Clay_BoundingBox b = box_of(CLAY_ID("Body"), &f);
            if (f && b.width > 40) {
                float px = (b.x + b.width) - m.x;
                float lg = px / (g_scale > 0.01f ? g_scale : 1.0f);
                if (lg < 120) lg = 120;
                if (lg > 900) lg = 900;
                a->chat_w = (int)(lg + 0.5f);
            }
            return;
        } else if (a->drag == 1) {                         // unified scrub
            bool f;
            Clay_BoundingBox b = box_of(CLAY_ID("UniBar"), &f);
            if (f && b.width > 4 && a->have_window) {
                float t = (m.x - b.x) / b.width;
                if (t < 0) t = 0;
                if (t > 1) t = 1;
                vs_seek_wall(a, a->wall_lo + t * (a->wall_hi - a->wall_lo));
            }
            return;
        } else if (a->drag >= 10 && a->drag < 10 + VS_MAX_SOURCES) {   // per-source
            int i = a->drag - 10;
            bool f;
            Clay_BoundingBox b = box_of(CLAY_IDI("SrcBar", i), &f);
            vs_source *s = &a->src[i];
            if (f && b.width > 4 && s->state == VS_LOAD_READY && s->duration > 0) {
                float t = (m.x - b.x) / b.width;
                if (t < 0) t = 0;
                if (t > 1) t = 1;
                // Move the shared clock to where this source's position maps.
                vs_seek_wall(a, vs_wall_for_pos(s, t * s->duration));
            }
            return;
        } else if (a->drag == 40) {                        // move the PiP
            bool f1, f2;
            Clay_BoundingBox st = box_of(CLAY_ID("Stage"), &f1);
            Clay_BoundingBox pb = box_of(CLAY_ID("PipStrip"), &f2);
            if (f1 && f2) {
                float availw = st.width  - pb.width;
                float availh = st.height - pb.height;
                if (availw > 1) {
                    float t = (m.x - st.x - a->drag_dx) / availw;
                    a->pip_x = t < 0 ? 0 : (t > 1 ? 1 : t);
                }
                if (availh > 1) {
                    float t = (m.y - st.y - a->drag_dy) / availh;
                    a->pip_y = t < 0 ? 0 : (t > 1 ? 1 : t);
                }
            }
            return;
        } else if (a->drag == 41) {                        // resize the PiP
            pip_resize_apply(a, m);
            return;
        } else if (a->drag >= 20 && a->drag < 20 + VS_MAX_SOURCES) {   // volume
            int i = a->drag - 20;
            bool f;
            Clay_BoundingBox b = box_of(CLAY_IDI("Vol", i), &f);
            if (f && b.width > 4) {
                float t = (m.x - b.x) / b.width;
                if (t < 0) t = 0;
                if (t > 1) t = 1;
                a->src[i].volume = t;
                vs_apply_audio(a);
            }
            return;
        }
    }

    if (!press) return;

    // --- channel browser (modal, so handle it before anything else) -------
    if (g_browser.open) {
        if (clicked(CLAY_ID("BrowserClose"))) { vs_browse_close(&g_browser); return; }
        if (clicked(CLAY_ID("BrowserField"))) {
            a->editing = VS_EDIT_CHANNEL;
            a->edit_buf[0] = 0;
            a->edit_len = 0;
            return;
        }
        if (clicked(CLAY_ID("BrowserFind"))) {
            if (a->edit_len) commit_edit(a);
            return;
        }
        if (clicked(CLAY_ID("BrowserSave"))) {
            // What is in the field counts, not only what has already been
            // looked up: Save before Find used to do nothing at all, and
            // say nothing either, which reads as the list being broken.
            const char *typed = (a->editing == VS_EDIT_CHANNEL && a->edit_len)
                                ? a->edit_buf
                                : (g_browser.query[0] ? g_browser.query : NULL);
            char name[VS_CHAN_MAX];
            if (!typed || !vs_channel_normalize(typed, name, sizeof name)) {
                vs_toast(a, "type a channel name first");
            } else if (vs_channels_add(&g_channels, name)) {
                vs_toast(a, "saved channel %s", name);
            } else {
                vs_toast(a, "%s is already saved (or the list is full)", name);
            }
            return;
        }
        for (int i = 0; i < g_channels.n; i++)
            if (clicked(CLAY_IDI("BChip", i))) {
                vs_browse_begin(&g_browser, g_channels.names[i]);
                return;
            }
        int slot = g_browser.slot;
        // Watching something from a channel is the signal that it is worth
        // keeping - pressing Save as well was a step nobody should have to
        // remember, and forgetting it looked like the list was broken.
        if (clicked(CLAY_ID("LiveTw")) || clicked(CLAY_ID("LiveKk")) ||
            browser_row_clicked()) {
            char remember[VS_CHAN_MAX];
            if (g_browser.query[0] &&
                vs_channel_normalize(g_browser.query, remember, sizeof remember))
                vs_channels_add(&g_channels, remember);
        }
        if (clicked(CLAY_ID("LiveTw"))) {
            char u[VS_URL_MAX];
            snprintf(u, sizeof u, "https://www.twitch.tv/%s", g_browser.query);
            vs_add_source_at(a, u, VS_ROLE_VIDEO, slot);
            vs_browse_close(&g_browser);
            return;
        }
        if (clicked(CLAY_ID("LiveKk"))) {
            char u[VS_URL_MAX];
            snprintf(u, sizeof u, "https://kick.com/%s", g_browser.query);
            vs_add_source_at(a, u, VS_ROLE_VIDEO, slot);
            vs_browse_close(&g_browser);
            return;
        }
        for (int i = 0; i < g_browser.ntw; i++)
            if (clicked(CLAY_IDI("VodTw", i))) {
                if (g_browser.chat_target) {
                    chat_add_from_browser(a, &g_browser.tw[i]);
                    return;
                }
                vs_add_source_at(a, g_browser.tw[i].url, VS_ROLE_VIDEO, slot);
                vs_browse_close(&g_browser);
                return;
            }
        for (int i = 0; i < g_browser.nkk; i++)
            if (clicked(CLAY_IDI("VodKk", i))) {
                if (g_browser.chat_target) {
                    chat_add_from_browser(a, &g_browser.kk[i]);
                    return;
                }
                vs_add_source_at(a, g_browser.kk[i].url, VS_ROLE_VIDEO, slot);
                vs_browse_close(&g_browser);
                return;
            }
        // Clicking the backdrop dismisses it.
        if (!over(CLAY_ID("Browser"))) { vs_browse_close(&g_browser); return; }
        return;
    }

    // --- drag starts ------------------------------------------------------
    if (a->layout == VS_LAYOUT_PIP) {
        if (over(CLAY_ID("PipGrip"))) {
            pip_resize_latch(a, m);
            a->drag = 41;
            return;
        }
        if (over(CLAY_ID("PipDrag"))) {
            bool f;
            Clay_BoundingBox pb = box_of(CLAY_ID("PipStrip"), &f);
            a->drag_dx = f ? (m.x - pb.x) : 0;
            a->drag_dy = f ? (m.y - pb.y) : 0;
            a->drag = 40;
            return;
        }
    }
    if (a->layout == VS_LAYOUT_SPLIT && over(CLAY_ID("Divider"))) { a->drag = 0; return; }
    if (over(CLAY_ID("ChatGrip"))) { a->drag = 50; return; }
    if (over(CLAY_ID("UniBar"))) { a->drag = 1; }
    for (int i = 0; i < VS_MAX_SOURCES; i++) {
        if (over(CLAY_IDI("SrcBar", i))) a->drag = 10 + i;
        if (over(CLAY_IDI("Vol", i)))    a->drag = 20 + i;
    }
    if (a->drag >= 0) return;

    // --- toolbar ----------------------------------------------------------
    if (clicked(CLAY_ID("PlayBtn")))   { vs_toggle_play(a); return; }
    if (clicked(CLAY_ID("LiveBtn")))   { vs_go_live(a); return; }
    if (clicked(CLAY_ID("LayoutBtn"))) { a->layout_menu = !a->layout_menu; return; }
    if (clicked(CLAY_ID("OrientBtn"))) { a->split_vertical = !a->split_vertical; return; }
    if (clicked(CLAY_ID("SwapBtn")))   { vs_ui_swap_split(a); return; }
    if (clicked(CLAY_ID("BmAdd")))     { vs_bookmark_add(a); return; }
    if (clicked(CLAY_ID("BmPrev")))    { vs_bookmark_jump(a, -1); return; }
    if (clicked(CLAY_ID("BmNext")))    { vs_bookmark_jump(a, +1); return; }
    if (clicked(CLAY_ID("HelpBtn")))   { a->show_help = !a->show_help; return; }
    if (clicked(CLAY_ID("TextDec"))) {
        vs_ui_set_text_scale(a, a->text_scale - 0.1f);
        vs_toast(a, "text %d%%", (int)(a->text_scale * 100 + 0.5f));
        return;
    }
    if (clicked(CLAY_ID("TextInc"))) {
        vs_ui_set_text_scale(a, a->text_scale + 0.1f);
        vs_toast(a, "text %d%%", (int)(a->text_scale * 100 + 0.5f));
        return;
    }
    if (clicked(CLAY_ID("TheaterBtn"))) {
        a->theater = !a->theater;
        vs_toast(a, a->theater ? "theater mode on - move the mouse to the edges"
                               : "theater mode off");
        return;
    }
    if (clicked(CLAY_ID("FullBtn"))) {
        ToggleBorderlessWindowed();
        a->fullscreen = !a->fullscreen;
        return;
    }
    if (clicked(CLAY_ID("SaveBtn"))) {
        char p[512]; vs_session_default_path(p, sizeof p);
        vs_toast(a, vs_session_save(a, p) ? "session saved" : "save failed");
        return;
    }
    if (clicked(CLAY_ID("LoadBtn"))) {
        char p[512]; vs_session_default_path(p, sizeof p);
        if (!vs_session_load(a, p)) vs_toast(a, "no saved session");
        return;
    }
    if (clicked(CLAY_ID("ResetBtn"))) {
        if (g_reset_armed_until > 0 && a->now < g_reset_armed_until) {
            g_reset_armed_until = 0;
            vs_ui_reset(a);           // toasts for itself, and may decline
        } else {
            g_reset_armed_until = a->now + VS_RESET_ARM_S;
            vs_toast(a, "clears every VOD, the chat, your marks and your "
                        "saved channels - click Reset again to confirm");
        }
        return;
    }
    if (a->layout_menu) {
        for (int l = 0; l < VS_LAYOUT_COUNT; l++)
            if (clicked(CLAY_IDI("LayoutOpt", l))) {
                a->layout = (vs_layout)l;
                a->layout_menu = false;
                return;
            }
        a->layout_menu = false;
    }
    for (int i = 0; i < VS_MAX_SOURCES; i++)
        if (clicked(CLAY_IDI("ChatSrc", i))) {
            vs_chat_bind_source(a, i);
            return;
        }
    if (clicked(CLAY_ID("ChatSync"))) {
        vs_chat_sync_all(&a->chat);
        return;
    }
    if (clicked(CLAY_ID("ChatAuto"))) {
        a->chat.auto_sync = !a->chat.auto_sync;
        a->chat.next_sync = 0;                 // act immediately
        vs_toast(a, a->chat.auto_sync ? "chat auto-update on"
                                      : "chat auto-update off");
        return;
    }
    if (clicked(CLAY_ID("WhoClose"))) { who_close(); return; }
    // A name in either list. The popup is the same either way; which list
    // you clicked in only changes how the row maps back to a message.
    for (int k = 0; k < g_row_n; k++) {
        if (!clicked(CLAY_IDI("ChatUser", k))) continue;
        who_open(&a->chat, a->chat.msgs[g_row_msg[k]].user);
        return;
    }
    for (int k = 0; k < g_find.nhits; k++) {
        if (!clicked(CLAY_IDI("ChatHitUser", k))) continue;
        who_open(&a->chat, a->chat.msgs[g_find.hits[k]].user);
        return;
    }
    if (clicked(CLAY_ID("ChatFind"))) {
        a->editing = VS_EDIT_CHATFIND;
        snprintf(a->edit_buf, sizeof a->edit_buf, "%s", g_find.query);
        a->edit_len = (int)strlen(a->edit_buf);
        return;
    }
    for (int i = 0; i < a->chat.nsources; i++)
        if (clicked(CLAY_IDI("ChatTileX", i))) {
            vs_toast(a, "removed %s", a->chat.sources[i].channel);
            vs_chat_source_remove(&a->chat, i);
            return;
        }
    if (clicked(CLAY_ID("ChatTileAdd"))) {
        vs_ui_browse_chat(a, NULL);
        vs_toast(a, "pick a VOD to add its chat");
        return;
    }
    if (clicked(CLAY_ID("ChatOtherVod"))) {
        vs_ui_browse_chat(a, NULL);
        vs_toast(a, "pick the Twitch VOD whose chat you want");
        return;
    }
    // The browser covers a channel's recent VODs; a URL still reaches one
    // that is older than the listing, or not listed at all.
    if (clicked(CLAY_ID("BrowserPasteUrl"))) {
        vs_browse_close(&g_browser);
        a->editing = VS_EDIT_CHATVOD;
        a->edit_buf[0] = 0;
        a->edit_len = 0;
        vs_toast(a, "paste a twitch.tv VOD url, then Enter");
        return;
    }
    if (clicked(CLAY_ID("ChatFindClear"))) {
        find_clear();
        if (a->editing == VS_EDIT_CHATFIND) {
            a->editing = -1;
            a->edit_buf[0] = 0;
            a->edit_len = 0;
        }
        return;
    }
    for (int k = 0; k < g_find.nhits; k++) {
        if (!clicked(CLAY_IDI("ChatHit", k))) continue;
        // A hit is a moment, not a line of text: go there. Every pane
        // follows, because the wall clock is what they all share.
        double w = vs_chat_msg_wall(&a->chat, g_find.hits[k]);
        if (w > 0) {
            vs_seek_wall(a, w);
            char ts[24];
            vs_fmt_dur(a->chat.msgs[g_find.hits[k]].t, ts, sizeof ts);
            vs_toast(a, "jumped to %s", ts);
        } else {
            vs_toast(a, "that message has no anchored time yet");
        }
        return;
    }
    // Both set SHOW as well: otherwise collapsing a panel that was only
    // there on AUTO grounds could drop it altogether on the next frame.
    if (clicked(CLAY_ID("ChatCollapse"))) {
        a->chat_collapsed = true;  a->chat_vis = VS_CHAT_SHOW; return;
    }
    if (clicked(CLAY_ID("ChatExpand"))) {
        a->chat_collapsed = false; a->chat_vis = VS_CHAT_SHOW; return;
    }
    if (clicked(CLAY_ID("ChatNudgeL"))) {
        a->chat.offset -= 0.5; vs_chat_reseek(&a->chat, a->wall); return;
    }
    if (clicked(CLAY_ID("ChatNudgeR"))) {
        a->chat.offset += 0.5; vs_chat_reseek(&a->chat, a->wall); return;
    }

    // --- panes ------------------------------------------------------------
    for (int i = 0; i < VS_MAX_SOURCES; i++) {
        if (clicked(CLAY_IDI("ChanField", i))) {
            a->editing = VS_EDIT_CHANNEL;
            g_browser.slot = i;
            a->edit_buf[0] = 0;
            a->edit_len = 0;
            return;
        }
        if (clicked(CLAY_IDI("FindBtn", i))) {
            g_browser.slot = i;
            if (a->editing == VS_EDIT_CHANNEL && a->edit_len) commit_edit(a);
            else {
                vs_browse_open(&g_browser, i);
                a->editing = VS_EDIT_CHANNEL;
                a->edit_buf[0] = 0;
                a->edit_len = 0;
            }
            return;
        }
        if (clicked(CLAY_IDI("UrlToggle", i))) {
            a->editing = (a->editing == i) ? -1 : i;
            a->edit_buf[0] = 0;
            a->edit_len = 0;
            return;
        }
        for (int k = 0; k < g_channels.n; k++) {
            if (clicked(CLAY_IDI("ChipDel", i * VS_MAX_SAVED + k))) {
                vs_channels_remove(&g_channels, k);
                return;
            }
            if (clicked(CLAY_IDI("Chip", i * VS_MAX_SAVED + k))) {
                g_browser.slot = i;
                vs_browse_open(&g_browser, i);
                vs_browse_begin(&g_browser, g_channels.names[k]);
                return;
            }
        }
        if (clicked(CLAY_IDI("PaneChatSync", i))) {
            vs_chat_sync_all(&a->chat);
            return;
        }
        if (clicked(CLAY_IDI("PaneChatAuto", i))) {
            a->chat.auto_sync = !a->chat.auto_sync;
            a->chat.next_sync = 0;
            return;
        }
        if (clicked(CLAY_IDI("UrlField", i))) {
            a->editing = i;
            a->edit_buf[0] = 0;
            a->edit_len = 0;
            return;
        }
        if (clicked(CLAY_IDI("AddBtn", i))) {
            if (a->editing == i) commit_edit(a);
            else { a->editing = i; a->edit_buf[0] = 0; a->edit_len = 0; }
            return;
        }
        if (!a->src[i].active) continue;
        if (clicked(CLAY_IDI("Solo", i))) {
            a->src[i].solo = !a->src[i].solo; vs_apply_audio(a); return;
        }
        if (clicked(CLAY_IDI("NudgeL", i)))   { vs_nudge(a, i, -1.0 / 30.0); return; }
        if (clicked(CLAY_IDI("NudgeR", i)))   { vs_nudge(a, i,  1.0 / 30.0); return; }
        if (clicked(CLAY_IDI("MkMaster", i))) { vs_set_master(a, i); return; }
        if (clicked(CLAY_IDI("PaneFill", i))) {
            a->src[i].fill = !a->src[i].fill;
            vs_toast(a, "%s: %s", a->src[i].channel[0] ? a->src[i].channel : "pane",
                     a->src[i].fill ? "crop to fill" : "fit, with bars");
            return;
        }
        if (clicked(CLAY_IDI("Close", i)))    { vs_remove_source(a, i); return; }
        if (over(CLAY_IDI("Pane", i))) {
            a->focus = i;
            bool dbl = (a->last_click_pane == i) &&
                       (a->now - a->last_click_t) < 0.35;
            a->last_click_t = a->now;
            a->last_click_pane = i;
            if (dbl) {
                vs_ui_toggle_solo(a, i);
                a->last_click_pane = -1;   // a third click is not another toggle
            }
        }
    }
}

static void handle_drop(vs_app *a) {
    if (!IsFileDropped()) return;
    FilePathList files = LoadDroppedFiles();
    for (unsigned i = 0; i < files.count; i++) {
        const char *p = files.paths[i];
        const char *ext = strrchr(p, '.');
        if (ext && (vs_stricmp(ext, ".json") == 0 || vs_stricmp(ext, ".tsv") == 0)) {
            if (a->chat.pdt_start <= 0)
                for (int k = 0; k < VS_MAX_SOURCES; k++)
                    if (a->src[k].active && a->src[k].platform == VS_PLAT_TWITCH &&
                        a->src[k].state == VS_LOAD_READY) {
                        vs_chat_set_anchor(&a->chat, a->src[k].pdt_start);
                        break;
                    }
            if (vs_chat_load(&a->chat, p)) {
                vs_recompute_window(a);
                vs_chat_reseek(&a->chat, a->wall);
                vs_toast(a, "chat: %s", a->chat.status);
            } else vs_toast(a, "chat failed: %s", a->chat.status);
        } else if (ext && vs_stricmp(ext, ".session") == 0) {
            vs_session_load(a, p);
        }
    }
    UnloadDroppedFiles(files);
}

// ------------------------------------------------------------------ frame

Clay_RenderCommandArray vs_ui_frame(vs_app *a, float dt) {
    g_app = a;
    g_stroff = 0;
    // The emote draw arena is per frame: Clay holds pointers into it only
    // until this frame is rendered.
    g_em_draw_n = 0;
    g_em_hover = -1;

    // Clay errors used to go only to stderr, which nobody sees when the app
    // is launched from Explorer.
    {
        extern int g_clay_error_count;
        static int seen = 0;
        if (g_clay_error_count > seen) {
            seen = g_clay_error_count;
            vs_toast(a, "layout warning (%d) - see the console", seen);
        }
    }

    // A session load can change the scale behind our back.
    if (fabsf(a->text_scale * g_dpi - g_scale) > 0.001f)
        apply_scale(a);

    handle_drop(a);
    if (vs_browse_poll(&g_browser) && g_browser.state == VS_LOAD_FAILED)
        vs_toast(a, "%s", g_browser.status);
    if (a->editing >= 0) handle_text_entry(a);
    else                 handle_keys(a);
    handle_mouse(a);

    bool want_chat = chat_visible(a);

    // Remember the channel of anything that is actually playing. Which
    // button was pressed does not matter - a pasted URL counts too - and
    // this is the one place that sees every way a source can arrive.
    for (int i = 0; i < VS_MAX_SOURCES; i++) {
        static char seen[VS_MAX_SOURCES][VS_CHAN_MAX];
        vs_source *s2 = &a->src[i];
        if (!s2->active || s2->state != VS_LOAD_READY || !s2->channel[0]) continue;
        if (strcmp(seen[i], s2->channel) == 0) continue;
        snprintf(seen[i], VS_CHAN_MAX, "%s", s2->channel);
        char name[VS_CHAN_MAX];
        if (vs_channel_normalize(s2->channel, name, sizeof name))
            vs_channels_add(&g_channels, name);
    }

    theater_update(a);

    Clay_BeginLayout();
    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                    .childGap = 0, .layoutDirection = CLAY_TOP_TO_BOTTOM },
        .backgroundColor = VS_BG,
    }) {
        // Outside theater mode the chrome is part of the column, and the
        // stage gets what is left. Inside it, the stage always has the whole
        // window and the chrome floats on top: revealing it used to re-lay
        // out everything below it, so the control the pointer was heading
        // for moved out from under it - and near the band's edge that
        // shifted the pointer back out of the band, flickering.
        if (!a->theater) toolbar(a);
        CLAY(CLAY_ID("Body"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                        .padding = CLAY_PADDING_ALL(VS_PAD), .childGap = VS_GAP },
        }) {
            switch (a->layout) {
                case VS_LAYOUT_GRID: stage_grid(a); break;
                case VS_LAYOUT_PIP:  stage_pip(a);  break;
                case VS_LAYOUT_SOLO: stage_solo(a); break;
                case VS_LAYOUT_TRI:  stage_tri(a);  break;
                default:             stage_split(a); break;
            }
            if (want_chat) chat_panel(a);
        }
        if (!a->theater) {
            timelines(a);
            status_line(a);
        }
        if (a->theater && g_th_top) {
            CLAY(CLAY_ID("TheaterTop"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED((float)GetScreenWidth()),
                                        CLAY_SIZING_FIT(0) },
                            .layoutDirection = CLAY_TOP_TO_BOTTOM },
                .backgroundColor = VS_SCRIM,
                .floating = { .zIndex = 12,
                              .attachPoints = { CLAY_ATTACH_POINT_LEFT_TOP,
                                                CLAY_ATTACH_POINT_LEFT_TOP },
                              .attachTo = CLAY_ATTACH_TO_ROOT },
            }) {
                toolbar(a);
            }
        }
        if (a->theater && g_th_bottom) {
            CLAY(CLAY_ID("TheaterBottom"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED((float)GetScreenWidth()),
                                        CLAY_SIZING_FIT(0) },
                            .layoutDirection = CLAY_TOP_TO_BOTTOM },
                .backgroundColor = VS_SCRIM,
                .floating = { .zIndex = 12,
                              .attachPoints = { CLAY_ATTACH_POINT_LEFT_BOTTOM,
                                                CLAY_ATTACH_POINT_LEFT_BOTTOM },
                              .attachTo = CLAY_ATTACH_TO_ROOT },
            }) {
                timelines(a);
                status_line(a);
            }
        }
        if (g_browser.open) browser_overlay(a);
        if (a->show_help) help_overlay();
        if (g_who.open && a->chat.state == VS_LOAD_READY) who_overlay(a);
        else if (g_who.open) who_close();
        if (a->editing == VS_EDIT_CHATVOD) {
            CLAY(CLAY_ID("ChatVodPrompt"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(overlay_w(560)),
                                        CLAY_SIZING_FIT(0) },
                            .padding = CLAY_PADDING_ALL(12), .childGap = 8,
                            .layoutDirection = CLAY_TOP_TO_BOTTOM },
                .backgroundColor = VS_PANEL_HI,
                .cornerRadius = CLAY_CORNER_RADIUS(8),
                .floating = { .zIndex = 25,
                              .attachPoints = { CLAY_ATTACH_POINT_CENTER_CENTER,
                                                CLAY_ATTACH_POINT_CENTER_CENTER },
                              .attachTo = CLAY_ATTACH_TO_ROOT },
                .border = { .color = VS_ACCENT, .width = CLAY_BORDER_OUTSIDE(1) },
            }) {
                label_text(CLAY_STRING("Chat from a Twitch VOD you are not watching"),
                           VS_TEXT, 14);
                text_field(a, CLAY_IDI("UrlField", VS_EDIT_CHATVOD),
                           VS_EDIT_CHATVOD,
                           CLAY_STRING("https://www.twitch.tv/videos/..."));
                label_text(CLAY_STRING("Takes a slot, but decodes no video. "
                                       "Enter loads, Esc cancels."),
                           VS_TEXT_DIM, 12);
            }
        }
        if (a->editing == VS_EDIT_CHAT) {
            CLAY(CLAY_ID("ChatPrompt"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(overlay_w(560)),
                                        CLAY_SIZING_FIT(0) },
                            .padding = CLAY_PADDING_ALL(12), .childGap = 8,
                            .layoutDirection = CLAY_TOP_TO_BOTTOM },
                .backgroundColor = VS_PANEL_HI,
                .cornerRadius = CLAY_CORNER_RADIUS(8),
                .floating = { .zIndex = 25,
                              .attachPoints = { CLAY_ATTACH_POINT_CENTER_CENTER,
                                                CLAY_ATTACH_POINT_CENTER_CENTER },
                              .attachTo = CLAY_ATTACH_TO_ROOT },
                .border = { .color = VS_ACCENT, .width = CLAY_BORDER_OUTSIDE(1) },
            }) {
                label_text(CLAY_STRING("Chat log path (.json from TwitchDownloader, or .tsv)"),
                           VS_TEXT, 14);
                text_field(a, CLAY_IDI("UrlField", VS_EDIT_CHAT), VS_EDIT_CHAT,
                           CLAY_STRING("path to a chat .json or .tsv"));
                label_text(CLAY_STRING("Ctrl+V pastes. Enter loads, Esc cancels."),
                           VS_TEXT_DIM, 12);
            }
        }
    }
    Clay_RenderCommandArray cmds = Clay_EndLayout(dt);

    // Record each pane's measured video area so mpv renders at the right size.
    for (int i = 0; i < VS_MAX_SOURCES; i++) {
        bool f;
        Clay_BoundingBox vb = box_of(CLAY_IDI("Video", i), &f);
        if (f && vb.width > 16 && vb.height > 16) {
            g_pw[i] = (int)vb.width;
            g_ph[i] = (int)vb.height;
        }
    }
    return cmds;
}
