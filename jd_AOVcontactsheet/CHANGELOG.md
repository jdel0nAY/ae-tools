# Changelog

All notable changes to `jd_AOVcontactsheet`.

## 1.1.0

### Added
- **Padding** — a configurable gap in pixels between cells, applied as both the
  gutter and the outer margin. Taken out of the width budget before the cells are
  scaled, so adding padding shrinks the cells rather than pushing the sheet past
  its maximum width.
- **Strip prefix from AOV names** — a literal or regex, anchored at the start of
  the name. `ViewLayer.` for Blender, `subimage\d+\.` for multi-part EXRs whose
  prefix differs per AOV. Empty keeps the automatic detection.

### Changed
- Cryptomatte rank layers now collapse even when no un-suffixed base layer exists,
  which is how Blender writes them — `CryptoObject00/01/02` becomes one cell.
- A layer prefix shared by every AOV is dropped from cell names automatically,
  compared by whole dot-separated segment rather than by character.
- Cell dimensions are floored rather than rounded, so rounding can no longer push
  the sheet one pixel past the maximum width. Cell height now follows cell width to
  keep the aspect exact.

### Fixed
- `decodeURI` was called unguarded in nine places. It throws on a lone per-cent
  sign, so a file named `shot_50%.exr` would have stopped the script.
- AOV names are sanitised before being built into a preset path. They come from the
  EXR header, so a channel named `../../x` could previously have sent the preset
  lookup outside the configured folder. Single dots are preserved, since an
  unstripped layer prefix legitimately contains them.
- `#` and `?` are now escaped when building a preset file URI; `encodeURI` leaves
  both alone and either breaks the path.
- The EXR header reader could read past the end of its buffer on a truncated file,
  where `charCodeAt` returns `NaN` and silently becomes 0 in bitwise maths. It now
  raises a clear error, and the attribute loop is capped.

## 1.0.0

Initial release. Builds a labelled contact sheet comp from a multi-layer EXR or
from a set of per-AOV files, applying one animation preset per AOV.
