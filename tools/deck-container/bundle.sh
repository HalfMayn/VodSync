#!/bin/sh
# Assemble a self-contained vodsync folder to copy to the Steam Deck.
#
# Run this inside the sniper SDK, after tools/build_linux.py has produced a
# binary there. It works out which shared libraries have to travel with the
# app and which the Deck already has, by asking Valve's own manifest for the
# runtime that ships to users rather than guessing.
#
# The answer matters: libmpv is NOT in the shipped runtime (checked - the
# Platform image's manifest has no libmpv at all), so it and the parts of
# its dependency chain the runtime lacks come along. Drivers must NOT:
# libGL, libva and friends have to be the host's, or hardware decode breaks.
#
# By default the bundle is self-contained: it carries everything except the
# drivers and libc, so the folder runs on SteamOS as it is, launched however
# you like. Set VODSYNC_RUNTIME_PROVIDED=1 to instead leave out whatever
# Valve's runtime already ships - a much smaller folder, but then it only
# runs when Steam launches it inside the sniper runtime, which a non-Steam
# shortcut does not do by default.
#
#   ./bundle.sh [outdir] [binary]
set -e

OUT=${1:-/src/build-linux/dist}
BIN=${2:-/src/build-linux/vodsync-sniper}
MANIFEST_URL="https://repo.steampowered.com/steamrt-images-sniper/snapshots/latest-container-runtime-depot/com.valvesoftware.SteamRuntime.Platform-amd64%2Ci386-sniper-sysroot.mtree.gz"

[ -f "$BIN" ] || { echo "no binary at $BIN"; exit 1; }
mkdir -p "$OUT/lib"
cp -f "$BIN" "$OUT/vodsync"

# The SDK is multiarch: /usr/lib/i386-linux-gnu sits beside
# /usr/lib/x86_64-linux-gnu. A bare `find -name libfoo.so.0 -print -quit`
# therefore returns whichever it reaches first, and once returned the i386
# copy of libdecor-0.so.0 - which the loader skips without explaining
# itself. SDL3 then failed to open with "Failed loading SDL3 library", and
# ldd said "not found" about a file sitting right there in lib/.
#
# So resolve a soname through the 64-bit directories first, and confirm the
# ELF machine type rather than trusting the path it came from.
is_elf64() {
    [ -f "$1" ] || return 1
    # e_machine is 2 bytes at offset 18; 0x3e little-endian is x86-64.
    m=$(od -An -tx1 -j18 -N2 "$1" 2>/dev/null | tr -d ' \n')
    [ "$m" = "3e00" ]
}

find_lib64() {
    for d in /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu \
             /usr/lib64 /lib64 /usr/local/lib; do
        if is_elf64 "$d/$1"; then echo "$d/$1"; return 0; fi
    done
    for c in $(find /usr/lib /lib /usr/local/lib -name "$1" 2>/dev/null); do
        if is_elf64 "$c"; then echo "$c"; return 0; fi
    done
    return 1
}

PROVIDED=$(mktemp)
: > "$PROVIDED"
if [ -n "$VODSYNC_RUNTIME_PROVIDED" ]; then
    echo "fetching the runtime's library manifest ..."
    if curl -sf --max-time 120 "$MANIFEST_URL" | zcat 2>/dev/null \
            | grep -oE '[^/ ]+\.so[^ ]*' | sed 's/\\[0-9]*//g' | sort -u > "$PROVIDED"
    then
        echo "  runtime provides $(wc -l < "$PROVIDED") libraries; leaving those out"
    else
        echo "  manifest unavailable; bundling everything instead"
        : > "$PROVIDED"
    fi
else
    echo "self-contained bundle (set VODSYNC_RUNTIME_PROVIDED=1 for the slim one)"
fi

# Never bundle these even if the manifest is missing them: they bind to the
# kernel, the GPU driver or the display server on the machine it runs on.
# libnsl is deliberately NOT here: glibc split it out years ago, so it is a
# normal shared library the target may simply not have.
# Anchored at both ends against the versionless soname. Unanchored, "libm"
# matches libmpv and "libc" matches libcurl - which is exactly what happened
# the first time: the one library that has to ship was the one left out.
never='^(libGL|libGLX|libGLdispatch|libEGL|libOpenGL|libglapi|libva|libva-drm|libva-x11|libvdpau|libdrm|libgbm|libwayland-[a-z]+|libX[A-Za-z0-9]*|libxcb[A-Za-z0-9-]*|libxkbcommon[a-z-]*|libasound|libpulse[a-z-]*|libpipewire-[0-9.]+|libsystemd|libudev|libdbus-1|libc|libm|libdl|libpthread|librt|libresolv|libgcc_s|libstdc\+\+|ld-linux[a-z0-9-]*)\.so$'

copy_closure() {
    ldd "$1" 2>/dev/null | awk '{print $3}' | grep -E '^/' | while read -r so; do
        base=$(basename "$so")
        soname=$(echo "$base" | sed 's/\.so.*/\.so/')
        echo "$soname" | grep -qE "$never" && continue   # driver or libc
        grep -qx "$base" "$PROVIDED" && continue
        [ -e "$OUT/lib/$base" ] && continue
        cp -L "$so" "$OUT/lib/$base"
        echo "  + $base"
        copy_closure "$so"
    done
}

echo "bundling libraries the Deck does not have ..."
copy_closure "$OUT/vodsync"

# Libraries that nothing declares a dependency on, because they are opened
# with dlopen at runtime. Neither ldd nor the NEEDED scan below can see
# these, by construction: the libSDL2-2.0.so.0 copied above is sdl2-compat,
# a shim that opens libSDL3.so.0 itself. Leave it out and the app aborts at
# startup with "Failed loading SDL3 library" on any machine that does not
# happen to have SDL3 installed system-wide - which is exactly why testing
# the bundle on Arch did not catch it. Added before the fixed-point loop so
# that its own dependencies are picked up with everything else.
for extra in libSDL3.so.0; do
    [ -e "$OUT/lib/$extra" ] && continue
    src=$(find_lib64 "$extra")
    if [ -n "$src" ]; then
        cp -L "$src" "$OUT/lib/$extra"
        echo "  + $extra (dlopen'd, not in any NEEDED record)"
    else
        echo "  !! $extra not found; the app will abort at startup without it"
    fi
done

# Second pass, to a fixed point, reading each library's own NEEDED list
# rather than asking ldd what is missing. Here inside the SDK everything
# resolves - libmpv's SMB support finds samba's private libraries through an
# RPATH of its own - so "what is missing" is the wrong question: the answer
# is only visible on the machine the folder is copied to. What travels has
# to be decided from the dependency records, not from what happens to load.
round=0
while [ $round -lt 8 ]; do
    round=$((round + 1))
    added=""
    for f in "$OUT/vodsync" "$OUT"/lib/*.so*; do
        [ -e "$f" ] || continue
        objdump -p "$f" 2>/dev/null | awk '/NEEDED/ {print $2}' | while read -r soname; do
            [ -e "$OUT/lib/$soname" ] && continue
            echo "$soname" | sed 's/\.so.*/\.so/' | grep -qE "$never" && continue
            grep -qx "$soname" "$PROVIDED" && continue
            src=$(find_lib64 "$soname")
            [ -z "$src" ] && continue
            cp -L "$src" "$OUT/lib/$soname"
            echo "  + $soname (pass $round)"
        done
    done
    # the loop above runs in a subshell, so count the result instead
    now=$(ls "$OUT/lib" | wc -l)
    [ "$now" = "${prev:-0}" ] && break
    prev=$now
done

rm -f "$PROVIDED"

# The helpers the app shells out to. yt-dlp because the Deck has no system
# python the app can count on; TwitchDownloaderCLI for chat. Both are
# resolved from the folder at runtime - the launcher puts it on PATH.
for helper in /usr/local/bin/yt-dlp /src/third_party/twitchdownloader/TwitchDownloaderCLI; do
    [ -f "$helper" ] && cp -f "$helper" "$OUT/" && echo "  + $(basename "$helper")"
done
if [ ! -f "$OUT/yt-dlp" ]; then
    echo "fetching yt-dlp (the standalone build, no python needed) ..."
    if curl -sfL --max-time 300 -o "$OUT/yt-dlp" \
        https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux
    then
        chmod +x "$OUT/yt-dlp"
        echo "  + yt-dlp"
    else
        echo "  !! could not fetch yt-dlp; the app cannot resolve VODs without it"
    fi
fi
# A font travels too. The app looks beside its own executable first, so
# this is what makes the folder readable on a machine whose fonts live
# somewhere the search does not reach, or that has none installed - which
# is how a Fedora laptop ended up on raylib's built-in bitmap font. DejaVu
# is freely redistributable; its license file comes along as required.
for f in /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
         /usr/share/fonts/TTF/DejaVuSans.ttf; do
    if [ -f "$f" ]; then
        cp -f "$f" "$OUT/DejaVuSans.ttf"
        echo "  + DejaVuSans.ttf"
        break
    fi
done
for l in /usr/share/doc/fonts-dejavu-core/copyright \
         /usr/share/licenses/ttf-dejavu/LICENSE; do
    if [ -f "$l" ]; then
        cp -f "$l" "$OUT/DejaVuSans.LICENSE.txt"
        break
    fi
done
[ -f "$OUT/DejaVuSans.ttf" ] || echo "  !! no font bundled; the app will use whatever the host has"

mkdir -p "$OUT/tools"
cp -f /src/tools/vs_chat_sync.py /src/tools/vs_chat_prep.py \
      /src/tools/vs_chat_fmt.py "$OUT/tools/" 2>/dev/null || true

# Check what we are about to ship. A wrong-architecture library does not
# announce itself: the app aborts at startup on the target machine, with a
# message about something else entirely. Cheaper to fail here.
bad=0
for f in "$OUT/vodsync" "$OUT"/lib/*.so*; do
    [ -e "$f" ] || continue
    if ! is_elf64 "$f"; then
        echo "  !! $(basename "$f") is not x86-64"
        bad=$((bad + 1))
    fi
done
if [ "$bad" != "0" ]; then
    echo "refusing to bundle: $bad file(s) of the wrong architecture"
    exit 1
fi
echo "architecture check: all $(ls "$OUT/lib" | wc -l) libraries are x86-64"

cat > "$OUT/vodsync.sh" <<'EOF'
#!/bin/sh
# Launcher: finds the bundled libraries and helpers without installing
# anything. LD_LIBRARY_PATH as well as the binary's RPATH, because a
# dependency of a dependency is not always covered by the latter.
here=$(cd "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="$here:$PATH"
exec "$here/vodsync" "$@"
EOF
chmod +x "$OUT/vodsync.sh"

echo
echo "bundle at $OUT  ($(du -sh "$OUT" | cut -f1))"
echo "copy it to the Deck, add vodsync.sh to Steam as a non-Steam game."
