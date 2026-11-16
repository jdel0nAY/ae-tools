# jd_EXRCompiler panel

An After Effects panel that turns separate render passes into **one multi-layer EXR**
and builds a **Cryptomatte** from your black-and-white or RGB mattes, so AE's own
Cryptomatte effect (and Nuke, Fusion, Blender...) can pick objects from it.

After Effects can read multi-layer EXRs and Cryptomattes but cannot write them.
The panel does the setup; the small `jd_exrc` program next to it writes the files.

It works from **files on disk** or straight from **layers in a comp**.

Windows only for now (`jd_exrc.exe`). Tested in After Effects 2026.

---

## Install

Copy both files into After Effects' `ScriptUI Panels` folder:

```
C:\Program Files\Adobe\Adobe After Effects <version>\Support Files\Scripts\ScriptUI Panels\
```

- `jd_EXRCompiler.jsx`
- `jd_exrc.exe` (next to the .jsx, or inside a folder called `jd_EXRCompiler` next to it)

Restart After Effects and open **Window → jd_EXRCompiler.jsx**. It docks like any panel.
You can also run it as a floating window with **File → Scripts → Run Script File…**.

Turn on **Edit → Preferences → Scripting & Expressions → Allow Scripts to Write Files and
Access Network**. The panel tells you if it is off.

## Quick start

1. Open the comp you are working on and press **Use active comp** (the setup is saved per comp).
2. **Passes** tab → **Add files…** → pick one frame of each pass. Sequences are found
   automatically (`name.1001.exr` becomes `name.####.exr`).
3. **Cryptomatte** tab → **Add mattes…** → pick one frame of each matte.
   An RGB matte asks whether to split it into three objects (R, G, B) and lets you name each.
4. Check the **Output** path, then press **Compile**.

With **Import and check** on, the result is checked, imported as a sequence and, if a comp is
active, added to it with AE's Cryptomatte effect.

## From comp layers

Switch the top of the panel to **From comp layers**, select layers in your comp and press
**Add selected** on either tab.

- **Passes**: each layer becomes a pass, named after the layer.
- **Cryptomatte**: each layer becomes an object from its **alpha** (or its **luminance** if it
  is footage without alpha). Choose *RGB matte* to split a layer into R, G and B objects instead.

**One-time setup.** After Effects does not let scripts choose OpenEXR or float depth for a render,
so make one output-module template by hand: **Edit → Templates → Output Module…**, **New**, name it
**jd_exrc EXR32**, Format **OpenEXR** (Format Options: 32-bit float, a lossless compression such as
ZIP), Channels **RGB + Alpha**, Depth **Floating Point+**, Color **Premultiplied**, and on the
**Color Management** tab tick **Preserve RGB** (otherwise AE converts the values to linear light). The panel only
uses this template; it never creates or changes templates. Without it, Compile stops and shows these
steps. (A 32-bit float TIFF template under the same name works too.)

On **Compile** the panel renders each of those layers on its own with that template to a temporary
sequence, then compiles. For each layer it uses a temporary copy of the comp with only that
layer switched on (cameras, lights and its track matte stay on), the same size, pixel aspect,
frame rate and work area. Your own comps are not changed. The temporary comps and render-queue
items are removed afterwards, and other items in your render queue are left as they were.
The panel then offers to delete the temporary renders (they sit in a `_jd_exrc_tmp_…` folder next
to the output).

Values are written as the comp computes them, with no output colour conversion; float keeps values
above 1 and below 0. Before rendering any layer, the panel renders a tiny calibration frame
with the template and stops with instructions if the values came back changed.

File rows and layer rows can be mixed in one setup.

## The two tabs

### Passes

| Column | Meaning |
|---|---|
| ★ | Main layer, written as bare `R G B A` (what every app shows by default). One per file. |
| Name ← source | The layer name in the output (`diffuse.R`, `diffuse.G`…), and the file it comes from. |
| Channels | What to copy: `RGBA`, `RGB`, `Z`, or a mapping such as `R->Z`. |
| Type | `half` (colour passes) or `float` (depth, position, normals, motion, IDs). |

- **Edit…** (or double-click) changes a row's name, source, layer inside the file, channels,
  type and the ★.
- **Bump version** moves the selected rows to the next version (`_v04` → `_v05`, in the file
  name and in a `v04` folder), only when the new version exists on disk. Press Compile and
  you are done.
- Names come from the file names with frame numbers, versions and the shared shot prefix removed.

### Cryptomatte

| Column | Meaning |
|---|---|
| # | Priority. The top row wins where mattes overlap. |
| ID | The object's colour chip and Cryptomatte ID (the colour AE shows in its preview). |
| Object ← source | The name you pick in AE, and the file it comes from. |
| Channel | `R`, `G`, `B`, `A`, or `L` (luminance). A black-and-white matte uses `R`. |

Under the list:

- **Overlap**: *Priority* (default; higher rows win, soft edges stay soft), *Holdout* (classic
  multiply), *Normalise* (overlaps are shared, no priority).
- **Ranks**: how many objects one pixel can hold (6 is the usual value).
- **Background**: adds an object that covers whatever no matte covers.
- **Cryptomatte name**: the layer name AE's effect lists (`CryptoMatte` by default).

Object names must be unique; the panel tells you if two names would get the same ID.

## Output options

- **Compression**: `zip` (default), `zips`, `piz`, `rle` or `none`. All lossless, as Cryptomatte needs.
- **Multi-part**: one EXR part per layer. Off by default because single-part files open everywhere.
- Use `####` in the output name for the frame number.

## Where setups are saved

In a small file next to your project: `<project name>.jd_EXRCompiler.json`, one setup per comp.
Projects that were never saved use a file in your user preferences folder until they are.
Every change is saved straight away.

## Good to know

- After Effects is busy while layers render and while `jd_exrc` runs. Long jobs run in up to 10 parts
  so the progress bar moves, but you cannot work in AE until it finishes.
- Inputs: EXR (any layers or parts), PNG (8 or 16 bit) and TIFF (8/16-bit, 16/32-bit float).
  Values pass through untouched; no colour conversion.
- If AE's Cryptomatte effect does not show the right layer, pick it in the effect's Layer menu.
- AE's Cryptomatte effect lists picked objects as hex IDs such as `<a94a3318>`, not names. It does
  the same for files from 3D renderers; the mattes are correct.

## Licence

MIT, like the rest of this repository. `jd_exrc.exe` includes open-source libraries; their licences
are listed in `THIRD_PARTY.md` and shipped in the release zip.
