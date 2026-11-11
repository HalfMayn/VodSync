#!/usr/bin/env python3
"""vodsync build driver for Linux (including the Steam Deck).

Deliberately separate from tools/build.py: that one drives MSVC and builds
the Windows app that is in daily use, and nothing here should be able to
disturb it. The two share every source file except the platform layer -
vs_platform_win32.c there, vs_platform_posix.c here.

  python3 tools/build_linux.py          -> build-linux/vodsync
  python3 tools/build_linux.py test     -> build-linux/vs_test_align
  python3 tools/build_linux.py desktop  -> a .desktop file for Steam / the menu

Needs, from the distro or from third_party/: raylib, libmpv, libcurl.
On SteamOS the root filesystem is read-only and is replaced on update, so
install them in a container or a Flatpak runtime and keep the result in
~/ rather than running `pacman -S` on the Deck itself.
"""
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
BUILD = ROOT / "build-linux"

SOURCES = [
    "main.c", "vs_app.c", "vs_resolve.c", "vs_player.c", "vs_chat.c",
    "vs_emote.c", "vs_emote_img.c", "vs_emote_sets.c",
    "vs_session.c", "vs_download.c", "vs_ui.c", "vs_browse.c", "vs_logo.c",
    "clay_renderer_raylib.c", "vs_platform_posix.c",
]

TEST_SOURCES = ["vs_test_align.c", "vs_resolve.c", "vs_platform_posix.c"]


def pkg_config(args, package):
    """Flags for a package, or None when pkg-config does not know it."""
    if not shutil.which("pkg-config"):
        return None
    r = subprocess.run(["pkg-config", args, package],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return None
    return r.stdout.split()


def deps(packages):
    """cflags + libs for each package, falling back to -l<name>."""
    cflags, libs = [], []
    for pkg, fallback in packages:
        c = pkg_config("--cflags", pkg)
        l = pkg_config("--libs", pkg)
        if c is None or l is None:
            print(f"    pkg-config has no '{pkg}', falling back to {fallback}")
            libs += [fallback]
        else:
            cflags += c
            libs += l
    return cflags, libs


def compiler():
    for name in (os.environ.get("CC"), "clang", "gcc", "cc"):
        if name and shutil.which(name):
            return name
    sys.exit("no C compiler found (set CC, or install clang)")


def compile_target(name, sources, extra_cflags=(), extra_libs=()):
    BUILD.mkdir(exist_ok=True)
    cc = compiler()

    pkgs = [("raylib", "-lraylib"), ("mpv", "-lmpv"), ("libcurl", "-lcurl")]
    cflags, libs = deps(pkgs)

    # A vendored raylib/mpv beats the system one when it is there, which is
    # how a self-contained folder for the Deck gets built.
    for vendor in (ROOT / "third_party" / "raylib-linux",
                   ROOT / "third_party" / "mpv-linux"):
        if (vendor / "include").is_dir():
            cflags += [f"-I{vendor / 'include'}"]
            libs = [f"-L{vendor / 'lib'}"] + libs

    cmd = [cc, "-std=c17", "-O2", "-Wall", "-Wno-unused-result",
           "-D_GNU_SOURCE",
           f"-I{SRC}", f"-I{ROOT / 'third_party' / 'clay'}"]
    cmd += list(cflags) + list(extra_cflags)
    cmd += [str(SRC / s) for s in sources]
    cmd += ["-o", str(BUILD / name)]
    # Order matters for static linking: our objects first, libraries after.
    cmd += list(libs) + list(extra_libs)
    cmd += ["-lGL", "-lm", "-lpthread", "-ldl", "-lrt"]
    # Let the binary find libraries shipped beside it, so the Deck build can
    # be a folder you copy rather than an install.
    #
    # --disable-new-dtags matters: the modern default emits DT_RUNPATH, which
    # applies only to this object's own dependencies. The bundled libmpv then
    # looks for its ffmpeg libraries without it and finds nothing. DT_RPATH is
    # inherited down the chain, which is what a self-contained folder needs.
    cmd += ["-Wl,--disable-new-dtags",
            "-Wl,-rpath,$ORIGIN/lib", "-Wl,-rpath,$ORIGIN"]

    print(f"--- building {name} ---")
    r = subprocess.run(cmd, cwd=ROOT, text=True)
    if r.returncode != 0:
        print(f"*** {name} FAILED (exit {r.returncode})")
        return False
    print(f"*** {name} ok -> build-linux/{name}")
    return True


def build_app():
    return compile_target("vodsync", SOURCES)


def build_test():
    return compile_target("vs_test_align", TEST_SOURCES)


def write_desktop():
    """A launcher, which is also what Steam needs to add it in Game Mode."""
    BUILD.mkdir(exist_ok=True)
    exe = BUILD / "vodsync"
    icon = BUILD / "vodsync.png"
    if not icon.exists() and exe.exists():
        subprocess.run([str(exe), "--export-logo", str(icon)],
                       capture_output=True, text=True)
    path = BUILD / "vodsync.desktop"
    path.write_text(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=vodsync\n"
        "Comment=Watch Twitch and Kick side by side, in sync\n"
        f"Exec={exe}\n"
        f"Icon={icon}\n"
        "Categories=AudioVideo;Player;\n"
        "Terminal=false\n", encoding="utf-8")
    path.chmod(0o755)
    print(f"*** wrote {path}")
    return True


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "app"
    results = []
    if what in ("test", "all"):
        results.append(build_test())
    if what in ("app", "all"):
        results.append(build_app())
    if what in ("desktop", "all"):
        results.append(write_desktop())
    if not results:
        sys.exit(f"unknown target: {what}")
    sys.exit(0 if all(results) else 1)
