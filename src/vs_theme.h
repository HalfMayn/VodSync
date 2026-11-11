// vodsync - theme: all colors, spacing and font ids live here.
#ifndef VS_THEME_H
#define VS_THEME_H
#include "clay.h"

// Three fonts rasterised at exact pixel sizes. Text is always drawn at the
// size its atlas was built at: scaling a glyph atlas is what turns small UI
// text into mush.
#define VS_FONT_BODY   0    // 16px
#define VS_FONT_SMALL  1    // 13px
#define VS_FONT_MICRO  2    // 11px
#define VS_SIZE_BODY   16
#define VS_SIZE_SMALL  13
#define VS_SIZE_MICRO  11

static const Clay_Color VS_BG        = {  18,  18,  22, 255 };
static const Clay_Color VS_PANEL     = {  30,  31,  38, 255 };
static const Clay_Color VS_PANEL_HI  = {  42,  44,  54, 255 };
static const Clay_Color VS_STAGE     = {   8,   8,  10, 255 };
static const Clay_Color VS_LINE      = {  58,  60,  72, 255 };
static const Clay_Color VS_TEXT      = { 232, 234, 242, 255 };
static const Clay_Color VS_TEXT_DIM  = { 150, 154, 170, 255 };
static const Clay_Color VS_ACCENT    = {  96, 150, 255, 255 };
static const Clay_Color VS_ACCENT_HI = { 132, 176, 255, 255 };
static const Clay_Color VS_TWITCH    = { 145,  70, 255, 255 };
static const Clay_Color VS_KICK      = {  83, 252,  24, 255 };
static const Clay_Color VS_WARN      = { 255, 176,  64, 255 };
static const Clay_Color VS_ERR       = { 255,  94,  94, 255 };
static const Clay_Color VS_OK        = { 110, 220, 140, 255 };
static const Clay_Color VS_CLEAR     = {   0,   0,   0,   0 };
static const Clay_Color VS_SCRIM     = {   0,   0,   0, 140 };

#define VS_PAD      8
#define VS_GAP      6
#define VS_RADIUS   5
#define VS_BAR_H    44
#define VS_CHAT_W   330
// Narrowest the chat column may be squeezed to before the layout would
// rather overflow the window. Deliberately NOT scaled: it is the floor
// that stops a high scale from pushing the column off-screen, so
// scaling it would defeat the point.
#define VS_CHAT_W_MIN 140

// Theater chrome floats over the picture, so it carries its own backing
// rather than letting video show through the text.
#define VS_SCRIM (Clay_Color){ 14, 15, 19, 238 }

#endif
