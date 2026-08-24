#!/usr/bin/env python3
"""
Unpack/repack tool for the vendor .mx 3D-model archive format used by this
AVM/360-camera product's dash unit.

Format, reverse-engineered from models/A360K0*.mx:
  - The whole file is a standard GNU tar archive (512-byte headers, GNU
    "ustar  \0" magic, root/root owner).
  - Obfuscation: the first 250 bytes of the FILE (i.e. of the very first
    tar header only -- name/mode/uid/gid/size/mtime/chksum/typeflag/most of
    linkname) are bitwise-inverted (byte ^ 0xFF). Everything from byte 250
    onward -- the rest of that header, and all subsequent headers/content --
    is standard, unobfuscated tar.
  - Every stored member is itself raw data compressed with zlib (.z / .m
    extensions both just mean "zlib blob"; .m files are just bigger).
  - car.config is a plain INI describing raw RGBA8888 canvas sizes for the
    2D assets (2d_car.z, 2d_door*.z, 2d_zoom*.z).

This module can:
  - list/extract a .mx into its raw (still zlib-compressed) tar members
  - decompress/recompress individual members
  - repack a directory of members back into a valid, working .mx

Round-trip note: to inject changed content, build the new archive with
build_tar() below (which reproduces the vendor's exact GNU header layout)
rather than the system `tar` binary -- bsdtar/gtar emit different header
bytes (uname/gname, magic spacing, checksum text form) that a byte-diff
against the original would flag, though the device likely only cares that
the tar is structurally valid, not byte-identical to stock.
"""
import os
import sys
import struct
import zlib

INVERT_BYTES = 250
BLOCK = 512


def deobfuscate(data: bytes) -> bytes:
    out = bytearray(data)
    for i in range(min(INVERT_BYTES, len(out))):
        out[i] ^= 0xFF
    return bytes(out)


def obfuscate(data: bytes) -> bytes:
    return deobfuscate(data)  # XOR 0xFF is its own inverse


def parse_octal(field: bytes) -> int:
    field = field.rstrip(b"\x00 ").strip()
    return int(field, 8) if field else 0


def list_members(mx_path):
    """Return [(name, data_offset, size, mtime, mode)] using the plain (post-deobfuscation) file."""
    data = bytearray(open(mx_path, "rb").read())
    for i in range(INVERT_BYTES):
        data[i] ^= 0xFF
    data = bytes(data)

    members = []
    off = 0
    n = len(data)
    while off + BLOCK <= n:
        hdr = data[off:off + BLOCK]
        if hdr == b"\x00" * BLOCK:
            break
        name = hdr[0:100].rstrip(b"\x00").decode("utf-8", "replace")
        mode = parse_octal(hdr[100:108])
        size = parse_octal(hdr[124:136])
        mtime = parse_octal(hdr[136:148])
        typeflag = hdr[156:157]
        content_off = off + BLOCK
        if typeflag in (b"0", b"\x00") and size > 0:
            members.append((name, content_off, size, mtime, mode))
        padded = ((size + BLOCK - 1) // BLOCK) * BLOCK
        off = content_off + padded
    return data, members


def extract_all(mx_path, out_dir):
    data, members = list_members(mx_path)
    os.makedirs(out_dir, exist_ok=True)
    for name, off, size, mtime, mode in members:
        raw = data[off:off + size]
        outpath = os.path.join(out_dir, name)
        with open(outpath, "wb") as f:
            f.write(raw)
        print(f"{name:30s} {size:>10d} bytes -> {outpath}")
    return members


def build_tar_header(name: str, size: int, mtime: int, mode: int = 0o644, typeflag: bytes = b"0") -> bytes:
    """Build a GNU-tar-style header matching the vendor's exact byte layout."""
    hdr = bytearray(BLOCK)
    name_b = name.encode("utf-8")
    hdr[0:len(name_b)] = name_b
    hdr[100:108] = ("%07o\x00" % mode).encode()
    hdr[108:116] = b"0000000\x00"          # uid 0
    hdr[116:124] = b"0000000\x00"          # gid 0
    hdr[124:136] = ("%011o\x00" % size).encode()
    hdr[136:148] = ("%011o\x00" % mtime).encode()
    hdr[148:156] = b" " * 8                # chksum placeholder during computation
    hdr[156:157] = typeflag
    hdr[257:265] = b"ustar  \x00"          # GNU magic+version
    uname = b"root\x00"
    gname = b"root\x00"
    hdr[265:265 + len(uname)] = uname
    hdr[297:297 + len(gname)] = gname
    chksum = sum(hdr)
    hdr[148:156] = ("%06o\x00 " % chksum).encode()
    return bytes(hdr)


def build_tar(members: list) -> bytes:
    """members: [(name, data_bytes, mtime, mode)]. Returns a full, plain (unobfuscated) tar body."""
    out = bytearray()
    for name, blob, mtime, mode in members:
        out += build_tar_header(name, len(blob), mtime, mode)
        out += blob
        pad = (-len(blob)) % BLOCK
        out += b"\x00" * pad
    out += b"\x00" * (BLOCK * 2)  # end-of-archive marker
    return bytes(out)


def repack(members: list) -> bytes:
    """members: [(name, data_bytes, mtime, mode)] -> final .mx bytes (tar + first-250-byte invert)."""
    tar_bytes = bytearray(build_tar(members))
    for i in range(min(INVERT_BYTES, len(tar_bytes))):
        tar_bytes[i] ^= 0xFF
    return bytes(tar_bytes)


def roundtrip_selftest(mx_path):
    data, members = list_members(mx_path)
    blobs = [(name, data[off:off + size], mtime, mode) for name, off, size, mtime, mode in members]
    rebuilt = repack(blobs)
    original = open(mx_path, "rb").read()
    if rebuilt == original:
        print("ROUNDTRIP: byte-identical to original")
        return True
    else:
        print(f"ROUNDTRIP: differs. original={len(original)} rebuilt={len(rebuilt)}")
        n = min(len(original), len(rebuilt))
        for i in range(n):
            if original[i] != rebuilt[i]:
                print(f"first diff at byte {i}: original={original[i]:02x} rebuilt={rebuilt[i]:02x}")
                print("context original:", original[max(0,i-8):i+24].hex())
                print("context rebuilt :", rebuilt[max(0,i-8):i+24].hex())
                break
        return False


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: mx_tool.py <list|extract|selftest> <path.mx> [outdir]")
        sys.exit(1)
    cmd = sys.argv[1]
    mx_path = sys.argv[2]
    if cmd == "list":
        _, members = list_members(mx_path)
        for name, off, size, mtime, mode in members:
            print(f"{name:30s} {size:>10d}  mtime={mtime} mode={oct(mode)}")
    elif cmd == "extract":
        out_dir = sys.argv[3]
        extract_all(mx_path, out_dir)
    elif cmd == "selftest":
        roundtrip_selftest(mx_path)
    else:
        print("unknown command", cmd)
        sys.exit(1)
