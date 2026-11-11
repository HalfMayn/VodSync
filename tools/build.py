#!/usr/bin/env python3
"""vodsync build driver.

Captures the MSVC environment from vcvars64.bat once, then invokes cl.exe
directly. Avoids the cmd.exe/MSYS argument mangling you get from a shell.

  python tools/build.py test   -> build\\vs_test_align.exe (headless checks)
  python tools/build.py app    -> build\\vodsync.exe
  python tools/build.py all
  python tools/build.py installer -> build\\installer\\vodsync-<ver>-setup.exe

VODSYNC_BUILD_DIR builds somewhere else entirely:

  set VODSYNC_BUILD_DIR=build-dev

which is how to compile and test while the real build folder's exe is
open and being used. It is not only that the exe would be locked: a
separate folder is also a separate *state* directory, so a test run
cannot touch the saved channels or the session of the build someone is
actually watching.
"""
import os, subprocess, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = ROOT / (os.environ.get("VODSYNC_BUILD_DIR") or "build")
VS = pathlib.Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community")
VCVARS = VS / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
RAYLIB = ROOT / "third_party" / "raylib" / "raylib-6.2_win64_msvc16"
MPV = ROOT / "third_party" / "mpv"
ICO = ROOT / "src" / "vodsync.ico"
RES = BUILD / "vodsync.res"
ISS = ROOT / "tools" / "installer" / "vodsync.iss"
YTDLP = ROOT / "third_party" / "yt-dlp" / "yt-dlp.exe"
VERSION_H = ROOT / "src" / "vs_version.h"

_env_cache = None


def msvc_env():
    """Run vcvars64.bat and capture the environment it sets."""
    global _env_cache
    if _env_cache:
        return _env_cache
    if not VCVARS.exists():
        sys.exit(f"vcvars64.bat not found at {VCVARS}")
    # 'set' after the call dumps the full environment.
    out = subprocess.run(
        f'"{VCVARS}" >nul 2>&1 && set',
        shell=True, capture_output=True, text=True, errors="replace")
    if out.returncode != 0:
        sys.exit("vcvars64.bat failed:\n" + out.stderr[-2000:])
    env = {}
    for line in out.stdout.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            env[k] = v
    # Normalise the keys we rely on, whatever case `set` reported them in.
    lookup = {k.upper(): k for k in env}
    for want in ("PATH", "INCLUDE", "LIB"):
        if want in lookup and lookup[want] != want:
            env[want] = env.pop(lookup[want])
    if "INCLUDE" not in env:
        sys.exit("vcvars did not set INCLUDE; is the C++ workload installed?")
    _env_cache = env
    return env


def find_tool(name, env):
    """Resolve an exe against the captured PATH (CreateProcess won't)."""
    for d in env.get("PATH", "").split(os.pathsep):
        if not d:
            continue
        p = pathlib.Path(d) / name
        if p.exists():
            return str(p)
    sys.exit(f"{name} not found on the MSVC PATH")


def compile_target(name, sources, includes, libs, extra=None, subsystem=None):
    BUILD.mkdir(exist_ok=True)
    env = msvc_env()
    cmd = [find_tool("cl.exe", env), "/nologo", "/std:c17", "/W3", "/O2", "/MD",
           "/D_CRT_SECURE_NO_WARNINGS", "/Zc:preprocessor",
           "/wd4244", "/wd4305", "/wd4267",
           f"/Fo{BUILD}\\", f"/Fe{BUILD}\\{name}.exe"]
    cmd += [str(s) for s in sources]
    cmd += [f"/I{i}" for i in includes]
    if extra:
        cmd += extra
    cmd += ["/link"]
    if subsystem:
        cmd += [f"/SUBSYSTEM:{subsystem}"]
    cmd += [str(l) for l in libs]

    print(f"--- building {name} ---")
    r = subprocess.run(cmd, env=env, cwd=ROOT, capture_output=True,
                       text=True, errors="replace")
    sys.stdout.write(r.stdout)
    if r.stderr.strip():
        sys.stderr.write(r.stderr)
    if r.returncode != 0:
        print(f"*** {name} FAILED (exit {r.returncode})")
        return False
    print(f"*** {name} ok -> {BUILD.name}\\{name}.exe")
    return True


def build_resource():
    """Compile src/vodsync.rc so Explorer shows the mark on vodsync.exe.

    SetWindowIcons only covers the window and taskbar at runtime; the file
    icon has to be a resource linked into the binary. Returns the .res path,
    or None when there is no .ico yet (see the "icon" target).
    """
    if not ICO.exists():
        return None
    env = msvc_env()
    rc = find_tool("rc.exe", env)
    BUILD.mkdir(exist_ok=True)
    r = subprocess.run([rc, "/nologo", f"/I{ROOT / 'src'}", "/fo", str(RES),
                        str(ROOT / "src" / "vodsync.rc")],
                       env=env, cwd=ROOT, capture_output=True,
                       text=True, errors="replace")
    if r.returncode != 0:
        print("*** rc.exe failed")
        print(r.stdout + r.stderr)
        return None
    return RES


def make_icon():
    """Generate src/vodsync.ico from the app's own drawing code.

    Two passes, because the mark lives in the binary: build, have the exe
    export PNGs (it does this before opening a window, so nothing flashes up),
    pack them into an .ico, then rebuild with the resource attached.
    """
    import tempfile
    if not build_app():
        return False
    with tempfile.TemporaryDirectory() as td:
        exe = BUILD / "vodsync.exe"
        r = subprocess.run([str(exe), "--export-logo-set", td],
                           capture_output=True, text=True, errors="replace")
        if r.returncode != 0:
            print("*** exporting icon sizes failed")
            print(r.stdout + r.stderr)
            return False
        pngs = sorted(pathlib.Path(td).glob("icon_*.png"))
        if not pngs:
            print("*** no icon PNGs were exported")
            return False
        r = subprocess.run([sys.executable, "-I",
                            str(ROOT / "tools" / "make_icon.py"), str(ICO)]
                           + [str(x) for x in pngs],
                           capture_output=True, text=True, errors="replace")
        sys.stdout.write(r.stdout)
        if r.returncode != 0:
            sys.stderr.write(r.stderr)
            return False
    return build_app()


def build_test():
    return compile_target(
        "vs_test_align",
        [ROOT / "src" / "vs_test_align.c", ROOT / "src" / "vs_resolve.c",
         ROOT / "src" / "vs_platform_win32.c"],
        [ROOT / "src", ROOT / "third_party" / "clay"],
        ["winhttp.lib"])


def build_live_probe():
    """Live-stream probe: needs libmpv, but no window and no raylib."""
    return compile_target(
        "vs_test_live",
        [ROOT / "src" / "vs_test_live.c", ROOT / "src" / "vs_resolve.c",
         ROOT / "src" / "vs_platform_win32.c"],
        [ROOT / "src", MPV / "include"],
        [MPV / "libmpv.dll.a", "winhttp.lib"])


def crt_runtime_dll():
    """The one CRT DLL the exe imports, to sit beside it.

    /MD and the prebuilt raylib go together: third_party's raylib.lib carries
    /DEFAULTLIB:MSVCRT, so switching this build to /MT is not a one-flag
    change at all - it is a RuntimeLibrary mismatch at link time, and there
    are no raylib sources here to rebuild. The supported alternative is
    app-local deployment: ship vcruntime140.dll next to the exe. That is
    117 KB, needs no redistributable installer and no admin rights, which
    also keeps a per-user install free of a UAC prompt. The UCRT itself
    (the api-ms-win-crt-* forwarders) is part of Windows 10 and 11.
    """
    base = VS / "VC" / "Redist" / "MSVC"
    found = sorted(base.glob("*/x64/Microsoft.VC*.CRT/vcruntime140.dll"))
    return found[-1] if found else None


def build_app():
    src = ROOT / "src"
    res = build_resource()
    ok = compile_target(
        "vodsync",
        [src / "main.c", src / "vs_app.c", src / "vs_resolve.c",
         src / "vs_player.c", src / "vs_chat.c", src / "vs_emote.c",
         src / "vs_emote_img.c", src / "vs_emote_sets.c",
         src / "vs_session.c", src / "vs_download.c",
         src / "vs_ui.c", src / "vs_browse.c", src / "vs_logo.c",
         src / "vs_platform_win32.c",
         src / "clay_renderer_raylib.c"],
        [src, ROOT / "third_party" / "clay", RAYLIB / "include",
         MPV / "include"],
        [RAYLIB / "lib" / "raylib.lib", MPV / "libmpv.dll.a",
         "winhttp.lib", "opengl32.lib", "gdi32.lib", "winmm.lib",
         "shell32.lib", "user32.lib"] + ([str(res)] if res else []))
    if ok:
        # Everything the exe needs beside it at runtime. Not all DLLs: the
        # standalone yt-dlp belongs here too, because vs_ytdlp_cmd looks in
        # the exe's own folder first and that is what frees both this build
        # and an installed copy from needing python to play anything.
        import shutil
        beside = [MPV / "libmpv-2.dll"]
        crt = crt_runtime_dll()
        if crt:
            beside.append(crt)
        else:
            print("    WARNING: no vcruntime140.dll in the VS redist folder;")
            print("             an installed copy would need the VC++ "
                  "redistributable")
        if YTDLP.exists():
            beside.append(YTDLP)
        else:
            print(f"    WARNING: no {YTDLP.name} at {YTDLP.parent};")
            print("             playback will fall back to \"python -m "
                  "yt_dlp\" and need python")
        for f in beside:
            if f.exists():
                shutil.copy2(f, BUILD / f.name)
                print(f"    copied {f.name}")
    return ok


def app_version():
    """The version, read from the one place that defines it.

    The .rc compiles the same header into the exe's VERSIONINFO, so passing
    this to iscc keeps the installer, the binary and the header from ever
    disagreeing - which is the failure you only notice after shipping.
    """
    import re
    m = re.search(r'#define\s+VS_VER_STR\s+"([^"]+)"',
                  VERSION_H.read_text(encoding="utf-8"))
    return m.group(1) if m else None


def find_iscc():
    """Inno Setup's compiler. Not part of the MSVC environment, so this looks
    where its installer puts it and then at PATH.

    All three locations are real. "winget install JRSoftware.InnoSetup" put
    it under %LOCALAPPDATA%\\Programs, per-user, which is also why it needed
    no UAC prompt - and why checking only the two Program Files paths
    reported a freshly installed Inno Setup as missing.
    """
    import shutil
    local = os.environ.get("LOCALAPPDATA")
    cands = []
    if local:
        cands.append(pathlib.Path(local) / "Programs" / "Inno Setup 6" / "ISCC.exe")
    for var, default in (("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                         ("ProgramFiles", r"C:\Program Files")):
        cands.append(pathlib.Path(os.environ.get(var, default))
                     / "Inno Setup 6" / "ISCC.exe")
    for c in cands:
        if c.exists():
            return str(c)
    found = shutil.which("ISCC.exe")
    if found:
        return found
    print("*** ISCC.exe not found. Install Inno Setup 6.3 or newer:")
    print("      winget install JRSoftware.InnoSetup")
    return None


def build_installer():
    """Pack what is already in the build folder. Builds nothing itself: the
    .iss errors out rather than packing a stale or partial folder."""
    exe = BUILD / "vodsync.exe"
    if not exe.exists():
        print(f"*** {exe} not there; run \"python tools/build.py app\" first")
        return False
    ver = app_version()
    if not ver:
        print(f"*** no VS_VER_STR in {VERSION_H}")
        return False
    iscc = find_iscc()
    if not iscc:
        return False

    print(f"--- building installer for {ver} ---")
    r = subprocess.run([iscc, f"/DAppVer={ver}", str(ISS)],
                       cwd=ROOT, capture_output=True, text=True,
                       errors="replace")
    if r.returncode != 0:
        sys.stdout.write(r.stdout[-4000:])
        sys.stderr.write(r.stderr[-2000:])
        print(f"*** installer FAILED (exit {r.returncode})")
        return False
    out = BUILD / "installer" / f"vodsync-{ver}-setup.exe"
    if not out.exists():
        print("*** iscc reported success but produced no file")
        return False
    print(f"*** installer ok -> {out}  ({out.stat().st_size / 1e6:.1f} MB)")
    return True


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "all"
    results = []
    if what in ("test", "all"):
        results.append(build_test())
    if what == "live":
        results.append(build_live_probe())
    if what == "icon":
        results.append(make_icon())
    if what == "installer":
        results.append(build_installer())
    if what in ("app", "all"):
        results.append(build_app())
    sys.exit(0 if all(results) else 1)
