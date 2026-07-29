#!/usr/bin/env python3
"""
make_extractor_preset.py - synthesise EXtractoR animation presets (.ffx)

An EXtractoR preset stores its channel routing as four null-terminated ASCII
strings inside the effect's arbitrary-data blob, at fixed offsets 164 bytes
apart. Nothing else in the file depends on those strings - no length prefix, no
checksum - so a new preset can be made by copying a template and overwriting the
four names.

    slot     offset
    red      0x30F
    green    0x3B3
    blue     0x457
    alpha    0x4FB

Use "(copy)" as the alpha name to reproduce EXtractoR's Copy Alpha setting,
which is what three-channel AOVs use.

UNVERIFIED: this is undocumented plug-in internals. Test one preset in After
Effects before trusting a batch.
"""

import argparse
import os
import shutil
import sys

SLOTS = {"red": 0x30F, "green": 0x3B3, "blue": 0x457, "alpha": 0x4FB}
TEMPLATE_SIZE = 5186
STRIDE = 0xA4          # distance between slots
SAFE_NAME_BYTES = 64   # generous, still far short of the next slot


def read_names(buf):
    out = {}
    for slot, off in SLOTS.items():
        chunk = buf[off:off + SAFE_NAME_BYTES]
        z = chunk.find(b"\0")
        out[slot] = chunk[:z if z >= 0 else len(chunk)].decode("latin-1")
    return out


def patch(buf, names):
    b = bytearray(buf)
    for slot, off in SLOTS.items():
        new = names[slot].encode("latin-1")
        if len(new) + 1 > SAFE_NAME_BYTES:
            raise ValueError(f"{slot} name is too long: {names[slot]!r}")
        # clear the old string's span, then write the new one
        old = b[off:off + SAFE_NAME_BYTES]
        z = old.find(b"\0")
        span = (z if z >= 0 else SAFE_NAME_BYTES)
        b[off:off + span] = b"\0" * span
        b[off:off + len(new)] = new
        b[off + len(new)] = 0
    return bytes(b)


def channels_for(aov, style):
    """Work out the four channel names for an AOV from a layout convention."""
    if style == "rgba":
        return {"red": f"{aov}.R", "green": f"{aov}.G", "blue": f"{aov}.B",
                "alpha": f"{aov}.A"}
    if style == "rgb":
        return {"red": f"{aov}.R", "green": f"{aov}.G", "blue": f"{aov}.B",
                "alpha": "(copy)"}
    if style == "xyz":
        return {"red": f"{aov}.X", "green": f"{aov}.Y", "blue": f"{aov}.Z",
                "alpha": "A"}
    if style == "single":
        return {"red": aov, "green": aov, "blue": aov, "alpha": aov}
    raise ValueError(style)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--template", required=True, help="an existing EXtractoR .ffx")
    ap.add_argument("--out-dir", default="presets")
    ap.add_argument("--prefix", default="extractAOV_")
    ap.add_argument("--layer-prefix", default="",
                    help="EXR part prefix, e.g. 'subimage03.' for multi-part files")
    ap.add_argument("--style", default="rgb",
                    choices=["rgba", "rgb", "xyz", "single"])
    ap.add_argument("--inspect", action="store_true",
                    help="just print the template's channel names and exit")
    ap.add_argument("aovs", nargs="*", help="AOV names")
    args = ap.parse_args()

    buf = open(args.template, "rb").read()
    if len(buf) != TEMPLATE_SIZE:
        print(f"warning: template is {len(buf)} bytes, expected {TEMPLATE_SIZE}. "
              "The Cryptomatte preset has a different structure and cannot be "
              "used as a template.", file=sys.stderr)

    if args.inspect:
        for slot, name in read_names(buf).items():
            print(f"{slot:6} {name}")
        return

    if not args.aovs:
        ap.error("give at least one AOV name, or use --inspect")

    os.makedirs(args.out_dir, exist_ok=True)
    for aov in args.aovs:
        names = channels_for(args.layer_prefix + aov, args.style)
        data = patch(buf, names)
        path = os.path.join(args.out_dir, f"{args.prefix}{aov}.ffx")
        with open(path, "wb") as f:
            f.write(data)
        check = read_names(data)
        ok = all(check[s] == names[s] for s in SLOTS)
        print(f"{'ok ' if ok else 'BAD'} {path}   "
              f"R={check['red']} G={check['green']} B={check['blue']} A={check['alpha']}")


if __name__ == "__main__":
    main()
