#!/usr/bin/env python3
"""Convert .3mf files into the "--- <entry>" text container read by Load3mfFuzzer.

Usage: 3mf_to_corpus.py OUT_DIR FILE.3mf [FILE.3mf ...]
Binary entries (thumbnails, embedded meshes in other formats) are dropped: they do not reach
the parsers under test and only slow mutation down.
"""
import pathlib
import sys
import zipfile


def convert(src: pathlib.Path) -> bytes:
    out = bytearray()
    with zipfile.ZipFile(src) as z:
        for info in z.infolist():
            if info.is_dir() or info.filename.lower().endswith((".png", ".jpg", ".jpeg", ".qoi", ".stl", ".step", ".stp")):
                continue
            body = z.read(info)
            out += b"--- " + info.filename.encode() + b"\n" + body
            if not body.endswith(b"\n"):
                out += b"\n"
    return bytes(out)


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    out_dir = pathlib.Path(sys.argv[1])
    out_dir.mkdir(parents=True, exist_ok=True)
    for arg in sys.argv[2:]:
        src = pathlib.Path(arg)
        (out_dir / (src.stem + ".txt")).write_bytes(convert(src))
    return 0


if __name__ == "__main__":
    sys.exit(main())
