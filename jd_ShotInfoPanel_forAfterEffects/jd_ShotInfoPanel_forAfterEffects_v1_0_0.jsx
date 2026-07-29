/*
jd_ShotInfoPanel_forAfterEffects_v1_0_0.jsx
===========================================

Dockable After Effects (ScriptUI) panel that pulls per-shot production data
from a Google Sheet: a thumbnail, the shot's camera / frame-range / resolution
info, an overlay HUD, and one-click import of the thumbnail into the comp.
The After Effects counterpart of the Maya / Blender "Shot Info" tool.

INSTALL
-------
Copy this file into After Effects' "ScriptUI Panels" folder:
  - Win: <AE install>/Support Files/Scripts/ScriptUI Panels/
  - Mac: /Applications/Adobe After Effects <ver>/Scripts/ScriptUI Panels/
Restart AE, then open it from  Window > jd_ShotInfoPanel_forAfterEffects_v1_0_0.
(The panel itself is titled "Shot Info".)
Also enable  Preferences > Scripting & Expressions > "Allow Scripts to Write
Files and Access Network".

PLATFORM NOTES (how AE differs from Maya/Blender)
-------------------------------------------------
* Networking: ExtendScript has no HTTPS, so the sheet is fetched with `curl`
  via system.callSystem(). curl ships with modern macOS and Windows 10/11.
* =IMAGE("url") thumbnails: Google's CSV export returns those cells empty. We
  recover the URLs from the XLSX export by reading the worksheet XML with
  `tar` (best-effort; if unavailable the rest still works). Plain URL columns
  are used directly.
* Thumbnail is shown at native size (ScriptUI can't scale images live) — keep
  sheet thumbnails modest, or just use "Import Thumbnail to Comp".
* "HUD" is a real text layer in the active comp, so it is captured by RAM
  preview and render (the playblast equivalent).
* Config is stored app-wide via app.settings (persists across sessions).
  AE has no clean per-project store, so there is no per-project shot memory;
  use AUTO to derive the shot from the active comp / project name instead.

Tested target: After Effects (ScriptUI). Version 1.0.0.
*/

(function (thisObj) {

    // ------------------------------------------------------------------ //
    // Constants
    // ------------------------------------------------------------------ //
    var SECTION = "jd_ShotInfoPanel_forAE";
    var HUD_NAME = "jd_ShotInfo_HUD";

    var FIELD_DEFS = [
        ["col_shot",   "Shot Number",              true],
        ["col_thumb",  "Thumbnail (URL)",          false],
        ["col_focal",  "Camera Focal Length",      false],
        ["col_start",  "Start Frame",              false],
        ["col_end",    "End Frame",                false],
        ["col_fps",    "Frame Rate",               false],
        ["col_res",    "Resolution (e.g. 1920x1080)", false],
        ["col_res_w",  "Res Width (optional)",     false],
        ["col_res_h",  "Res Height (optional)",    false]
    ];

    var DEFAULTS = {
        url: "", gid: "",
        col_shot: "shot", col_thumb: "thumbnail", col_focal: "focal",
        col_start: "start", col_end: "end", col_fps: "fps",
        col_res: "resolution", col_res_w: "", col_res_h: ""
    };

    var CONFIG_KEYS = ["url", "gid", "col_shot", "col_thumb", "col_focal",
        "col_start", "col_end", "col_fps", "col_res", "col_res_w", "col_res_h"];

    // Session state
    var CACHE = null;       // { headers:[], data:[{}] }
    var LAST_INFO = null;
    var LAST_SHOT = "";
    var THUMB_PATH = "";
    var HUD_ON = false;

    // ------------------------------------------------------------------ //
    // Small helpers
    // ------------------------------------------------------------------ //
    function trim(s) { return String(s).replace(/^\s+/, "").replace(/\s+$/, ""); }

    function tempFile(name) { return new File(Folder.temp.fsName + "/" + name); }

    function curlToFile(url, outFile) {
        try { if (outFile.exists) { outFile.remove(); } } catch (e) {}
        var cmd = 'curl -s -L --max-time 30 -o "' + outFile.fsName + '" "' + url + '"';
        try { system.callSystem(cmd); } catch (e) { return false; }
        return outFile.exists && outFile.length > 0;
    }

    function readTextFile(f) {
        f.encoding = "UTF-8";
        f.open("r");
        var s = f.read();
        f.close();
        return s;
    }

    function extractId(url) {
        var m = /\/spreadsheets\/d\/([a-zA-Z0-9\-_]+)/.exec(String(url));
        return m ? m[1] : "";
    }
    function extractGid(url) {
        var m = /[#&?]gid=(\d+)/.exec(String(url));
        return m ? m[1] : "";
    }

    function directImageUrl(url) {
        if (!url) return url;
        var m = /drive\.google\.com\/file\/d\/([a-zA-Z0-9\-_]+)/.exec(url)
             || /drive\.google\.com\/open\?id=([a-zA-Z0-9\-_]+)/.exec(url)
             || /drive\.google\.com\/uc\?[^\s]*id=([a-zA-Z0-9\-_]+)/.exec(url);
        if (m) return "https://drive.google.com/thumbnail?id=" + m[1] + "&sz=w1024";
        return url;
    }

    function digits(s) { return String(s).replace(/\D/g, ""); }

    // ------------------------------------------------------------------ //
    // Config persistence (app.settings, app-wide)
    // ------------------------------------------------------------------ //
    function loadCfg() {
        var cfg = {};
        for (var i = 0; i < CONFIG_KEYS.length; i++) {
            var k = CONFIG_KEYS[i];
            cfg[k] = app.settings.haveSetting(SECTION, k)
                ? app.settings.getSetting(SECTION, k) : DEFAULTS[k];
        }
        return cfg;
    }
    function saveCfgKey(k, v) {
        try { app.settings.saveSetting(SECTION, k, String(v)); } catch (e) {}
    }

    // ------------------------------------------------------------------ //
    // CSV parsing
    // ------------------------------------------------------------------ //
    function parseCSV(text) {
        var rows = [], row = [], field = "", i = 0, inQ = false;
        var n = text.length;
        while (i < n) {
            var c = text.charAt(i);
            if (inQ) {
                if (c === '"') {
                    if (i + 1 < n && text.charAt(i + 1) === '"') { field += '"'; i += 2; continue; }
                    inQ = false; i++; continue;
                }
                field += c; i++; continue;
            } else {
                if (c === '"') { inQ = true; i++; continue; }
                if (c === ',') { row.push(field); field = ""; i++; continue; }
                if (c === '\r') { i++; continue; }
                if (c === '\n') { row.push(field); rows.push(row); row = []; field = ""; i++; continue; }
                field += c; i++; continue;
            }
        }
        row.push(field);
        if (row.length > 1 || trim(row[0]) !== "") { rows.push(row); }
        return rows;
    }

    function rowsToObjects(rows) {
        if (rows.length === 0) return { headers: [], data: [] };
        var headers = [];
        for (var i = 0; i < rows[0].length; i++) headers.push(trim(rows[0][i]));
        var data = [];
        for (var r = 1; r < rows.length; r++) {
            var obj = {};
            for (var c = 0; c < headers.length; c++) {
                obj[headers[c]] = (c < rows[r].length) ? rows[r][c] : "";
            }
            data.push(obj);
        }
        return { headers: headers, data: data };
    }

    // ------------------------------------------------------------------ //
    // =IMAGE() URL recovery from the XLSX export (best-effort, via tar)
    // ------------------------------------------------------------------ //
    function colIndexToLetter(idx) {
        var s = "", n = idx + 1;
        while (n > 0) { var m = (n - 1) % 26; s = String.fromCharCode(65 + m) + s; n = Math.floor((n - 1) / 26); }
        return s;
    }

    function decodeXml(s) {
        return String(s).replace(/&quot;/g, '"').replace(/&lt;/g, "<")
            .replace(/&gt;/g, ">").replace(/&apos;/g, "'").replace(/&amp;/g, "&");
    }

    function recoverImageUrls(cfg, parsed) {
        try {
            var thumbCol = trim(cfg.col_thumb);
            if (thumbCol === "") return;
            var thumbIdx = -1;
            for (var i = 0; i < parsed.headers.length; i++) {
                if (parsed.headers[i] === thumbCol) { thumbIdx = i; break; }
            }
            if (thumbIdx < 0) return;

            var anyEmpty = false;
            for (var d = 0; d < parsed.data.length; d++) {
                if (trim(parsed.data[d][thumbCol]) === "") { anyEmpty = true; break; }
            }
            if (!anyEmpty) return;

            var id = extractId(cfg.url);
            if (!id) return;
            var xlsx = tempFile("jd_shotinfo.xlsx");
            if (!curlToFile("https://docs.google.com/spreadsheets/d/" + id + "/export?format=xlsx", xlsx)) return;

            var letter = colIndexToLetter(thumbIdx);
            var xml = "";
            for (var sh = 1; sh <= 8; sh++) {
                var out = "";
                try { out = system.callSystem('tar -xOf "' + xlsx.fsName + '" xl/worksheets/sheet' + sh + '.xml'); } catch (e) { out = ""; }
                if (out && out.length > 0 && out.indexOf("<row") >= 0) {
                    xml = out;
                    if (out.indexOf("http") >= 0) break;
                }
            }
            if (!xml) return;

            var rowRe = /<row[^>]*\br="(\d+)"[^>]*>([\s\S]*?)<\/row>/g;
            var m, byRow = {};
            while ((m = rowRe.exec(xml)) != null) {
                var rn = parseInt(m[1], 10);
                var cellRe = new RegExp('<c[^>]*\\br="' + letter + rn + '"[^>]*>([\\s\\S]*?)<\\/c>');
                var cm = cellRe.exec(m[2]);
                if (cm) {
                    var fm = /<f[^>]*>([\s\S]*?)<\/f>/.exec(cm[1]);
                    var src = decodeXml(fm ? fm[1] : cm[1]);
                    var um = /https?:\/\/[^"'<>\s\)]+/.exec(src);
                    if (um) byRow[rn] = um[0];
                }
            }
            for (var key in byRow) {
                var di = parseInt(key, 10) - 2;
                if (di >= 0 && di < parsed.data.length && trim(parsed.data[di][thumbCol]) === "") {
                    parsed.data[di][thumbCol] = byRow[key];
                }
            }
        } catch (e) { /* best-effort: leave thumbnails blank */ }
    }

    // ------------------------------------------------------------------ //
    // Fetch + lookup
    // ------------------------------------------------------------------ //
    function fetchSheet(cfg) {
        var id = extractId(cfg.url);
        if (!id) return { ok: false, error: "Could not parse a spreadsheet ID from the URL." };
        var gid = trim(cfg.gid);
        if (gid === "") gid = extractGid(cfg.url);
        if (gid === "") gid = "0";
        var url = "https://docs.google.com/spreadsheets/d/" + id + "/export?format=csv&gid=" + gid;
        var out = tempFile("jd_shotinfo.csv");
        if (!curlToFile(url, out)) {
            return { ok: false, error: "Download failed. Check the URL, that the sheet is shared 'Anyone with the link -> Viewer', and that curl is available." };
        }
        var text = readTextFile(out);
        if (/^\s*<(!doctype html|html)/i.test(text)) {
            return { ok: false, error: "The sheet returned an HTML page, not CSV. It is probably not shared publicly." };
        }
        var rows = parseCSV(text);
        if (rows.length === 0) return { ok: false, error: "The sheet appears to be empty." };
        var parsed = rowsToObjects(rows);
        recoverImageUrls(cfg, parsed);
        return { ok: true, headers: parsed.headers, data: parsed.data };
    }

    function findShotRow(data, cfg, shot) {
        var col = cfg.col_shot;
        shot = trim(shot);
        if (col === "" || shot === "") return null;
        var i;
        for (i = 0; i < data.length; i++) {
            if (trim(data[i][col]) === shot) return data[i];
        }
        var target = digits(shot);
        if (target !== "") {
            var ti = parseInt(target, 10);
            for (i = 0; i < data.length; i++) {
                var dstr = digits(data[i][col]);
                if (dstr !== "" && parseInt(dstr, 10) === ti) return data[i];
            }
        }
        return null;
    }

    function parseResolution(row, cfg) {
        var w = trim(row[cfg.col_res_w] || "");
        var h = trim(row[cfg.col_res_h] || "");
        if (w !== "" && h !== "") return [w, h];
        var combined = trim(row[cfg.col_res] || "");
        if (combined !== "") {
            var parts = combined.split(/[xX,\s]+/);
            var clean = [];
            for (var i = 0; i < parts.length; i++) if (parts[i] !== "") clean.push(parts[i]);
            if (clean.length >= 2) return [clean[0], clean[1]];
        }
        return ["", ""];
    }

    function extractInfo(row, cfg) {
        function g(k) { return trim(row[cfg[k]] || ""); }
        var res = parseResolution(row, cfg);
        return {
            focal: g("col_focal"), start: g("col_start"), end: g("col_end"),
            fps: g("col_fps"), res_w: res[0], res_h: res[1], thumb: g("col_thumb")
        };
    }

    function detectShot() {
        var name = "";
        var ai = app.project.activeItem;
        if (ai && (ai instanceof CompItem)) name = ai.name;
        if (name === "" && app.project.file) name = app.project.file.name;
        var m = /(?:SHOT|SH)[ _\-]?(\d+)/i.exec(name);
        return m ? m[1] : "";
    }

    // ------------------------------------------------------------------ //
    // Thumbnail download + image sniff
    // ------------------------------------------------------------------ //
    function detectImageExt(f) {
        try {
            f.encoding = "BINARY"; f.open("r");
            var head = f.read(12); f.close();
            if (head.length < 4) return null;
            var b0 = head.charCodeAt(0), b1 = head.charCodeAt(1), b2 = head.charCodeAt(2);
            if (b0 === 0xFF && b1 === 0xD8 && b2 === 0xFF) return ".jpg";
            if (b0 === 0x89 && b1 === 0x50) return ".png";
            if (head.substr(0, 6) === "GIF87a" || head.substr(0, 6) === "GIF89a") return ".gif";
            if (head.substr(0, 2) === "BM") return ".bmp";
            if (head.substr(0, 4) === "RIFF") return ".webp";
            if ((b0 === 0x49 && b1 === 0x49) || (b0 === 0x4D && b1 === 0x4D)) return ".tif";
            return null;
        } catch (e) { return null; }
    }

    function downloadThumb(url, shot) {
        var real = directImageUrl(url);
        var safe = String(shot).replace(/[^A-Za-z0-9_\-]/g, "_");
        // Download to a neutral name, then name the file by its REAL format so
        // After Effects' importer (which keys off the extension) doesn't hit a
        // "bad header" on, e.g., a PNG served from a .jpg URL.
        var tmp = tempFile("jd_thumb_" + safe + ".download");
        if (!curlToFile(real, tmp)) return null;
        var ext = detectImageExt(tmp);
        if (!ext) return null;                    // not a recognisable image
        var finalFile = tempFile("jd_thumb_" + safe + ext);
        try { if (finalFile.exists) finalFile.remove(); } catch (e) {}
        if (tmp.rename(finalFile.name)) return finalFile.fsName;
        if (tmp.copy(finalFile.fsName)) return finalFile.fsName;
        return tmp.fsName;
    }

    // ------------------------------------------------------------------ //
    // After Effects actions
    // ------------------------------------------------------------------ //
    function activeComp() {
        var ai = app.project.activeItem;
        return (ai && (ai instanceof CompItem)) ? ai : null;
    }

    function importThumbToComp(path) {
        var comp = activeComp();
        if (!comp) return "No active comp to import into.";
        var src = new File(path);
        if (!src.exists) return "Thumbnail file not found.";

        // Import a fresh copy, not the file the preview is displaying, so any
        // handle ScriptUI holds on the original can't cause a read error.
        var ext = ""; var dot = path.lastIndexOf("."); if (dot >= 0) ext = path.substring(dot);
        var safe = String(LAST_SHOT || "shot").replace(/[^A-Za-z0-9_\-]/g, "_");
        var copyF = tempFile("jd_import_" + safe + ext);
        try { if (copyF.exists) copyF.remove(); } catch (e) {}
        var useFile = src;
        try { if (src.copy(copyF.fsName)) useFile = copyF; } catch (e) {}

        app.beginUndoGroup("Import Shot Thumbnail");
        try {
            var io = new ImportOptions(useFile);
            io.importAs = ImportAsType.FOOTAGE;
            var item = app.project.importFile(io);
            var layer = comp.layers.add(item);
            var fw = item.width, fh = item.height;
            if (fw > 0 && fh > 0) {
                var s = Math.min(comp.width / fw, comp.height / fh) * 100; // letterbox
                layer.property("Scale").setValue([s, s]);
            }
            layer.property("Position").setValue([comp.width / 2, comp.height / 2]);
            layer.name = "ShotThumbnail";
        } catch (e) {
            return "Import failed (" + e.toString() + "). File: " + useFile.fsName;
        }
        finally { app.endUndoGroup(); }
        return "Imported thumbnail into '" + comp.name + "'.";
    }

    function hudCompose(info, shot) {
        var parts = [];
        if (shot) parts.push("SH " + shot);
        if (info.focal) parts.push("Focal " + info.focal + "mm");
        if (info.start || info.end) parts.push((info.start || "?") + "-" + (info.end || "?"));
        if (info.fps) parts.push(info.fps + "fps");
        if (info.res_w && info.res_h) parts.push(info.res_w + "x" + info.res_h);
        return parts.join("   |   ");
    }

    function hudFind(comp) {
        for (var i = 1; i <= comp.numLayers; i++) {
            if (comp.layer(i).name === HUD_NAME) return comp.layer(i);
        }
        return null;
    }

    function hudSet(on, info, shot) {
        var comp = activeComp();
        if (!comp) return "No active comp for the HUD.";
        app.beginUndoGroup("Shot Info HUD");
        try {
            var existing = hudFind(comp);
            if (on) {
                var txt = hudCompose(info, shot);
                var layer = existing ? existing : comp.layers.addText(txt);
                layer.name = HUD_NAME;
                var srcProp = layer.property("Source Text");
                var td = srcProp.value;
                td.text = txt;
                td.fontSize = 28;
                td.applyFill = true;
                td.fillColor = [1, 1, 1];
                srcProp.setValue(td);
                layer.property("Position").setValue([40, comp.height - 40]);
            } else if (existing) {
                existing.remove();
            }
        } catch (e) { return "HUD error: " + e.toString(); }
        finally { app.endUndoGroup(); }
        return on ? "HUD on (text layer '" + HUD_NAME + "')." : "HUD off.";
    }

    function applyToComp(info) {
        var comp = activeComp();
        if (!comp) return "No active comp.";
        app.beginUndoGroup("Apply Shot to Comp");
        var applied = [];
        try {
            if (info.fps) {
                var fps = parseFloat(info.fps);
                if (fps > 0) { comp.frameRate = fps; applied.push("fps"); }
            }
            if (info.res_w && info.res_h) {
                var w = parseInt(info.res_w, 10), h = parseInt(info.res_h, 10);
                if (w > 0 && h > 0) { comp.width = w; comp.height = h; applied.push("resolution"); }
            }
            if (trim(info.start) !== "" && trim(info.end) !== "") {
                var s = parseFloat(info.start), e = parseFloat(info.end);
                var fr = comp.frameRate > 0 ? comp.frameRate : 24;
                if (!isNaN(s) && !isNaN(e) && e >= s) {
                    var startSec = s / fr;
                    var durSec = (e - s + 1) / fr;
                    if (startSec + durSec > comp.duration) comp.duration = startSec + durSec + (1 / fr);
                    comp.workAreaStart = startSec;
                    comp.workAreaDuration = durSec;
                    applied.push("work area");
                }
            }
        } catch (err) { return "Apply failed: " + err.toString(); }
        finally { app.endUndoGroup(); }
        return applied.length ? ("Applied to comp: " + applied.join(", ") + ".") : "Nothing to apply.";
    }

    // ================================================================== //
    // UI
    // ================================================================== //
    function buildUI(thisObj) {
        var win = (thisObj instanceof Panel)
            ? thisObj
            : new Window("palette", "Shot Info", undefined, { resizeable: true });
        win.orientation = "column";
        win.alignChildren = ["fill", "top"];
        win.spacing = 6;
        win.margins = 8;

        // ---- controls we reference later ----
        var ui = {};
        var CFG = loadCfg();   // working config (from app.settings)

        // Fetch row
        var fetchRow = win.add("group");
        fetchRow.alignment = ["fill", "top"];
        ui.fetchBtn = fetchRow.add("button", undefined, "Fetch Sheet");
        ui.fetchStatus = fetchRow.add("statictext", undefined, "Not fetched");
        ui.fetchStatus.alignment = ["fill", "center"];
        ui.configBtn = fetchRow.add("button", undefined, "Config\u2026");

        // Shot row
        var shotRow = win.add("group");
        shotRow.alignment = ["fill", "top"];
        shotRow.add("statictext", undefined, "Shot:");
        ui.shot = shotRow.add("edittext", undefined, "");
        ui.shot.characters = 8;
        ui.shot.alignment = ["fill", "center"];
        ui.autoBtn = shotRow.add("button", undefined, "AUTO");
        ui.queryBtn = shotRow.add("button", undefined, "Query");

        // (Thumbnail panel is created last, below, so it can't push the
        //  controls and buttons off-screen when the panel is short.)

        // Info fields
        var infoPanel = win.add("panel", undefined, "Shot Info");
        infoPanel.alignChildren = ["fill", "top"];
        infoPanel.margins = 8;
        function infoField(label) {
            var g = infoPanel.add("group");
            g.alignment = ["fill", "top"];
            var l = g.add("statictext", undefined, label);
            l.preferredSize.width = 90;
            var e = g.add("edittext", undefined, "");
            e.alignment = ["fill", "center"];
            return e;
        }
        ui.iFocal = infoField("Focal (mm):");
        var rangeGrp = infoPanel.add("group");
        rangeGrp.alignment = ["fill", "top"];
        var rl = rangeGrp.add("statictext", undefined, "Range:");
        rl.preferredSize.width = 90;
        ui.iStart = rangeGrp.add("edittext", undefined, ""); ui.iStart.characters = 7;
        rangeGrp.add("statictext", undefined, "-");
        ui.iEnd = rangeGrp.add("edittext", undefined, ""); ui.iEnd.characters = 7;
        ui.iFps = infoField("FPS:");
        var resGrp = infoPanel.add("group");
        resGrp.alignment = ["fill", "top"];
        var wl = resGrp.add("statictext", undefined, "Resolution:");
        wl.preferredSize.width = 90;
        ui.iW = resGrp.add("edittext", undefined, ""); ui.iW.characters = 7;
        resGrp.add("statictext", undefined, "x");
        ui.iH = resGrp.add("edittext", undefined, ""); ui.iH.characters = 7;

        // Actions
        var actRow = win.add("group");
        actRow.alignment = ["fill", "top"];
        ui.hudBtn = actRow.add("button", undefined, "Show HUD");
        ui.applyBtn = actRow.add("button", undefined, "Apply to Comp");

        // Status
        ui.status = win.add("statictext", undefined, "", { truncate: "end" });
        ui.status.alignment = ["fill", "top"];

        // Thumbnail (placed here, ABOVE the config section, so a collapsed
        // config panel can't reserve empty space above it). It fills the
        // leftover height; Import sits on top so it's never clipped.
        var thumbPanel = win.add("panel", undefined, "Thumbnail");
        thumbPanel.alignChildren = ["fill", "top"];
        thumbPanel.alignment = ["fill", "fill"];       // absorb spare vertical space
        thumbPanel.margins = 8;
        ui.importBtn = thumbPanel.add("button", undefined, "Import Thumbnail to Comp");
        ui.importBtn.alignment = ["fill", "top"];
        ui.importBtn.enabled = false;
        ui.thumbPlaceholder = thumbPanel.add("statictext", undefined, "No thumbnail");
        ui.thumbPlaceholder.alignment = ["fill", "top"];
        // Preview box: fills to track the panel, with a usable minimum height so
        // it stays legible at the panel's default size. onDraw scales the image
        // to fit, because ScriptUI 'image' controls render at native size.
        ui.thumbPreview = thumbPanel.add("iconbutton", undefined, undefined, { style: "toolbutton" });
        ui.thumbPreview.alignment = ["fill", "fill"];
        ui.thumbPreview.preferredSize = [220, 180];
        ui.thumbPreview.minimumSize = [80, 160];       // floor so it never gets tiny
        ui.thumbPreview.previewImg = null;
        ui.thumbPreview.visible = false;
        ui.thumbPreview.onDraw = function () {
            var g = this.graphics;
            var W = this.size.width, H = this.size.height;
            try {
                var bg = g.newBrush(g.BrushType.SOLID_COLOR, [0.14, 0.14, 0.15, 1]);
                g.newPath(); g.rectPath(0, 0, W, H); g.fillPath(bg);
            } catch (e) {}
            var img = this.previewImg;
            if (!img) return;
            var iw = img.size[0], ih = img.size[1];
            if (iw <= 0 || ih <= 0) return;
            var sc = Math.min(W / iw, H / ih);
            if (sc > 1) sc = 1;                 // never upscale past native
            var w = iw * sc, h = ih * sc;
            try { g.drawImage(img, (W - w) / 2, (H - h) / 2, w, h); } catch (e) {}
        };

        // (Configuration lives in a separate modal dialog opened by the
        //  "Config..." button, so it never reserves space in this panel.)


        // ---------------------------------------------------------------- //
        // UI helpers
        // ---------------------------------------------------------------- //
        function setStatus(msg) { ui.status.text = msg; }

        function relayout() {
            try { win.layout.layout(true); win.layout.resize(); } catch (e) {}
        }

        function fillInfo(info) {
            ui.iFocal.text = info.focal || "";
            ui.iStart.text = info.start || "";
            ui.iEnd.text = info.end || "";
            ui.iFps.text = info.fps || "";
            ui.iW.text = info.res_w || "";
            ui.iH.text = info.res_h || "";
        }
        function clearInfo() { fillInfo({ focal: "", start: "", end: "", fps: "", res_w: "", res_h: "" }); }

        function setThumbnail(path) {
            if (path) {
                var img = null;
                try { img = ScriptUI.newImage(File(path)); }
                catch (e1) { try { img = ScriptUI.newImage(path); } catch (e2) { img = null; } }
                if (img) {
                    ui.thumbPreview.previewImg = img;
                    ui.thumbPreview.visible = true;
                    ui.thumbPlaceholder.visible = false;
                    ui.importBtn.enabled = true;
                } else {
                    ui.thumbPreview.previewImg = null;
                    ui.thumbPreview.visible = false;
                    ui.thumbPlaceholder.text = "Preview unavailable (can still import)";
                    ui.thumbPlaceholder.visible = true;
                    ui.importBtn.enabled = true;
                }
            } else {
                ui.thumbPreview.previewImg = null;
                ui.thumbPreview.visible = false;
                ui.thumbPlaceholder.text = "No thumbnail";
                ui.thumbPlaceholder.visible = true;
                ui.importBtn.enabled = false;
            }
            relayout();
            try { ui.thumbPreview.notify("onDraw"); } catch (e) {}
        }

        // ---------------------------------------------------------------- //
        // Actions
        // ---------------------------------------------------------------- //
        function doFetch() {
            var cfg = CFG;
            ui.fetchStatus.text = "Fetching...";
            var res = fetchSheet(cfg);
            if (!res.ok) {
                CACHE = null;
                ui.fetchStatus.text = "Not fetched";
                setStatus(res.error);
                return;
            }
            CACHE = { headers: res.headers, data: res.data };
            ui.fetchStatus.text = res.data.length + " shots loaded";
            setStatus("Sheet fetched. Lookups now run locally.");
            if (trim(ui.shot.text) !== "") doQuery(ui.shot.text);
        }

        function doQuery(shot) {
            shot = trim(shot);
            if (shot === "") { setStatus("Enter a shot number (or use AUTO)."); return; }
            if (!CACHE) { setStatus("No sheet data yet - press 'Fetch Sheet' first."); return; }
            var cfg = CFG;
            var row = findShotRow(CACHE.data, cfg, shot);
            if (!row) { clearInfo(); setThumbnail(null); setStatus("Shot '" + shot + "' not found in the sheet."); return; }
            var info = extractInfo(row, cfg);
            LAST_INFO = info; LAST_SHOT = shot;
            fillInfo(info);
            var turl = trim(row[cfg.col_thumb] || "");
            if (turl !== "") {
                var p = downloadThumb(turl, shot);
                if (p) { THUMB_PATH = p; setThumbnail(p); }
                else { THUMB_PATH = ""; setThumbnail(null); setStatus("Loaded shot " + shot + " (thumbnail failed to load)."); return; }
            } else { THUMB_PATH = ""; setThumbnail(null); }
            if (HUD_ON) hudSet(true, info, shot);
            setStatus("Loaded shot " + shot + ".");
        }

        // ---------------------------------------------------------------- //
        // Wiring
        // ---------------------------------------------------------------- //
        ui.fetchBtn.onClick = function () { doFetch(); };
        ui.queryBtn.onClick = function () { doQuery(ui.shot.text); };
        ui.shot.onEnterKey = function () { doQuery(ui.shot.text); };

        ui.autoBtn.onClick = function () {
            var s = detectShot();
            if (s === "") { setStatus("No SH/SHOT number in the active comp or project name."); return; }
            ui.shot.text = s;
            doQuery(s);
        };

        ui.importBtn.onClick = function () {
            if (!THUMB_PATH) { setStatus("No thumbnail to import."); return; }
            setStatus(importThumbToComp(THUMB_PATH));
        };

        ui.hudBtn.onClick = function () {
            HUD_ON = !HUD_ON;
            var msg = hudSet(HUD_ON, LAST_INFO || {}, LAST_SHOT);
            ui.hudBtn.text = HUD_ON ? "Hide HUD" : "Show HUD";
            setStatus(msg);
        };

        ui.applyBtn.onClick = function () {
            if (!LAST_INFO) { setStatus("Query a shot first."); return; }
            setStatus(applyToComp(LAST_INFO));
        };

        ui.configBtn.onClick = function () { openConfigDialog(); };

        // ---------------------------------------------------------------- //
        // Configuration dialog (modal) -- keeps the main panel compact
        // ---------------------------------------------------------------- //
        function openConfigDialog() {
            var dlg = new Window("dialog", "Shot Info - Configuration");
            dlg.orientation = "column";
            dlg.alignChildren = ["fill", "top"];
            dlg.margins = 12;
            dlg.spacing = 8;

            var fields = {};
            function dlgField(parent, label, value) {
                var g = parent.add("group");
                g.alignment = ["fill", "top"];
                var l = g.add("statictext", undefined, label);
                l.preferredSize.width = 150;
                var e = g.add("edittext", undefined, value || "");
                e.characters = 34;
                e.alignment = ["fill", "center"];
                return e;
            }

            var srcP = dlg.add("panel", undefined, "Source");
            srcP.alignChildren = ["fill", "top"]; srcP.margins = 10;
            fields.url = dlgField(srcP, "Spreadsheet URL:", CFG.url);
            fields.gid = dlgField(srcP, "Sheet gid:", CFG.gid);

            var mapP = dlg.add("panel", undefined, "Column mapping (your header names)");
            mapP.alignChildren = ["fill", "top"]; mapP.margins = 10;
            for (var i = 0; i < FIELD_DEFS.length; i++) {
                var key = FIELD_DEFS[i][0], lbl = FIELD_DEFS[i][1], req = FIELD_DEFS[i][2];
                fields[key] = dlgField(mapP, lbl + (req ? " *" : ""), CFG[key]);
            }

            function dlgCfg() {
                var c = {};
                for (var j = 0; j < CONFIG_KEYS.length; j++) {
                    c[CONFIG_KEYS[j]] = trim(fields[CONFIG_KEYS[j]].text);
                }
                return c;
            }

            var sanityRow = dlg.add("group");
            sanityRow.alignment = ["fill", "top"];
            var sanityBtn = sanityRow.add("button", undefined, "Run Sanity Check");
            var dlgStatus = dlg.add("statictext", undefined, "", { multiline: true });
            dlgStatus.preferredSize = [440, 56];
            sanityBtn.onClick = function () {
                dlgStatus.text = "Checking...";
                var problems = sanityCheck(dlgCfg());
                dlgStatus.text = (problems.length === 0)
                    ? "All good. Sheet reachable and every mapped column found."
                    : "- " + problems.join("\n- ");
            };

            var warn = dlg.add("statictext", undefined, "Reset restores every field to the script defaults.");
            try {
                warn.graphics.foregroundColor =
                    warn.graphics.newPen(warn.graphics.PenType.SOLID_COLOR, [0.86, 0.30, 0.25, 1], 1);
            } catch (e) {}
            var resetRow = dlg.add("group");
            resetRow.alignment = ["left", "top"];
            var resetBtn = resetRow.add("button", undefined, "Reset to Defaults");
            resetBtn.onClick = function () {
                if (!confirm("Reset all parameters to script defaults?\nThis clears the URL, gid and column mapping.")) return;
                for (var j = 0; j < CONFIG_KEYS.length; j++) fields[CONFIG_KEYS[j]].text = DEFAULTS[CONFIG_KEYS[j]];
                dlgStatus.text = "Fields reset to defaults (press Save to apply).";
            };

            var btnRow = dlg.add("group");
            btnRow.alignment = ["right", "top"];
            var cancelBtn = btnRow.add("button", undefined, "Cancel", { name: "cancel" });
            var okBtn = btnRow.add("button", undefined, "Save", { name: "ok" });
            okBtn.onClick = function () { dlg.close(1); };
            cancelBtn.onClick = function () { dlg.close(0); };

            if (dlg.show() === 1) {
                var newCfg = dlgCfg();
                var changed = false;
                for (var k = 0; k < CONFIG_KEYS.length; k++) {
                    var kk = CONFIG_KEYS[k];
                    if (CFG[kk] !== newCfg[kk]) changed = true;
                    CFG[kk] = newCfg[kk];
                    saveCfgKey(kk, newCfg[kk]);
                }
                if (changed) {
                    CACHE = null;                       // source changed -> stale
                    ui.fetchStatus.text = "Not fetched";
                    setStatus("Config saved. Press 'Fetch Sheet' to reload.");
                } else {
                    setStatus("Config saved.");
                }
            }
        }

        function sanityCheck(cfg) {
            var problems = [];
            if (trim(cfg.url) === "") return ["No spreadsheet URL set."];
            if (!extractId(cfg.url)) problems.push("Could not parse a spreadsheet ID from the URL.");
            if (trim(cfg.col_shot) === "") problems.push("Required field 'Shot Number' is empty.");
            var res = fetchSheet(cfg);
            if (!res.ok) { problems.push(res.error); return problems; }
            if (res.data.length === 0) problems.push("The sheet has headers but no data rows.");
            for (var i = 0; i < FIELD_DEFS.length; i++) {
                var key = FIELD_DEFS[i][0], lbl = FIELD_DEFS[i][1];
                var col = trim(cfg[key]);
                if (col !== "") {
                    var found = false;
                    for (var h = 0; h < res.headers.length; h++) { if (res.headers[h] === col) { found = true; break; } }
                    if (!found) problems.push("Column '" + col + "' (" + lbl + ") not found.");
                }
            }
            return problems;
        }

        // ---- init ----
        // CFG is loaded from app.settings above; config is edited via the dialog.

        win.onResizing = win.onResize = function () { relayout(); };
        if (!(thisObj instanceof Panel)) { win.center(); win.show(); }
        else { win.layout.layout(true); }

        return win;
    }

    buildUI(thisObj);

})(this);
