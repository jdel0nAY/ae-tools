# jd_EXRCompiler — specification

Status: 1.0.0 (2026-11-16). This is the design document; user-facing docs are in `README.md` and `panel/README.md`.

## 1. Why

After Effects 2026 **reads** multichannel EXRs and has a built-in Cryptomatte effect, but it cannot:

1. **write** an EXR with more than one layer (its OpenEXR output module writes RGBA only);
2. **create** a Cryptomatte (the effect only reads one a 3D renderer made).

jd_EXRCompiler fills both gaps. It writes files; it is not an effect.

## 2. Shape

| Part | What | Notes |
|---|---|---|
| `jd_exrc` CLI (C++17, OpenEXR 3.x) | Reads image sequences, writes multi-layer EXRs and Cryptomattes, checks files | Windows first (MSVC, CMake, vcpkg manifest mode). Mac later via CI if wanted. No Adobe SDK, no GPU. |
| `jd_EXRCompiler.jsx` panel (ScriptUI) | Maps AE layers/comps or files on disk to passes and mattes, renders temp files, runs the CLI, imports the result | Only talks to the CLI through a job file (§5). |
| `tools/` Python | Independent reference checker (§8) | Separate code from the CLI so it can catch the CLI's mistakes. |

Both routes (AE layers, files on disk) end in the same thing: a **job file** the CLI runs. The CLI only ever reads files.

## 3. Channel merge

- **Inputs:** EXR (single-part, multi-layer, multi-part), PNG (8/16-bit), TIFF (8/16-bit integer, 16/32-bit float). Image sequences as `name.####.ext`, `name.%04d.ext` or a single still.
- **Mapping:** each output layer takes channels from one input: `source channel -> output channel`. Shorthands: `RGBA`, `RGB`, `A`, `Y` (single channel), or an explicit list (`R->Z`).
- **Naming:** output channels are `<layer>.<chan>` (`diffuse.R`, `depth.Z`). The layer called `beauty` (or flagged `--main`) is written bare (`R G B A`), as every app expects.
- **Pixel type per layer:** `half` (default for colour passes) or `float`. Data passes (depth, position, normals, motion, IDs) default to `float`.
- **Compression:** lossless only by default: `zip` (default), `zips`, `piz`, `rle`, `none`. `dwaa/dwab` allowed per layer only for colour passes, never on Cryptomatte or data layers.
- **Windows:** every input must share the display window. Data windows may differ; the output data window is their union, with empty areas filled with 0.
- **Metadata:** copied from one chosen input (default: the first EXR), so camera matrices etc. survive. The CLI adds `jd_exrcompiler/version` and `jd_exrcompiler/job` (the job JSON).
- **Colour:** no colour conversion, ever. Values go through bit for bit (8/16-bit integers are mapped to 0–1).
- **Output:** single-part by default (widest compatibility). Multi-part as an option (`--multipart`, one part per layer).
- **Frames:** frame range from the inputs (intersection); a missing frame is an error unless `--hold-missing`. Frames run in parallel.

## 4. Cryptomatte builder

Follows the Psyop Cryptomatte specification 1.2 and its reference code (`nuke/cryptomatte_utilities.py`, checked 2026-11-15).

### 4.1 Inputs → objects

- A **BW matte** (one channel, or RGB where R=G=B) gives one object. Channel used: `R` by default, or `A`/`Y`/luminance by choice.
- An **RGB(A) matte** gives one object per channel: R, G, B (and A if asked). Each channel gets its own name.
- Each object has a **name** (UTF-8). Names must be unique within one Cryptomatte. Values are clamped to 0–1.
- Objects come in a **priority list** (top = highest), set by the user; the panel's list order.

### 4.2 Coverage modes (overlapping mattes)

Let `m_i` be the matte values at a pixel, in priority order. Coverage `c_i`:

| Mode | Rule | Use |
|---|---|---|
| `priority` **(default)** | `c_i = min(m_i, 1 − Σ_{j<i} c_j)` | Correct both for mattes that already exclude each other (renderer coverage) and for full silhouettes that overlap. Continuous. |
| `holdout` | `c_i = m_i · Π_{j<i} (1 − m_j)` | Classic multiply holdout. Wrong at shared antialiased edges (two 0.5 edges give 0.5 and 0.25), so not the default. |
| `normalise` | if `Σ m > 1`: `c_i = m_i / Σ m`, else `c_i = m_i` | No priority; overlaps are shared. |

Optional `--background <name>`: an extra object with `c = 1 − Σ c_i` (only if > 0).

> `priority` is a priority holdout in a form that does not darken shared antialiased edges; the plain multiply version is available as `holdout`.

### 4.3 Hashing (exact)

- `h = MurmurHash3_x86_32(utf8(name), seed 0)` as uint32.
- `e = (h >> 23) & 0xFF`; if `e == 0` or `e == 255`: `h ^= 1 << 23` (no denormals, inf or NaN).
- **ID value** in the image = the float32 with the bits of `h`.
- **Manifest entry** = `"%08x"` of `h` **after** the fix (same as the reference `id_to_hex`).
- **Metadata key** for a Cryptomatte called `T`: the first 7 hex characters of the same conversion applied to `T` (reference `layer_hash`).

### 4.4 Ranks and layers

- Per pixel, keep the objects with `c > 0`, sorted by coverage (descending; ties broken by ID bits, so output is deterministic).
- `ranks` (default 6, even number, max 16) pairs are written: `T00.R = id0, T00.G = c0, T00.B = id1, T00.A = c1`, `T01` for ranks 2–3, etc. Empty ranks are `0, 0`.
- If a pixel has more objects than ranks, the smallest are dropped and counted (reported per frame).
- All `T##` channels are **float32**. Compression `zip` (or `zips`/`piz`/`none`); lossy compression is refused.
- Optional **preview** layer `T.R/G/B` (`--preview`): colour from the ID bits, weighted by coverage. Off by default.
- Type name `T`: default `CryptoMatte`; letters, digits and `_` only (no dots, so every reader splits names the same way).

### 4.5 Metadata (for each Cryptomatte `T`, key `k`)

```
cryptomatte/k/name       = T
cryptomatte/k/hash       = MurmurHash3_32
cryptomatte/k/conversion = uint32_to_float32
cryptomatte/k/manifest   = {"name":"hex8", ...}   (JSON, sorted by name)
```

`--manifest-sidecar` writes `T.json` next to the frames and stores `cryptomatte/k/manif_file` (relative path, forward slashes) instead, for very large manifests.

### 4.6 Combined output

A job may hold passes **and** one or more Cryptomattes (`CryptoMatte`, `CryptoMaterial`, ...); everything goes into one file per frame.

## 5. Job file and CLI

The panel writes a job file; people can too.

```json
{
  "version": 1,
  "output": "D:/shots/sh010/comp/sh010_compiled.####.exr",
  "frames": "auto",
  "compression": "zip",
  "multipart": false,
  "metadata_from": "beauty",
  "layers": [
    {"name": "beauty",  "main": true, "src": "renders/beauty.####.exr", "map": "RGBA", "type": "half"},
    {"name": "depth",   "src": "renders/z.####.exr", "map": "R->Z", "type": "float"}
  ],
  "cryptomattes": [
    {"name": "CryptoMatte", "ranks": 6, "mode": "priority", "background": null, "preview": false,
     "objects": [
       {"name": "hero",  "src": "mattes/hero.####.png", "channel": "R"},
       {"name": "car",   "src": "mattes/rgb.####.exr",  "channel": "R"},
       {"name": "tree",  "src": "mattes/rgb.####.exr",  "channel": "G"}
     ]}
  ]
}
```

Commands:

```
jd_exrc run job.json [--frames 1001-1100] [--threads N] [--dry-run]
jd_exrc merge -o out.####.exr --layer beauty=beauty.####.exr:RGBA --layer depth=z.####.exr:R->Z
jd_exrc crypto -o crypto.####.exr --name CryptoMatte --mode priority --matte hero=hero.####.png --matte car,tree,sky=rgb.####.exr:RGB
jd_exrc check file.exr       # spec checks (§8), exit code 0/1
jd_exrc info file.exr        # parts, layers, channels, types, windows, Cryptomattes found
```

Progress and errors: plain lines on stdout, and with `--json-log` one JSON object per line (the panel reads these). Exit code 0 = ok, 1 = check failed, 2 = bad job, 3 = I/O error.

## 6. AE panel

Layout: **one dockable panel, two tabs.** Native ScriptUI look.

- **Top (always visible):** title + version; source switch `From comp layers | From files`; comp name, frame range, size.
- **Tab "Passes" (row count in the tab label):** filter box; list with columns `★ main | name ← layer | channels | half/float`;
  buttons under the list: add, remove, up, down, **bump version** (repoints `_v04` → `_v05` on the selected rows).
  Row names come from the layer/file name with the version suffix removed.
- **Tab "Cryptomatte" (row count in the label):** filter box; list with columns `priority # | colour chip | object ← layer | from`
  (top row = highest priority); buttons: add, remove, up, down, **move to top**; under the list: overlap mode
  (Priority / Holdout / Normalise), ranks, background on/off. Colour chip = the object's ID preview colour.
  One Cryptomatte per file in v1.
- **Lists:** fixed visible height (about 9 rows, grows with the panel when the dock is resized), then scroll;
  multi-select; ScriptUI `listbox` with columns (no drag and drop, so reorder by buttons).
- **Bottom (always visible):** output path, compression, multi-part, **Import and check** (imports the result and puts
  AE's Cryptomatte effect on it), progress bar, one **Compile** button, one status line.
- **Matte source for a comp layer:** the layer's alpha by default; luminance for black-and-white footage with no alpha;
  an RGB matte splits into three rows (R, G, B), each named by the user.
- **Saved setups:** the panel remembers the setup per comp, so a recompile after a new render version is one click.

Behaviour:

- **Files mode:** pick files or sequences; same lists as above.
- **AE mode:** select layers in a comp. For each pass or matte the panel makes a temp comp (layer solo'd, same size, frame range, pixel aspect), queues it with a 32-bit EXR output module into a temp folder, renders, writes the job, runs `jd_exrc` (via `system.callSystem`), imports the result and offers to delete the temp files.
- **Colour:** temp renders must keep raw values (no working-space conversion on output; checked in managed/OCIO and unmanaged projects). Mattes are data.
- **EXR header reading** in JSX for the channel list: reuse the approach in `jd_AOVcontactsheet`.
- Needs AE's "Allow Scripts to Write Files and Access Network"; the panel explains if it is off.
- Settings saved per project (JSON next to the `.aep`, or project XMP).
- The panel finds `jd_exrc.exe` next to itself (`ScriptUI Panels/jd_EXRCompiler/`).
- Comp-layers mode needs a one-time output-module template named `jd_exrc EXR32` (OpenEXR, 32-bit float, RGB + Alpha, Preserve RGB). After Effects does not let a script set the output format or depth, so the panel cannot create it; it stops with instructions when the template is missing, and a 4x4 calibration frame rendered first stops the compile if the template changes values.

## 7. Layout in ae-tools

```
jd_EXRCompiler/
  README.md  CHANGELOG.md  SPEC.md  THIRD_PARTY.md
  cli/  CMakeLists.txt  vcpkg.json  src/  tests/
  panel/  jd_EXRCompiler.jsx
  tools/  check_crypto.py  make_test_mattes.py
  test-data/   small synthetic files only (< 1 MB total)
```

Dependencies (vcpkg manifest mode): `openexr` (+ `imath`, `libdeflate`), `nlohmann-json`, PNG via `stb_image`, TIFF via `libtiff` (+ `zlib`). No OpenImageIO (too heavy). Licences in `THIRD_PARTY.md`. Build outputs and `vcpkg_installed/` stay out of git.

## 8. Tests and acceptance

1. **Hash:** MurmurHash3 against published vectors, and every entry of a Blender-written manifest (name → hex) must match exactly.
2. **Ranks:** synthetic mattes (overlapping soft discs, 3+ objects per pixel, > 6 objects) against a separate NumPy reference.
3. **Modes:** `priority`, `holdout`, `normalise` and background on hand-worked pixels.
4. **Round trip:** Blender renders a test scene with a native CryptoObject (6 ranks) (`tools/blender_crypto_scene.py`). Extract each object's matte from it, rebuild with `jd_exrc` in `priority` mode, and compare: same IDs, coverage within 1e-6, same manifest (`tools/crypto_roundtrip.py`). Result for 1.0.0: bit-exact, 0 differing pixels.
5. **Readers:** the output must open and pick correctly in AE's Cryptomatte effect, Blender's Cryptomatte node, and Nuke/Fusion/Resolve where available. `jd_AOVcontactsheet` must list the merged layers correctly.
6. **Merge:** bit-exact channel copies (half and float), data-window union, metadata carried over, multi-part option opens in AE.
7. **`jd_exrc check`:** float32 IDs, no NaN/inf IDs, coverage in 0–1 and sum ≤ 1 (+1e-5), ranks sorted, manifest covers every ID present, metadata key matches the name, lossless compression.

## 9. Phases

| Version | Content | Gate |
|---|---|---|
| 0.1.0 | CLI: `crypto`, `merge`, `run`, `check`, `info`; tests 1–4, 6, 7 | local build + tests green |
| 0.2.0 | Panel, files mode | tested in After Effects 2026 |
| 0.3.0 | Panel, comp-layers mode (temp renders) | tested in After Effects 2026 (unmanaged, working-space and OCIO projects) |
| 1.0.0 | README, release zip (exe + panel), CI build | first public release |

## 10. Notes from testing

1. After Effects 2026's built-in Cryptomatte effect shows picked objects as `<hex>` IDs with an empty Manifest field, for Blender's own files too. Picking and mattes are correct; names are not shown.
2. Cycles writes a few Cryptomatte ranks out of order (8 of 61,440 pixels in a test CryptoMaterial). `jd_exrc check` reports this as a warning, not a failure.
3. OpenEXR 3.x converts pixel types only when reading, so half channels are converted before writing.
4. After Effects' `system.callSystem` does not pass a child program's output back; the panel runs `jd_exrc --log-file` and reads the log.
5. Still open: multi-part output in other readers (Nuke, Fusion, Resolve) has not been tested.
