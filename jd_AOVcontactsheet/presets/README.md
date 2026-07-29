# presets

Animation presets for `jd_AOVcontactsheet`. One `.ffx` per AOV.

## Naming

The script resolves a preset filename per AOV from the pattern set in its
configuration window. The default is:

```
extractAOV_<AOV>.ffx
```

`<AOV>` is replaced by the AOV name, so:

| AOV | Preset file |
| --- | --- |
| `Beauty` | none needed — shown natively |
| `N` | `extractAOV_N.ffx` |
| `P` | `extractAOV_P.ffx` |
| `UV` | `extractAOV_UV.ffx` |
| `TextureMasks` | `extractAOV_TextureMasks.ffx` |
| `crypto_object` | `extractAOV_crypto_object.ffx` |

Matching is case-insensitive unless **Match filename case** is ticked.

## What goes inside

Usually an **EXtractoR** with its Red / Green / Blue / Alpha dropdowns set to the
AOV in question.

For Cryptomatte passes, save a **Cryptomatte** effect instead. The script applies
whatever the `.ffx` contains without inspecting it, so the crypto passes display
properly rather than as raw ID hashes.

## Making one

1. Drop the multi-layer EXR into a comp.
2. Apply the effect and configure it for the AOV.
3. Select **only the effect** in the timeline, not the whole layer.
4. **Animation → Save Animation Preset…** and name it to match the pattern.

Selecting just the effect matters: a preset that captured transform properties
will still work, since presets are applied before the grid layout is set, but it
will drag along anything else it grabbed.

## Populating this folder from After Effects

After Effects saves presets to its own User Presets folder by default. To copy the
ones you've already made into here:

**PowerShell**

```powershell
Copy-Item "$env:USERPROFILE\Documents\Adobe\After Effects 2026\User Presets\extractAOV_*.ffx" -Destination ".\presets\"
```

**macOS / bash**

```bash
cp ~/Documents/Adobe/After\ Effects\ 2026/User\ Presets/extractAOV_*.ffx ./presets/
```

Adjust the version in the path to match your install. You can also just leave the
script pointing at User Presets and ignore this folder entirely — it exists so a
preset library can travel with the repo.
