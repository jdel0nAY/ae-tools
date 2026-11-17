# Changelog

All notable changes to `jd_EXRCompiler`.

## 1.0.1

### Panel
- Reads TIFF passes and mattes (8/16-bit integer, 16/32-bit float), like `jd_exrc` already did.

### Docs
- One illustrated user guide (`README.md`) with annotated panel screenshots and results in AE;
  it is also the README in the release zip.

## 1.0.0

First public release.

### Panel (`jd_EXRCompiler.jsx`)
- One dockable panel with two tabs, **Passes** and **Cryptomatte**, filter boxes and scrolling lists.
- **From files:** passes and mattes from EXR, PNG or TIFF sequences; RGB mattes split into three objects;
  luminance mattes; priority order with up/down/move to top; bump version (`_v04` → `_v05`).
- **From comp layers:** selected layers render alone (alpha, luminance or an RGB split) through a
  one-time `jd_exrc EXR32` output-module template; a calibration frame stops the compile if the
  template changes values. The user's comps and render queue are left as they were.
- One `jd_exrc` call per compile, with progress; **Import and check** imports the result and adds AE's
  Cryptomatte effect. Setups are saved per comp next to the project.

### `jd_exrc`
- `crypto`, `merge`, `run job.json`, `check`, `info`; `--check` checks every frame as it is written.
- Cryptomatte per the specification: MurmurHash3 IDs with the exponent fix, ranked 32-bit float layers,
  inline or sidecar manifest, `priority` / `holdout` / `normalise` overlap modes, optional background
  and preview layer.
- Inputs: EXR (any layers or parts), PNG 8/16-bit, TIFF 8/16-bit integer and 16/32-bit float.
- Frame sequences, frame ranges, `--hold-missing`, multi-part output, `--json-log`, `--log-file`.
