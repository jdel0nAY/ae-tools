# aftereffects_tools

Public collection of After Effects scripts and panels by Jean Delaunay.

Each tool lives in its own subfolder and is a self-contained ScriptUI panel:
copy the tool's `.jsx` into After Effects' **ScriptUI Panels** folder, restart,
and open it from the **Window** menu. See each tool's own README for usage.

## Tools

| Tool | Description | Version |
|------|-------------|---------|
| [jd_ShotInfoPanel_forAfterEffects](jd_ShotInfoPanel_forAfterEffects/) | Pulls per-shot data (thumbnail, camera focal length, frame range, fps, resolution) from a Google Sheet into a dockable panel, with a comp HUD and thumbnail import. | 1.0.0 |

<!-- Add new tools as rows above. -->

## Naming & versioning

Tools follow `jd_<ToolName>_forAfterEffects`, and each build's file carries a
version suffix using underscores (e.g. `_v1_0_0.jsx`).

## License

[MIT](LICENSE) — applies to everything in this repository unless a subfolder
states otherwise.
