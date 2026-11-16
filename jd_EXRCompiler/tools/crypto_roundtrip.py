"""Cryptomatte round trip: renderer Cryptomatte -> per-object mattes -> jd_exrc crypto -> compare.

Independent of the CLI's C++: own MurmurHash3, own EXR reading/writing (OpenImageIO), NumPy.
Run with Blender's bundled Python (has numpy + OpenImageIO):
    python.exe -I crypto_roundtrip.py REF.exr WORKDIR JD_EXRC.exe

Checks
  1. every manifest entry in REF hashes to the same hex with our MurmurHash3 (+ exponent fix)
  2. rebuilding from mattes in `priority` mode gives, per pixel, the same {id: coverage} as REF (|diff| <= 1e-6)
  3. the rebuilt manifest equals REF's (same names, same hex)
  4. rebuilt file: ID/coverage channels are float32, ranks sorted by coverage, coverage sum <= 1 + 1e-5
Exit 0 = all pass.
"""
import json
import os
import re
import struct
import subprocess
import sys

import numpy as np
import OpenImageIO as oiio

TOL = 1e-6


def mmh3(s, seed=0):
    data = s.encode("utf-8"); n = len(data); h = seed; c1, c2 = 0xCC9E2D51, 0x1B873593
    rotl = lambda x, r: ((x << r) | (x >> (32 - r))) & 0xFFFFFFFF
    for i in range(0, n - n % 4, 4):
        k = struct.unpack("<I", data[i:i + 4])[0]
        k = rotl((k * c1) & 0xFFFFFFFF, 15); k = (k * c2) & 0xFFFFFFFF
        h = rotl(h ^ k, 13); h = (h * 5 + 0xE6546B64) & 0xFFFFFFFF
    tail = data[n - n % 4:]; k = 0
    for b in reversed(tail):
        k = (k << 8) | b
    if tail:
        k = rotl((k * c1) & 0xFFFFFFFF, 15); h ^= (k * c2) & 0xFFFFFFFF
    h ^= n; h ^= h >> 16; h = (h * 0x85EBCA6B) & 0xFFFFFFFF; h ^= h >> 13; h = (h * 0xC2B2AE35) & 0xFFFFFFFF
    return h ^ (h >> 16)


def fixed_hex(name):
    h = mmh3(name)
    if ((h >> 23) & 0xFF) in (0, 255):
        h ^= 1 << 23
    return "%08x" % h


def read_exr(path):
    """All channels of all subimages -> {channel: HxW float32 array}, {attr: value}, (h, w), {channel: type}."""
    inp = oiio.ImageInput.open(path)
    if not inp:
        raise SystemExit("cannot open %s: %s" % (path, oiio.geterror()))
    chans, attrs, types, sub = {}, {}, {}, 0
    while inp.seek_subimage(sub, 0):
        spec = inp.spec()
        px = inp.read_image(0, 0, 0, spec.nchannels, "float")
        for c, name in enumerate(spec.channelnames):
            chans[name] = np.ascontiguousarray(px[:, :, c], dtype=np.float32)
            fmt = spec.channelformat(c) if spec.channelformats else spec.format
            types[name] = str(fmt)
        for a in spec.extra_attribs:
            attrs[a.name] = a.value
        shape = (spec.height, spec.width)
        sub += 1
    inp.close()
    return chans, attrs, shape, types


def find_crypto(chans, attrs):
    """First Cryptomatte in the file -> (type name, manifest dict, [(id_bits, cov), ...] per rank)."""
    names = {k.split("/")[1]: v for k, v in attrs.items()
             if re.match(r"^(exr/)?cryptomatte/[0-9a-f]{7}/name$", k)}
    if not names:
        raise SystemExit("no cryptomatte metadata")
    key, tname = sorted(names.items())[0]
    prefix = "exr/" if any(k.startswith("exr/cryptomatte/") for k in attrs) else ""
    manifest = json.loads(attrs[prefix + "cryptomatte/%s/manifest" % key])
    # channel suffix may be R/G/B/A, r/g/b/a or red/green/blue/alpha (Blender writes lowercase)
    lut = {}
    for c in chans:
        m = re.match(r"^(%s\d\d)\.(r|g|b|a|red|green|blue|alpha)$" % re.escape(tname), c, re.I)
        if m:
            lut[(m.group(1), m.group(2)[0].lower())] = c
    layers = sorted({lay for lay, _ in lut})
    ranks = []
    for lay in layers:
        for idc, covc in (("r", "g"), ("b", "a")):
            ranks.append((chans[lut[(lay, idc)]].view(np.uint32), chans[lut[(lay, covc)]]))
    return tname, key, manifest, ranks


def pixel_dicts(ranks, shape):
    out = [[{} for _ in range(shape[1])] for _ in range(shape[0])]
    for ids, cov in ranks:
        ys, xs = np.nonzero(cov > 0)
        for y, x in zip(ys, xs):
            d = out[y][x]
            d[int(ids[y, x])] = d.get(int(ids[y, x]), 0.0) + float(cov[y, x])
    return out


def main():
    ref_path, work, exe = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(work, exist_ok=True)
    report, ok = {}, True

    chans, attrs, shape, _ = read_exr(ref_path)
    tname, key, manifest, ranks = find_crypto(chans, attrs)
    report["ref"] = {"type": tname, "key": key, "objects": len(manifest), "ranks": len(ranks), "size": shape}

    # 1. manifest hashes
    bad = {n: (h, fixed_hex(n)) for n, h in manifest.items() if h.lower() != fixed_hex(n)}
    report["hash_mismatches"] = bad
    ok &= not bad

    # mattes: coverage of each object, summed over ranks
    mattes = {}
    for name, hx in manifest.items():
        bits = int(hx, 16)
        m = np.zeros(shape, np.float32)
        for ids, cov in ranks:
            m += np.where(ids == bits, cov, 0.0).astype(np.float32)
        mattes[name] = m
    args = [exe, "crypto", "-o", os.path.join(work, "roundtrip.exr"), "--name", "CryptoObject",
            "--mode", "priority", "--ranks", str(len(ranks))] + sys.argv[4:]  # extra CLI flags pass through
    for i, (name, m) in enumerate(sorted(mattes.items())):
        p = os.path.join(work, "matte_%03d.exr" % i)
        o = oiio.ImageOutput.create(p)
        spec = oiio.ImageSpec(shape[1], shape[0], 1, "float")
        spec.channelnames = ("R",)
        spec.attribute("compression", "zip")
        o.open(p, spec); o.write_image(m.reshape(shape[0], shape[1], 1)); o.close()  # 2D arrays get garbled
        args += ["--matte", "%s=%s:R" % (name, p)]
    run = subprocess.run(args, capture_output=True, text=True)
    report["cli"] = {"exit": run.returncode, "stdout": run.stdout[-2000:], "stderr": run.stderr[-2000:]}
    if run.returncode != 0:
        print(json.dumps(report, indent=1)); return 1

    # 2-4. compare
    rchans, rattrs, rshape, rtypes = read_exr(os.path.join(work, "roundtrip.exr"))
    _, rkey, rmanifest, rranks = find_crypto(rchans, rattrs)
    report["rebuilt"] = {"key": rkey, "key_expected": fixed_hex("CryptoObject")[:7], "objects": len(rmanifest)}
    ok &= rkey == fixed_hex("CryptoObject")[:7]
    report["manifest_equal"] = {k: v.lower() for k, v in manifest.items()} == rmanifest
    ok &= report["manifest_equal"]

    a, b = pixel_dicts(ranks, shape), pixel_dicts(rranks, rshape)
    worst, bad_px, px_with_cov = 0.0, 0, 0
    for y in range(shape[0]):
        for x in range(shape[1]):
            da, db = a[y][x], b[y][x]
            if da:
                px_with_cov += 1
            d = max([abs(da.get(k, 0.0) - db.get(k, 0.0)) for k in set(da) | set(db)] or [0.0])
            worst = max(worst, d)
            bad_px += d > TOL
    report["pixels"] = {"with_coverage": px_with_cov, "mismatch_gt_1e-6": bad_px, "worst_abs_diff": worst}
    ok &= bad_px == 0

    floats = all("float" in t and "half" not in t for c, t in rtypes.items() if re.match(r"^CryptoObject\d\d\.", c))
    covs = np.stack([c for _, c in rranks])
    sorted_ok = bool(np.all(covs[:-1] >= covs[1:] - 0))
    sum_ok = bool(np.all(covs.sum(0) <= 1 + 1e-5))
    report["rebuilt_checks"] = {"float32": floats, "ranks_sorted": sorted_ok, "sum_le_1": sum_ok}
    ok &= floats and sorted_ok and sum_ok

    report["RESULT"] = "PASS" if ok else "FAIL"
    with open(os.path.join(work, "roundtrip_report.json"), "w") as f:
        json.dump(report, f, indent=1)
    print(json.dumps(report, indent=1))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
