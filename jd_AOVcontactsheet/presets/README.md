# presets

Animation presets for `jd_AOVcontactsheet`. One `.ffx` per AOV.

Twenty presets are bundled here, covering an Arnold-style AOV set: 19 built on
**EXtractoR** and one on **Cryptomatte**.

## Naming

The script resolves a preset filename per AOV from the pattern set in its
configuration window. The default is:

```
extractAOV_<AOV>.ffx
```

`<AOV>` is replaced by the AOV name, so an AOV called `Reflections` looks for
`extractAOV_Reflections.ffx`.

Matching is case-insensitive unless **Match filename case** is ticked in the
config panel.

## What's included

**Lighting and shading components**

| Preset | Effect |
| --- | --- |
| `extractAOV_DiffuseLighting.ffx` | EXtractoR |
| `extractAOV_SpecularLighting.ffx` | EXtractoR |
| `extractAOV_GI.ffx` | EXtractoR |
| `extractAOV_Reflections.ffx` | EXtractoR |
| `extractAOV_Refractions.ffx` | EXtractoR |
| `extractAOV_Transmission.ffx` | EXtractoR |
| `extractAOV_Opacity.ffx` | EXtractoR |
| `extractAOV_VolumeLighting.ffx` | EXtractoR |
| `extractAOV_VolumeFogEmission.ffx` | EXtractoR |
| `extractAOV_VolumeFogTint.ffx` | EXtractoR |

**Data and utility passes**

| Preset | Effect |
| --- | --- |
| `extractAOV_N.ffx` | EXtractoR |
| `extractAOV_P.ffx` | EXtractoR |
| `extractAOV_Z.ffx` | EXtractoR |
| `extractAOV_UV.ffx` | EXtractoR |
| `extractAOV_Normalized.ffx` | EXtractoR |
| `extractAOV_ShadingPoints.ffx` | EXtractoR |
| `extractAOV_EdgeLength.ffx` | EXtractoR |

**Mattes**

| Preset | Effect |
| --- | --- |
| `extractAOV_PuzzleMatte.ffx` | EXtractoR |
| `extractAOV_PuzzleMatte1.ffx` | EXtractoR |
| `extractAOV_Crypto_Object.ffx` | **Cryptomatte** |

No preset is needed for the beauty pass — bare `R`/`G`/`B`/`A` channels are what
After Effects shows by default, so that cell is built with no effect at all.

> **Note on `Crypto_Object`** — most renderers write this layer lowercase as
> `crypto_object`, which still resolves to this file because matching is
> case-insensitive by default. If you tick **Match filename case**, rename the
> preset to match your layer exactly.

## What goes inside a preset

Usually an **EXtractoR** with its Red / Green / Blue / Alpha dropdowns set to the
AOV in question.

For Cryptomatte passes, save a **Cryptomatte** effect instead. The script applies
whatever the `.ffx` contains without inspecting it, so crypto passes display
properly rather than as raw ID hashes.

## Adding your own

1. Drop the multi-layer EXR into a comp.
2. Apply the effect and configure it for the AOV.
3. Select **only the effect** in the timeline, not the whole layer.
4. **Animation -> Save Animation Preset...** and name it to match the pattern.

Selecting just the effect matters: a preset that captured transform properties
will still work, since presets are applied before the grid layout is set, but it
will drag along anything else it grabbed.

## Copying presets out of After Effects

After Effects saves to its own User Presets folder by default. To bring new ones
into this folder:

**PowerShell**

```powershell
Copy-Item "$env:USERPROFILE\Documents\Adobe\After Effects 2026\User Presets\extractAOV_*.ffx" -Destination ".\presets\"
```

**macOS / bash**

```bash
cp ~/Documents/Adobe/After\ Effects\ 2026/User\ Presets/extractAOV_*.ffx ./presets/
```

Adjust the version in the path to match your install.

## A note on why these are files and not code

The channel selection inside an EXtractoR preset lives in a parameter called
`Channel Info` (matchName `EXtractoR-0013`), stored as an opaque binary blob.
After Effects reports it to scripting as `PropertyValueType.NO_VALUE` and refuses
both reads and writes, which is precisely why the script applies saved presets
instead of setting the dropdowns itself. See the main README for the full account.
