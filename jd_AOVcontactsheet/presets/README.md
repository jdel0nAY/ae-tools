# presets

129 presets in one flat set: 123 EXtractoR and 6 Cryptomatte, covering pass names from Arnold, Redshift, V-Ray, Houdini Mantra and Blender.

## Naming

The script resolves one preset per AOV from the pattern in its configuration window.
The default is:

```
extractAOV_<AOV>.ffx
```

`<AOV>` is replaced by the AOV name, so a `Reflections` pass looks for
`extractAOV_Reflections.ffx`. Matching is case-insensitive unless **Match filename
case** is ticked.

No preset is needed for a beauty pass written as bare `R`/`G`/`B`/`A` — After
Effects shows those natively and the script builds that cell with no effect at all.

## Cryptomatte effect presets

These hold a real **Cryptomatte** effect rather than an EXtractoR, in both naming conventions so either matches.

| Preset | Size |
| --- | --- |
| `extractAOV_Crypto_Mats.ffx` | 4606 bytes |
| `extractAOV_Crypto_Nodes.ffx` | 7726 bytes |
| `extractAOV_Crypto_Object.ffx` | 4606 bytes |
| `extractAOV_CryptoMats.ffx` | 4606 bytes |
| `extractAOV_CryptoNodes.ffx` | 7726 bytes |
| `extractAOV_CryptoObject.ffx` | 4606 bytes |

`extractAOV_Cryptomatte.ffx` is an EXtractoR, not a Cryptomatte effect — it shows the raw ID hashes. Replace it with a Cryptomatte preset under that name if your renderer writes a single `Cryptomatte` layer, as Redshift does.

## Blender passes

Channel paths carry Blender's `ViewLayer.` prefix.

| Preset | Channels |
| --- | --- |
| `extractAOV_BloomCol.ffx` | `ViewLayer.BloomCol.R/G/B/(copy)` |
| `extractAOV_Combined.ffx` | `ViewLayer.Combined.R/G/B/A` |
| `extractAOV_Debug Sample Count.ffx` | `ViewLayer.Debug Sample Count.X/X/X/X` |
| `extractAOV_Denoising Albedo.ffx` | `ViewLayer.Denoising Albedo.R/G/B/(copy)` |
| `extractAOV_Denoising Depth.ffx` | `ViewLayer.Denoising Depth.Z/Z/Z/Z` |
| `extractAOV_Denoising Normal.ffx` | `ViewLayer.Denoising Normal.X/Y/Z/(copy)` |
| `extractAOV_DiffCol.ffx` | `ViewLayer.DiffCol.R/G/B/(copy)` |
| `extractAOV_DiffDir.ffx` | `ViewLayer.DiffDir.R/G/B/(copy)` |
| `extractAOV_DiffInd.ffx` | `ViewLayer.DiffInd.R/G/B/(copy)` |
| `extractAOV_Emit.ffx` | `ViewLayer.Emit.R/G/B/(copy)` |
| `extractAOV_Env.ffx` | `ViewLayer.Env.R/G/B/(copy)` |
| `extractAOV_GlossCol.ffx` | `ViewLayer.GlossCol.R/G/B/(copy)` |
| `extractAOV_GlossDir.ffx` | `ViewLayer.GlossDir.R/G/B/(copy)` |
| `extractAOV_GlossInd.ffx` | `ViewLayer.GlossInd.R/G/B/(copy)` |
| `extractAOV_IndexMA.ffx` | `ViewLayer.IndexMA.X/X/X/X` |
| `extractAOV_IndexOB.ffx` | `ViewLayer.IndexOB.X/X/X/X` |
| `extractAOV_Mist.ffx` | `ViewLayer.Mist.Z/Z/Z/Z` |
| `extractAOV_Normal.ffx` | `ViewLayer.Normal.X/Y/Z/(copy)` |
| `extractAOV_Position.ffx` | `ViewLayer.Position.X/Y/Z/(copy)` |
| `extractAOV_Shadow Catcher.ffx` | `ViewLayer.Shadow Catcher.R/G/B/(copy)` |
| `extractAOV_Shadow.ffx` | `ViewLayer.Shadow.R/G/B/(copy)` |
| `extractAOV_TransCol.ffx` | `ViewLayer.TransCol.R/G/B/(copy)` |
| `extractAOV_TransDir.ffx` | `ViewLayer.TransDir.R/G/B/(copy)` |
| `extractAOV_TransInd.ffx` | `ViewLayer.TransInd.R/G/B/(copy)` |
| `extractAOV_Transparent.ffx` | `ViewLayer.Transparent.R/G/B/A` |
| `extractAOV_UV.ffx` | `ViewLayer.UV.U/V/V/A` |
| `extractAOV_Vector.ffx` | `ViewLayer.Vector.X/Y/Z/W` |
| `extractAOV_VolumeDir.ffx` | `ViewLayer.VolumeDir.R/G/B/(copy)` |
| `extractAOV_VolumeInd.ffx` | `ViewLayer.VolumeInd.R/G/B/(copy)` |

## Everything else

| Preset | Channels |
| --- | --- |
| `extractAOV_Af.ffx` | `Af` in all four |
| `extractAOV_Albedo.ffx` | `Albedo.R/G/B/A` |
| `extractAOV_all_comp.ffx` | `all_comp.R/G/B/(copy)` |
| `extractAOV_AO.ffx` | `AO.R/G/B/A` |
| `extractAOV_atmosphere.ffx` | `atmosphere.R/G/B/(copy)` |
| `extractAOV_Background.ffx` | `Background.R/G/B/A` |
| `extractAOV_basecolor.ffx` | `basecolor.R/G/B/(copy)` |
| `extractAOV_Beauty_Noisy.ffx` | `Beauty_Noisy.R/G/B/A` |
| `extractAOV_bg.ffx` | `bg.R/G/B/(copy)` |
| `extractAOV_BumpNormals.ffx` | `BumpNormals.R/G/B/(copy)` |
| `extractAOV_C.ffx` | `C.R/G/B/A` |
| `extractAOV_Caustics.ffx` | `Caustics.R/G/B/A` |
| `extractAOV_CausticsRaw.ffx` | `CausticsRaw.R/G/B/A` |
| `extractAOV_coat.ffx` | `coat.R/G/B/(copy)` |
| `extractAOV_Cryptomatte.ffx` | `Cryptomatte.R/G/B/A` |
| `extractAOV_Depth.ffx` | `Depth.Z/Z/Z/(copy)` |
| `extractAOV_diffuse.ffx` | `diffuse.R/G/B/(copy)` |
| `extractAOV_diffuse_albedo.ffx` | `diffuse_albedo.R/G/B/(copy)` |
| `extractAOV_diffuse_direct.ffx` | `diffuse_direct.R/G/B/(copy)` |
| `extractAOV_diffuse_indirect.ffx` | `diffuse_indirect.R/G/B/(copy)` |
| `extractAOV_DiffuseFilter.ffx` | `DiffuseFilter.R/G/B/A` |
| `extractAOV_DiffuseLighting.ffx` | `DiffuseLighting.R/G/B/A` |
| `extractAOV_DiffuseLightingRaw.ffx` | `DiffuseLightingRaw.R/G/B/A` |
| `extractAOV_direct.ffx` | `direct.R/G/B/(copy)` |
| `extractAOV_direct_comp.ffx` | `direct_comp.R/G/B/(copy)` |
| `extractAOV_EdgeLength.ffx` | `EdgeLength.R/G/B/A` |
| `extractAOV_Emission.ffx` | `Emission.R/G/B/A` |
| `extractAOV_GI.ffx` | `GI.R/G/B/A` |
| `extractAOV_GIRaw.ffx` | `GIRaw.R/G/B/A` |
| `extractAOV_globalIllumination.ffx` | `globalIllumination.R/G/B/(copy)` |
| `extractAOV_ID.ffx` | `ID` in all four |
| `extractAOV_indirect.ffx` | `indirect.R/G/B/(copy)` |
| `extractAOV_indirect_comp.ffx` | `indirect_comp.R/G/B/(copy)` |
| `extractAOV_lighting.ffx` | `lighting.R/G/B/(copy)` |
| `extractAOV_materialId.ffx` | `materialId` in all four |
| `extractAOV_motionvector.ffx` | `motionvector.R/G/B/(copy)` |
| `extractAOV_MotionVectors.ffx` | `MotionVectors.R/G/B/(copy)` |
| `extractAOV_N.ffx` | `N.X / N.Y / N.Z / A` |
| `extractAOV_Normalized.ffx` | `Normalized.R/G/B/A` |
| `extractAOV_normals.ffx` | `normals.X/Y/Z/(copy)` |
| `extractAOV_ObjectID.ffx` | `ObjectID.R/R/R/(copy)` |
| `extractAOV_Of.ffx` | `Of.R/G/B/(copy)` |
| `extractAOV_Opacity.ffx` | `opacity.R/G/B/A` |
| `extractAOV_P.ffx` | `P.X / P.Y / P.Z / A` |
| `extractAOV_PuzzleMatte.ffx` | `PuzzleMatte.R/G/B/(copy)` |
| `extractAOV_PuzzleMatte1.ffx` | `PuzzleMatte1.R/G/B/(copy)` |
| `extractAOV_Pz.ffx` | `Pz` in all four |
| `extractAOV_rawGlobalIllumination.ffx` | `rawGlobalIllumination.R/G/B/(copy)` |
| `extractAOV_rawLighting.ffx` | `rawLighting.R/G/B/(copy)` |
| `extractAOV_rawShadow.ffx` | `rawShadow.R/G/B/(copy)` |
| `extractAOV_rawTotalLighting.ffx` | `rawTotalLighting.R/G/B/(copy)` |
| `extractAOV_reflect.ffx` | `reflect.R/G/B/(copy)` |
| `extractAOV_reflect_direct.ffx` | `reflect_direct.R/G/B/(copy)` |
| `extractAOV_reflect_indirect.ffx` | `reflect_indirect.R/G/B/(copy)` |
| `extractAOV_reflection.ffx` | `reflection.R/G/B/(copy)` |
| `extractAOV_reflectionFilter.ffx` | `reflectionFilter.R/G/B/(copy)` |
| `extractAOV_Reflections.ffx` | `Reflections.R/G/B/A` |
| `extractAOV_ReflectionsFilter.ffx` | `ReflectionsFilter.R/G/B/A` |
| `extractAOV_ReflectionsRaw.ffx` | `ReflectionsRaw.R/G/B/A` |
| `extractAOV_refract.ffx` | `refract.R/G/B/(copy)` |
| `extractAOV_refraction.ffx` | `refraction.R/G/B/(copy)` |
| `extractAOV_refractionFilter.ffx` | `refractionFilter.R/G/B/(copy)` |
| `extractAOV_Refractions.ffx` | `Refractions.R/G/B/A` |
| `extractAOV_RefractionsFilter.ffx` | `RefractionsFilter.R/G/B/A` |
| `extractAOV_RefractionsRaw.ffx` | `RefractionsRaw.R/G/B/A` |
| `extractAOV_renderId.ffx` | `renderId` in all four |
| `extractAOV_RGBA.ffx` | `RGBA.R/G/B/A` |
| `extractAOV_selfIllumination.ffx` | `selfIllumination.R/G/B/(copy)` |
| `extractAOV_ShadingPoints.ffx` | `ShadingPoints.R/G/B/A` |
| `extractAOV_shadow_matte.ffx` | `shadow_matte.R/G/B/(copy)` |
| `extractAOV_Shadows.ffx` | `Shadows.R/G/B/A` |
| `extractAOV_sheen.ffx` | `sheen.R/G/B/(copy)` |
| `extractAOV_specular.ffx` | `specular.R/G/B/(copy)` |
| `extractAOV_specular_albedo.ffx` | `specular_albedo.R/G/B/(copy)` |
| `extractAOV_specular_direct.ffx` | `specular_direct.R/G/B/(copy)` |
| `extractAOV_specular_indirect.ffx` | `specular_indirect.R/G/B/(copy)` |
| `extractAOV_SpecularLighting.ffx` | `SpecularLighting.R/G/B/A` |
| `extractAOV_SpecularLightingRaw.ffx` | `SpecularLightingRaw.R/G/B/A` |
| `extractAOV_SSS.ffx` | `SSS.R/G/B/A` |
| `extractAOV_TotalDiffuseLightingRaw.ffx` | `TotalDiffuseLightingRaw.R/G/B/A` |
| `extractAOV_totalLighting.ffx` | `totalLighting.R/G/B/(copy)` |
| `extractAOV_TranslucencyGI.ffx` | `TranslucencyGI.R/G/B/A` |
| `extractAOV_Transmission.ffx` | `transmission.R/G/B/A` |
| `extractAOV_transmission_albedo.ffx` | `transmission_albedo.R/G/B/(copy)` |
| `extractAOV_transmission_direct.ffx` | `transmission_direct.R/G/B/(copy)` |
| `extractAOV_transmission_indirect.ffx` | `transmission_indirect.R/G/B/(copy)` |
| `extractAOV_velocity.ffx` | `velocity.X/Y/Z/(copy)` |
| `extractAOV_volume.ffx` | `volume.R/G/B/(copy)` |
| `extractAOV_VolumeFogEmission.ffx` | `VolumeFogEmission.R/G/B/A` |
| `extractAOV_VolumeFogTint.ffx` | `VolumeFogTint.R/G/B/A` |
| `extractAOV_VolumeLighting.ffx` | `VolumeLighting.R/G/B/A` |
| `extractAOV_WorldPosition.ffx` | `WorldPosition.R/G/B/(copy)` |
| `extractAOV_Z.ffx` | `Z` in all four |
| `extractAOV_zdepth.ffx` | `zdepth` in all four |

## Known conflicts

A preset filename has to match the AOV name, while the channel path inside it has to
match the file. Where two renderers use the same AOV name but different channel
paths, only one version can live in a flat folder.

| AOV | This folder holds | Breaks on |
| --- | --- | --- |
| `Depth` | `Depth.Z` | Blender, which writes `ViewLayer.Depth.Z` |
| `AO` | `AO.R/G/B/A` | Blender, which writes `ViewLayer.AO.*` |
| `UV` | `ViewLayer.UV.U/V/V/A` | any renderer writing a plain `UV.R/G/B` layer |

The failure is quiet: the cell reports **found** in green and then renders blank,
because the preset applies cleanly but points at channels the file does not contain.
Worth knowing until a later version can detect it.

## Adding your own

1. Drop the multilayer EXR into a comp.
2. Apply the effect and set it up for the AOV.
3. Select **only the effect** in the timeline, not the whole layer.
4. **Animation → Save Animation Preset…**, named to match the pattern.

Or generate one without opening After Effects — see `../tools/`.
