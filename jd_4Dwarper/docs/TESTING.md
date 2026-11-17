# Testing jd_4Dwarper

Four layers of tests, from fast and offline to slow and in After Effects.

## 1. Build
Windows, Visual Studio 2022 Build Tools (C++17, x64). The After Effects SDK is not in this repo (Adobe licence):
download it from Adobe and set the `AE_SDK` environment variable to its `Examples` folder.

```bat
build.bat
```

Outputs, all in `build\`:
- `Release\warper_test.exe`: synthetic tests.
- `Release\w4d_verify.exe`: checks the effect's analysis path against a reference solve.
- `Release\warper_cli.exe`: the same solve on PNG folders, offline.
- `dev\jd_4Dwarper.aex`: the effect. Never write it straight into After Effects' Plug-ins folder.

## 2. Synthetic tests: `warper_test`
No footage needed, about 30 seconds. Builds shots from generated images with a known time offset and warp, then
checks the solve finds them:
- per-frame homography error (pixels) and time error (frames): constant and variable-speed pans, an offset that
  drifts, a dolly-in with two depths, a near floor under a far set (parallax), talent over half the frame, and a
  shot analysed at one size and applied at twice that size;
- the frame pick never asks for source frames outside the source range (all Amounts, blend on or off);
- Space / Time Warp Amount: 0 % / 0 % gives the source frame bit for bit; 100 % / 100 % gives the full render.

```bat
build\Release\warper_test.exe
```
Every variant prints a line ending in PASS or FAIL. The last line is the overall result.

## 3. Effect path check: `w4d_verify`
Runs the analysis exactly the way the effect does (same settings struct, same mask handling, same record writer)
on PNG folders, and compares it with a reference `records.json` written by `warper_cli`:

```bat
build\Release\w4d_verify.exe <target frames> <source frames> <target masks> <source masks> <records.json> [--source-frames N]
```
PASS = source time within 0.0002 frames, homography and local warp within 0.01 px of the reference.
`--source-frames N` uses only source frames 0 to N-1, like a Source Layer out point in After Effects. It also
prints the time offset range and the last source times.

Other `w4d_verify` options: `--noise L` adds +-L levels of random noise to the analysed luma (how stable the
solve is to small decoding differences), `--dump <csv>` writes per-frame solved time, homography, mask share and
time scores, `--cand-stride N` / `--confidence 1` / `--time-smooth X` override solver settings. It opts out of
Windows power throttling, so timings are not slowed down when nobody is at the computer. Set the environment
variable `W4D_TIMING=1` to print the time of each solve phase.

## 4. Project compatibility: `check_param_ids.py`
After Effects finds a saved project's values by parameter ID. This script compares the controls of an older
version (from git) with the current source: same ID, type and default for every old control, no ID used twice.

```bat
python tools\check_param_ids.py <older commit or tag>
```
Give the commit or tag of the version to stay compatible with. It ends with PASS or FAIL. It does not replace opening a real old project in After Effects.

## 5. The judges (real footage, kept outside the repo)
The real test shot (two motion-control takes and the hand-made talent mattes) is client material and is not in
this repo. The judges run on PNG frames rendered by `warper_cli`.

**Judge 1, metric** (`judge_metrics.py`, needs numpy and ffmpeg). Measures how well the aligned source sits on the
target, on the set only:
- grey, blur, gradient magnitude (robust to lighting changes), 160 px tiles;
- sub-pixel phase correlation per tile; a tile counts only if both sides have texture, it is not under the
  ignore matte, and the correlation at the found shift is 0.6 or more (same content);
- with `--coverage`, tiles where the source has no pixels are left out.

Reported: per-frame median and p95 of the tile shift (px at 1080p), overall median and p95, and the "bad" frames
(p95 over 3 px). Target: median 0.5 px or less, p95 1.5 px or less, no bad frames.

```bat
python -I judge_metrics.py --target <target frames> --source <aligned frames> --ignore <target masks> --coverage <coverage masks> --out <dir> --label <name>
```

**Judge 2, visual** (`make_sheet.sh`): a contact sheet, per chosen frame, of the source over the target before and
after, plus full-resolution crops, so errors of 1 to 2 pixels show.

```bash
bash make_sheet.sh <aligned frames> <out.png> 20 80 116 160 220
```

**Judge 3** = judge 1 with `--coverage`. It is the reference for decisions. Results at 0.3.2 / 0.3.3 on the test
shot: median 0.218 px, p95 1.97 px, 5 bad frames (close-up parallax).

## 6. In After Effects
Done by a tester on a real project, never by the build:
1. Install the `.aex` with After Effects closed.
2. A project saved with the previous version opens with its values (compare a screenshot of the panel).
3. Analyze; compare the log line in `%TEMP%\jd_4Dwarper_analyze.log` (offset min, median, max) with the CLI.
4. Renders at 8, 16 and 32 bpc; Difference view at chosen frames; Amount ramps play without jumps.
