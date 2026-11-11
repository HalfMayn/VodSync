# Building vodsync for the Steam Deck

Two images, for two different meanings of "like the Deck":

| | what it is | use it to |
|---|---|---|
| `Dockerfile` | Valve's Steam Runtime 3 **sniper** SDK — the container Steam games run inside on the Deck | build something to **ship**: its glibc is older than any desktop's, so the binary runs on the Deck and elsewhere |
| `Dockerfile.arch` | plain Arch, which is what **SteamOS 3** is | reproduce anything involving the system's own mpv, Mesa or VAAPI |

Neither is needed to *run* the app on the Deck — they are build environments.

## Arch in WSL, the route taken on this machine

SteamOS 3 is Arch, and Arch is an official WSL distribution, so this is the
closest Deck-like environment available here without Docker:

```bash
wsl --install archlinux --no-launch
wsl -d archlinux -u root -e bash -lc "pacman -Syu --noconfirm base-devel clang pkgconf python raylib mpv curl mesa libva"
wsl -d archlinux -u root -e bash -lc "cd /mnt/<drive>/<path>/vodsync && python3 tools/build_linux.py all"
```

`--no-launch` matters: without it the first run stops to ask for a username
and password, which a script cannot answer.

Enabling WSL stages Windows optional features and **needs a reboot before
any distribution can be installed**. Until that reboot happens,
`wsl --install <distro>` reports "Changes will not be effective until the
system is rebooted" and quietly installs nothing.

Arch's libraries are newer than the Deck's, so use this to find bugs rather
than to produce the binary you ship - for that, the sniper SDK above.

## Running the container: podman inside the Arch distro

There is no need for Docker Desktop, and no need for a second WSL distro.
The Arch distro above runs **as root** with systemd and cgroup v2, so podman
works rootful - which skips the `/etc/subuid` and `newuidmap` setup that
rootless podman needs under WSL:

```
pacman -Syu --noconfirm --needed podman fuse-overlayfs
```

Verify before the big pull, because the sniper SDK is a large image:

```
podman run --rm docker.io/library/alpine echo ok
```

Then:

```
podman build -t vodsync-deck tools/deck-container
podman run --rm -v /mnt/<drive>/<path>/vodsync:/src -w /src vodsync-deck python3 tools/build_linux.py all
```

Docker Desktop (`winget install Docker.DockerDesktop`) does the same job if
it happens to be installed - same commands, `docker` for `podman`. It is a
heavier install for no gain here.

### Two builds, one output path - mind the order

`tools/build_linux.py` always writes `build-linux/vodsync`, whichever
environment it runs in. So the Arch build and the sniper build **overwrite
each other**, and nothing warns you: a binary needing GLIBC_2.38 and one
needing 2.17 look identical in `ls`.

`bundle.sh` reads `build-linux/vodsync-sniper` for that reason. Copy the
container's output there as soon as it is built, before any Arch build runs:

```
cp build-linux/vodsync build-linux/vodsync-sniper
```

Then check which one you have, since this is the whole point of building in
sniper:

```
objdump -T build-linux/vodsync-sniper | grep -oE 'GLIBC_2[.][0-9]+' | sort -uV | tail -1
```

`GLIBC_2.17` means it runs on the Deck. Anything higher means an Arch binary
got copied by mistake.

`tools/build_linux.py desktop` also writes a `.desktop` file, which is what
Steam needs to add it as a non-Steam game.

## Shipping it to the Deck

The SteamOS root filesystem is read-only and is replaced wholesale on
update, so nothing should be installed into it. Copy a folder into `~/`
instead:

```
build-linux/vodsync
build-linux/yt-dlp          # standalone binary; the Deck has no system python
build-linux/lib/            # libmpv.so.2 and libraylib.so if built in sniper
```

The binary is linked with `-rpath $ORIGIN/lib`, so it finds those without
`LD_LIBRARY_PATH`.

Chat export additionally needs TwitchDownloaderCLI, which has official
`linux-x64` builds; drop it in the same folder.
