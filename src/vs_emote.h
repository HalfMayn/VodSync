// vodsync - the emote table.
//
// One entry per distinct emote, whatever found it. Messages refer to entries
// by index, so the id and name strings exist once rather than once per
// occurrence - and the index is also what the renderer wants, since it has
// to reach the texture.
//
// Two ways in, because the four sources genuinely differ:
//
//   Twitch and Kick emotes are addressed by the platform's OWN ID, which the
//   chat log carries per message (Twitch in the export's fragments, Kick
//   in-band as [emote:id:name], both normalised into spans by
//   tools/vs_chat_fmt.py). Those are only ever drawn where the log says
//   there is one, which is what stops a word that happens to match an emote
//   name from turning into a picture.
//
//   7TV and BTTV are overlays: the platform knows nothing about them, and
//   their emotes appear in chat as ordinary words. So they are matched by
//   NAME against a set fetched at run time, exactly as those extensions do
//   it. Resolving them at export time would freeze a log to the set as it
//   was on the day it was exported.
//
// Name matching is therefore restricted to 7TV and BTTV entries. A Twitch
// emote's name is still recorded, for the hover label, but it is not a
// lookup key: "KEKW" meaning a Kick emote in one message must not turn every
// later "KEKW" into that same image when the log did not say so.
#ifndef VS_EMOTE_H
#define VS_EMOTE_H
#include <stdbool.h>

typedef enum {
    VS_EM_TWITCH = 0,
    VS_EM_KICK,
    VS_EM_7TV,
    VS_EM_BTTV,
    VS_EM_SRC_COUNT
} vs_emote_src;

// "emotesv2_" plus 32 hex is 41 characters, which is the longest seen.
#define VS_EMOTE_ID_MAX   48
#define VS_EMOTE_NAME_MAX 40

typedef enum {
    VS_EM_UNKNOWN = 0,   // in the table; no image asked for yet
    VS_EM_FETCHING,
    VS_EM_READY,
    VS_EM_FAILED
} vs_emote_state;

// A run of a message's text that is an emote.
//
// Held in one array per chat log rather than inline in the message struct: a
// message with no emotes then costs nothing, and one with fifty is not
// truncated to whatever fixed array would have been chosen. 6 bytes each.
typedef struct {
    unsigned short start;   // byte offset into the message's text
    unsigned short len;     // bytes, so the hover can show the word itself
    unsigned short emote;   // index into the emote table
} vs_emote_span;

// The source an emote came from, and its platform id. Together these are its
// identity: the two platforms number theirs independently, so neither alone
// is unique.
vs_emote_src vs_emote_source(int i);
const char  *vs_emote_id(int i);
const char  *vs_emote_name(int i);
int          vs_emote_count(void);

// Add an emote, or return the index of the one already there. `name` may be
// NULL when it is not known yet; a later call carrying one fills it in.
// Returns -1 only if the arguments are unusable.
int vs_emote_intern(vs_emote_src src, const char *id, const char *name);

// The emote a bare word in chat refers to, or -1. 7TV and BTTV only, for the
// reason in the header comment. `len` is in bytes; pass 0 for a C string.
int vs_emote_find_name(const char *name, int len);

// Is it animated? Decides whether its image is a GIF or a PNG, which the
// two overlay CDNs encode in the file name rather than content-negotiating.
bool vs_emote_animated(int i);
void vs_emote_set_animated(int i, bool animated);

// ---- reading an emote set ----
//
// These take the response body rather than fetching it, so the parsing - the
// part with the bugs in it - is testable with no network at all. The caller
// does the HTTP.
//
// 7TV: an emote-set response, either /v3/emote-sets/global or the
// `emote_set` of /v3/users/{platform}/{id}. Emotes are interned under their
// ALIAS IN THAT SET (the outer "name"), which is what chat actually types
// and can differ from the emote's original name.
int vs_emote_add_7tv(const char *json, int len);

// BetterTTV: /3/cached/emotes/global (a bare array) or
// /3/cached/users/twitch/{id} (channelEmotes + sharedEmotes). The name is
// the "code" field.
int vs_emote_add_bttv(const char *json, int len);

// Where to fetch an emote's image. `scale` is 1..4 and is clamped to what
// the source offers. This is the only place that knows the CDN layouts.
bool vs_emote_url(int i, int scale, char *out, int cap);

// Forget everything. For --reset and for tests.
void vs_emote_reset(void);

#endif
