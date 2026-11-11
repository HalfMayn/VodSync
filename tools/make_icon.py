#!/usr/bin/env python3
"""Pack PNGs into a Windows .ico.

    python tools/make_icon.py <out.ico> <16.png> <32.png> ...

Entries are stored as PNG, which Windows has accepted inside .ico since Vista
and which keeps the 256px entry small. No third-party imaging library: the
container is a 6-byte header plus one 16-byte record per image.

This exists because SetWindowIcons only sets the *window* icon at runtime.
The icon Explorer shows on the .exe is a resource compiled into the binary,
which is what src/vodsync.rc and the build's rc.exe step are for.
"""
import pathlib
import struct
import sys


def png_size(data):
    # PNG signature, then an IHDR chunk whose first two fields are w and h.
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    w, h = struct.unpack(">II", data[16:24])
    return w, h


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    out = pathlib.Path(sys.argv[1])
    srcs = [pathlib.Path(p) for p in sys.argv[2:]]

    images = []
    for p in srcs:
        if not p.exists():
            print(f"missing: {p}", file=sys.stderr)
            return 1
        data = p.read_bytes()
        w, h = png_size(data)
        images.append((w, h, data))
    images.sort(key=lambda i: i[0])

    n = len(images)
    header = struct.pack("<HHH", 0, 1, n)          # reserved, type=icon, count
    offset = 6 + 16 * n
    entries, blobs = b"", b""
    for w, h, data in images:
        entries += struct.pack(
            "<BBBBHHII",
            0 if w >= 256 else w,                  # 0 means 256
            0 if h >= 256 else h,
            0,                                     # palette size: 0 for truecolour
            0,                                     # reserved
            1,                                     # colour planes
            32,                                    # bits per pixel
            len(data),
            offset)
        blobs += data
        offset += len(data)

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(header + entries + blobs)
    print(f"wrote {out}  ({n} sizes: "
          f"{', '.join(str(i[0]) for i in images)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
