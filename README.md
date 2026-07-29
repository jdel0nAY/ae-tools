# aftereffects_tools

Public collection of After Effects scripts and panels. Some by myself, some with ai help.

Each tool lives in its own subfolder and is self-contained. **Panels** go in After
Effects' `Scripts/ScriptUI Panels/` folder — restart, then open from the
**Window** menu. **Scripts** go in `Scripts/` and run from **File → Scripts**.
Some tools ship extra assets alongside the `.jsx`; see each tool's own README for
usage and requirements.

## Tools

| Tool | Type | Description | Version |
|------|------|-------------|---------|
| [jd_ShotInfoPanel_forAfterEffects](jd_ShotInfoPanel_forAfterEffects/) | Panel | Pulls per-shot data (thumbnail, camera focal length, frame range, fps, resolution) from a Google Sheet into a dockable panel, with a comp HUD and thumbnail import. | 1.0.0 |
| [jd_AOVcontactsheet](jd_AOVcontactsheet/) | Script | Builds a labelled contact sheet comp of a render's AOVs — from one multi-layer EXR, or from a set of separate per-AOV files. Ships with 129 animation presets. | 1.1.0 |

<!-- Add new tools as rows above. -->

## License

[MIT](LICENSE) — applies to everything in this repository unless a subfolder
states otherwise.
