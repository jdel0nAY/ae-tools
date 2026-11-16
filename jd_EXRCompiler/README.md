# jd_EXRCompiler

Fills two gaps in After Effects:

- **Merge passes into one multi-layer EXR.** AE reads multi-layer EXRs but can only write RGBA.
- **Build a Cryptomatte from mattes.** Turn black-and-white or RGB mattes (or comp layers) into a
  spec-exact Cryptomatte that AE's built-in Cryptomatte effect, Nuke, Fusion and Blender can pick from.

Two parts:

| Part | What it is |
|---|---|
| [`panel/`](panel/) | The After Effects panel (`jd_EXRCompiler.jsx`): two tabs, Passes and Cryptomatte, working from files on disk or from comp layers. **Start here:** [panel/README.md](panel/README.md). |
| [`cli/`](cli/) | `jd_exrc`, the small program that writes the files. The panel runs it for you; you can also use it on its own or in a pipeline. |

Windows only for now. Tested in After Effects 2026 and Blender 5.2.

## Install

Download `jd_EXRCompiler-<version>-win64.zip` from the repository's Releases page and copy
`jd_EXRCompiler.jsx` and `jd_exrc.exe` into AE's `Scripts\ScriptUI Panels` folder. Details, and the
one-time output-module template that comp-layers mode needs, are in [panel/README.md](panel/README.md).

## jd_exrc on its own

```
jd_exrc crypto -o crypto.####.exr --matte hero=hero.####.png --matte car,tree,sky=rgb.####.exr:RGB
jd_exrc merge  -o out.####.exr --layer beauty=beauty.####.exr:RGBA --layer depth=z.####.exr:R->Z
jd_exrc run job.json --check
jd_exrc check file.exr
jd_exrc info file.exr
```

`merge` and `crypto` take the same options, so one command can write passes and a Cryptomatte
together. `jd_exrc --help` lists everything. Exit codes: 0 ok, 1 check failed, 2 bad job, 3 file error.

- **Inputs:** EXR (single-part, multi-layer, multi-part), PNG (8/16-bit), TIFF (8/16-bit integer,
  16/32-bit float). Frame patterns `####` or `%04d`; frame ranges found automatically or given with
  `--frames A-B`.
- **Cryptomatte:** MurmurHash3 IDs, ranks sorted by coverage, 32-bit float, manifest inline or as a
  sidecar file, following the Cryptomatte specification. Overlapping mattes: `priority` (default, top
  matte wins without darkening shared edges), `holdout` or `normalise`.
- **Merge:** bit-exact channel copies, half or float per layer, lossless compression by default,
  metadata (camera etc.) carried over from the main pass.

## Build from source

Needs Visual Studio 2022 (C++ tools, CMake) and vcpkg (Visual Studio ships one).

```
cd cli
vcpkg install --triplet x64-windows-static-md --x-install-root=vcpkg_installed
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=%CD%/vcpkg_installed/x64-windows-static-md
cmake --build build --config Release
ctest --test-dir build -C Release
```

The panel's pure-JavaScript parts have tests too: `node panel/tests/test_core.js`.

## How it was checked

- 32 core tests and an end-to-end CLI test (procedural test mattes only).
- Round trip against Blender: a Cryptomatte rendered by Blender, split into per-object mattes and
  rebuilt by `jd_exrc`, comes back bit for bit (`tools/`).
- After Effects 2026: the panel's files mode and comp-layers mode, in unmanaged, working-space and
  OCIO projects; values pass through raw, picking gives the right matte per object.

Design notes: [SPEC.md](SPEC.md). Changes: [CHANGELOG.md](CHANGELOG.md).
Third-party libraries in `jd_exrc.exe`: [THIRD_PARTY.md](THIRD_PARTY.md).
