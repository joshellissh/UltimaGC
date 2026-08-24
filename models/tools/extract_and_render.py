#!/usr/bin/env python3
"""
Full extraction of a .mx archive: unpacks every tar member, decompresses the
zlib blobs, and renders every raw RGBA8888 payload to PNG (including every
frame of the multi-frame 3d_total*.m turntable sequences) so the assets can
be browsed/viewed directly.

Dimensions come from car.config (2D assets) and the known 768x480 "3D" render
canvas (confirmed by car.config's 3dw/3dh and by decompressed-size / 4).
"""
import os
import sys
import struct
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from mx_tool import list_members  # noqa: E402

from PIL import Image

FRAME_W, FRAME_H = 768, 480
FRAME_BYTES = FRAME_W * FRAME_H * 4

DIMS_2D = {
    "2d_car.z": (170, 314),
    "2d_door0.z": (170, 314),
    "2d_door1.z": (170, 314),
    "2d_door2.z": (170, 314),
    "2d_door3.z": (170, 314),
    "2d_zoom0.z": (400, 216),
    "2d_zoom1.z": (400, 216),
}


def save_rgba(raw, w, h, outpath):
    img = Image.frombytes("RGBA", (w, h), raw, "raw", "RGBA")
    img.save(outpath)


def split_frames(blob):
    """Yield each decompressed frame from a concatenated multi-zlib-stream blob."""
    off = 0
    n = len(blob)
    while off < n:
        dc = zlib.decompressobj()
        try:
            out = dc.decompress(blob[off:])
        except zlib.error:
            break
        if len(out) != FRAME_BYTES:
            break
        consumed = len(blob[off:]) - len(dc.unused_data)
        yield out
        off += consumed


def main(mx_path, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    data, members = list_members(mx_path)

    for name, off, size, mtime, mode in members:
        raw = data[off:off + size]

        if name == "car.config":
            with open(os.path.join(out_dir, name), "wb") as f:
                f.write(raw)
            print(f"{name:30s} -> copied as-is")
            continue

        if name in DIMS_2D:
            w, h = DIMS_2D[name]
            dec = zlib.decompress(raw)
            outpath = os.path.join(out_dir, name.replace(".z", ".png"))
            save_rgba(dec, w, h, outpath)
            print(f"{name:30s} -> {os.path.basename(outpath)} ({w}x{h})")
            continue

        if name.endswith(".m"):
            # multi-frame turntable sequence
            subdir = os.path.join(out_dir, name.replace(".m", ""))
            os.makedirs(subdir, exist_ok=True)
            count = 0
            for frame in split_frames(raw):
                outpath = os.path.join(subdir, f"frame_{count:03d}.png")
                save_rgba(frame, FRAME_W, FRAME_H, outpath)
                count += 1
            print(f"{name:30s} -> {count} frames in {os.path.basename(subdir)}/")
            continue

        if name.endswith(".z"):
            # single 768x480 frame (door/led/lun states)
            dec = zlib.decompress(raw)
            if len(dec) == FRAME_BYTES:
                outpath = os.path.join(out_dir, name.replace(".z", ".png"))
                save_rgba(dec, FRAME_W, FRAME_H, outpath)
                print(f"{name:30s} -> {os.path.basename(outpath)} (768x480)")
            else:
                outpath = os.path.join(out_dir, name.replace(".z", ".bin"))
                with open(outpath, "wb") as f:
                    f.write(dec)
                print(f"{name:30s} -> {os.path.basename(outpath)} (UNKNOWN dims, {len(dec)} bytes raw)")
            continue

        # fallback: dump raw
        with open(os.path.join(out_dir, name), "wb") as f:
            f.write(raw)
        print(f"{name:30s} -> raw dump ({size} bytes)")


if __name__ == "__main__":
    mx_path = sys.argv[1] if len(sys.argv) > 1 else "A360K000.mx"
    out_dir = sys.argv[2] if len(sys.argv) > 2 else "extracted_" + os.path.splitext(os.path.basename(mx_path))[0]
    main(mx_path, out_dir)
