// vodsync - fetching the overlay emote sets (7TV and BetterTTV).
//
// Separate from vs_emote.c, which parses these responses but deliberately
// touches no network, and from vs_emote_img.c, which is about images. This
// file is only the errand: get the four documents, hand them to the parsers.
//
// Downloading happens on a worker; PARSING happens on the UI thread, because
// parsing interns into the emote table and the table has exactly one writer.
// That is the same split the image layer uses and for the same reason.
#ifndef VS_EMOTE_SETS_H
#define VS_EMOTE_SETS_H

// Ask for a channel's overlay emotes. `platform` is "twitch" or "kick" and
// `user_id` is that platform's numeric id for the channel, which the chat
// log's #source line carries. Safe to call every time a log loads: the work
// happens once per channel, and the global sets once overall.
//
// Does nothing without a user id. A log written before the format carried
// one simply shows no overlay emotes, rather than guessing.
void vs_emote_sets_request(const char *platform, const char *user_id);

// Parse whatever has arrived and intern it. UI thread, once a frame.
void vs_emote_sets_pump(void);

// The channel's avatar URL, found in the set documents on the way past, or
// "" if none is known. For a Kick channel this is the linked Twitch
// account's picture when there is one: Kick serves only WebP, which nothing
// here can decode, so a channel with no Twitch counterpart has no avatar
// and the caller draws a letter instead.
const char *vs_emote_sets_avatar(const char *platform, const char *user_id);

// Documents fetched, still in flight, and failed - for the status line and
// for tests. `emotes` is how many the sets have contributed to the table.
void vs_emote_sets_status(int *fetched, int *pending, int *failed,
                          int *emotes);

#endif
