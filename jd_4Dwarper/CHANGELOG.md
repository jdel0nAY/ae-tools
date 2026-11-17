# Changelog

All versions are test builds for After Effects 2026 (Windows, x64).

## 0.3.6 (18 Nov 2026): failures and layer changes on the banner
- **A failed Analyze turns the banner red** with the reason and what to do, and writes a log line (0.3.5 showed a
  message box but kept the old banner, and logged nothing). With the banner off, the message box still appears.
- **Changing the Source Layer or an Ignore Layer** after Analyze turns the banner orange: "needs re-analysis".
- The menu shows jd_4Dwarper 0.3.6. No change to the analysis.

## 0.3.5 (18 Nov 2026): faster, clearer panel
- **Faster Analyze**: about 50 seconds for 250 HD frames on a 24-thread CPU (0.3.4: about 63). Same quality on the
  test shot (judge 3: median 0.212 px, p95 1.92 px, the same frames above 3 px).
- **Shorter control names**, so they fit at the default panel width: Search Range (fr), Region Top Left / Bottom
  Right, Stiffness With Masks, Blend Frames; Ignore Layers Use shows Luma / Alpha. Projects keep their values.
- **The menu shows the version** (jd_4Dwarper 0.3.5), like the other JD tools.
- **Amber banner** when the first or last frames need source frames past the Source Layer's in or out point.
- **Clear failure** when an Ignore Layer ignores almost the whole frame (a masked solid read without its masks:
  precompose it).
- **About** text fits in its box.

## 0.3.4 (17 Nov 2026): renamed jd_4Dwarper
- The effect is now **jd_4Dwarper** (menu, panel, banner, messages) and the file `jd_4Dwarper.aex`. Copy it into any
  folder under `Plug-ins` and remove the old `4Dwarper.aex`.
- Projects made with 4Dwarper 0.3.x open with their settings and analysis (same internal name and controls).
- The log and the debug dump are now `%TEMP%\jd_4Dwarper_analyze.log` and `%TEMP%\jd_4Dwarper_dump`.
- **About** shows the version and a quick start; the new **Open Docs** button opens the docs (the About box no
  longer opens the browser by itself, and its cut-off link is gone).
- Docs at https://github.com/jdel0nAY/ae-tools/tree/main/jd_4Dwarper. No change to the analysis.

## 0.3.3 (17 Nov 2026): end of the take, panel, docs
- **Fixed: the end of a take.** When the target needs source frames past the source take's out point, the time
  path is no longer pulled away from the end, and the last frames hold the take's last frame with a warp that
  follows the set (0.3.2 could tear or stretch those frames). The banner and the log say how many frames are held.
- **New: debug dump.** With the folder `%TEMP%\4Dwarper_dump` present, Analyze writes the frames and masks as
  analysed, the frame ranges and the solved times there.
- Elsewhere in the shot the results are the same as 0.3.2.
- The controls are grouped by workflow: **Setup**, **Analyze**, **Warp**, **View** and **Advanced** (collapsed).
- Projects made with 0.3.x open with their values. Each control keeps its internal ID; only the order and some
  names changed (Mode is now **Solve**, Show Banner is now **Show Status Banner**).
- The status banner uses plain words: not analysed, ready (with frames and time offset range), needs re-analysis
  (with the reason) and failed (with what to do).
- The banner says when Setup or Advanced settings changed after the last Analyze.
- Cancelling Analyze keeps the previous analysis and its status.
- New **Help** button: shows the version and a quick start, and opens the docs.
- Docs: README, User Guide, Testing guide, this changelog.

## 0.3.2 (16 Nov 2026): Amounts, source range fix
- New **Space Warp Amount** and **Time Warp Amount** (0 to 100 %, keyframable). Both at 0 % show the source layer
  untouched.
- The solve holds the time offset where the source would run past its last frame, and never picks a frame outside
  the Source Layer's in and out points. This fixed CG frames past the end of a take showing up in the result.
- One shared rule picks the source frames for the effect and the command-line tool.

## 0.3.1 (16 Nov 2026): mask-aware defaults
- With Ignore Layers set, the stiffness prior is off by default (it hurt with hand-made masks).
  **Use Stiffness With Ignore Layers** turns it back on.
- **Edges** defaults to **Transparent**: nothing is invented where the source has no pixels.
- Edges = **Show Target** fills those areas with the target.

## 0.3.0 (16 Nov 2026): two ignore layers, new record format
- **Source Ignore Layer** next to the **Target Ignore Layer**, with luma or alpha, and **Ignore Mask Grow**.
- Each target frame stores the warp of both neighbouring source frames, each with its own local warp field.
  The render shows the nearer source frame (sharpest). **Blend Neighbour Frames** mixes the two.
- The analysis only uses source frames inside the Source Layer's in and out points.
- **Advanced** group: Hold Time After Frame, Perspective and Scale / Rotation Stiffness, Affine Only In Range.
- **Analysis Width** control.
- Projects analysed with 0.2 ask for a new Analyze (the old data is kept, never read).

## 0.2.0 (16 Nov 2026): ignore zones
- **Target Ignore Layer**: roto the talent so only the set drives the solve.
- **Analysis Region** (two points), **Auto Ignore Changes**.
- Analyze uses the layer's in and out points.
- Views: **Difference (exposure matched)**, **Ignored Zones**.
- Local warp grid for parallax (Local Warp, Local Warp Smoothness).

## 0.1.0 (16 Nov 2026): first build
- Analyze button: fits time (sub-frame) and space (homography) from the source pass to the target pass.
- 8, 16 and 32 bpc SmartFX render, multi-frame rendering safe.
- Results saved in the project, independent of resolution.
- Progress dialog with Cancel (and Esc), status banner in the comp view, log file in `%TEMP%\4Dwarper_analyze.log`.
- Views: Result, Difference, 50 % Mix. Edges: Extend Source Edge, Transparent.
