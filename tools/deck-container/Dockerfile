# vodsync - build environment matching what the Steam Deck actually runs.
#
# Base: Valve's Steam Runtime 3 "sniper" SDK. This is the container Steam
# games run inside on the Deck, so a binary built here runs on the Deck and
# on every other distro Steam supports - its glibc is older than any modern
# desktop's, which is the direction that matters.
#
# The alternative is Dockerfile.arch, which matches SteamOS's own desktop
# session (SteamOS 3 is Arch). Use that one to reproduce a bug that involves
# the system's own mpv or Mesa; use this one to build something to ship.
#
#   docker build -t vodsync-deck tools/deck-container
#   docker run --rm -v "$PWD":/src -w /src vodsync-deck \
#       python3 tools/build_linux.py all
FROM registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest

# raylib is not in the runtime, so it is built from source and installed into
# the image. Everything else - mpv, curl, GL, VAAPI - the runtime has.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git pkg-config python3 \
        libmpv-dev libcurl4-openssl-dev \
        libgl1-mesa-dev libegl1-mesa-dev libva-dev \
        libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
        libwayland-dev wayland-protocols libxkbcommon-dev \
        fonts-dejavu-core \
    && rm -rf /var/lib/apt/lists/*

# 6.2 is what the Windows build and the Arch route were verified against.
ARG RAYLIB_TAG=6.2
RUN git clone --depth 1 --branch ${RAYLIB_TAG} \
        https://github.com/raysan5/raylib.git /tmp/raylib \
 && cmake -S /tmp/raylib -B /tmp/raylib/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=ON \
        -DBUILD_EXAMPLES=OFF \
 && cmake --build /tmp/raylib/build -j"$(nproc)" \
 && cmake --install /tmp/raylib/build \
 && ldconfig \
 && rm -rf /tmp/raylib

# yt-dlp as a standalone binary: the Deck has no system Python the app can
# rely on, and this is what gets shipped beside the executable.
ADD https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux /usr/local/bin/yt-dlp
RUN chmod +x /usr/local/bin/yt-dlp

WORKDIR /src
CMD ["python3", "tools/build_linux.py", "all"]
