// vodsync - the version, in one place.
//
// src/vodsync.rc turns this into the VERSIONINFO resource that Explorer's
// Details tab and an installer both read.
//
// The three numbers are authoritative. VS_VER_STR has to repeat them because
// vodsync.rc uses it directly as a VERSIONINFO string value, where the RC
// compiler cannot be trusted to concatenate string literals - so bump all
// four. tools/build.py computes the version from the numbers and refuses to
// build if VS_VER_STR disagrees, which is how bumping only some of them
// stops being a thing you discover after shipping.
#ifndef VS_VERSION_H
#define VS_VERSION_H

#define VS_VER_MAJOR 0
#define VS_VER_MINOR 11
#define VS_VER_PATCH 0
#define VS_VER_STR   "0.11.0"

#endif
