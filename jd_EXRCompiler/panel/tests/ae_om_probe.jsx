/*
 * jd_EXRCompiler output-module probe v4: how can a script get an OpenEXR (or float) output module in AE 2026?
 * Run in an EMPTY UNTITLED project (it refuses otherwise), with or without UI:
 *   File > Scripts > Run Script File... > this file      (or headless: AfterFX.exe -m -r this file)
 * Makes one 16x16 temp comp + queue item, tries every route below, writes
 *   jd_EXRCompiler/test-data/_generated/panel/om_probe.txt
 * and removes the item and comp. Saves nothing, creates no template (prefs untouched). Renders ONE 16x16
 * frame (route F) into %TEMP%\jd_EXRCompiler and deletes it after reading its header.
 * No alert when AE runs without UI (app.isUISuppressed) or when JD_EXRC_HEADLESS is set in the environment.
 *
 * Routes:
 *   A  setSettings Format as the SPEC int (8) and as strings
 *   B  Format together with "Output File Info" (file name .exr) in one call
 *   C  om.file = *.exr first, then Format
 *   D  every OM template applied (read-only use): resulting Format / Depth / Channels, allowed Depth values
 *   E  "TIFF Sequence with Alpha" + Depth "Floating Point+" / 128 (RGB + Alpha), "Floating Point" / 96 (RGB)
 *   F  render one frame with route E's settings, read the TIFF's BitsPerSample / SampleFormat (32 / 3 = float)
 * Summary lines start with "RESULT".
 */
(function () {
    var p = app.project;
    var headless = false;
    try { headless = !!app.isUISuppressed; } catch (e0) { headless = false; }
    try { if ($.getenv("JD_EXRC_HEADLESS")) { headless = true; } } catch (e1) { /* no env */ }
    var here = new File($.fileName).parent;
    var reportFile = new File(here.fsName + "/../../test-data/_generated/panel/om_probe.txt");
    var out = ["jd_EXRCompiler OM probe v2  " + new Date().toString() + "  AE " + app.version + (headless ? "  (headless)" : "")];
    var finish = function () {
        reportFile.encoding = "UTF-8";
        if (reportFile.open("w")) { reportFile.write(out.join("\n") + "\n"); reportFile.close(); }
        if (!headless) { alert("OM probe written to\n" + reportFile.fsName); }
    };
    if (!p || p.file !== null || p.numItems !== 0) { out.push("REFUSED: project not empty/untitled"); finish(); return; }

    var J = function (o) { try { return o.toSource(); } catch (e) { return String(o); } };
    var S = function (om) {
        try {
            var s = om.getSettings(GetSettingsFormat.STRING);
            return "Format=" + s.Format + "  Depth=" + s.Depth + "  Channels=" + s.Channels + "  Color=" + s.Color;   // top-level keys
        } catch (e) { return "(getSettings: " + e.message + ")"; }
    };
    var tryIt = function (label, om, obj) {
        try { om.setSettings(obj); var r = S(om); out.push("ok   " + label + " " + J(obj) + "  ->  " + r); if (/exr/i.test(r)) { scriptable = true; } return true; }
        catch (e) { out.push("no   " + label + " " + J(obj) + "  (" + e.message + ")"); return false; }
    };
    // Minimal TIFF header reader: first IFD's BitsPerSample (258), SamplesPerPixel (277),
    // SampleFormat (339: 1 uint, 3 IEEE float), Compression (259), width/height.
    var tiffInfo = function (f) {
        f.encoding = "BINARY";
        f.open("r");
        var b = f.read(65536);
        f.close();
        var le = b.substr(0, 2) === "II";
        var u16 = function (o) { var a = b.charCodeAt(o), c = b.charCodeAt(o + 1); return le ? (a | (c << 8)) : ((a << 8) | c); };
        var u32 = function (o) { return le ? ((u16(o) + u16(o + 2) * 65536) >>> 0) : ((u16(o) * 65536 + u16(o + 2)) >>> 0); };
        var r = { bits: null, format: 1, spp: null, text: "byte order " + b.substr(0, 2) + ", magic " + u16(2) };
        var ifd = u32(4), n = u16(ifd), k, tag, type, count, val, first;
        for (k = 0; k < n; k++) {
            var e = ifd + 2 + k * 12;
            tag = u16(e); type = u16(e + 2); count = u32(e + 4);
            // SHORT values: inline when they fit in 4 bytes, else at the offset
            first = (type === 3) ? ((count <= 2) ? u16(e + 8) : u16(u32(e + 8))) : u32(e + 8);
            if (tag === 256 || tag === 257 || tag === 258 || tag === 259 || tag === 277 || tag === 339) {
                r.text += ", tag " + tag + "=" + first + (count > 1 ? " (x" + count + ")" : "");
            }
            if (tag === 258) { r.bits = first; }
            if (tag === 277) { r.spp = first; }
            if (tag === 339) { r.format = first; }
        }
        return r;
    };
    var tmpDir = new Folder(Folder.temp.fsName + "/jd_EXRCompiler");
    if (!tmpDir.exists) { tmpDir.create(); }
    var exrPath = tmpDir.fsName + "/om_probe_[#####].exr";

    var comp = null, item = null, i, om, scriptable = false;
    try {
        comp = p.items.addComp("om_probe", 16, 16, 1, 2 / 25, 25);
        item = p.renderQueue.items.add(comp);
        om = item.outputModule(1);
        var defaultTpl = null;
        out.push("start: " + S(om));

        // A
        out.push("--- A: Format by value");
        tryIt("A", om, { "Format": 8 });
        tryIt("A", om, { "Format": "8" });
        tryIt("A", om, { "Format": "OpenEXR Sequence" });
        // B
        out.push("--- B: Format + Output File Info together");
        tryIt("B", om, { "Format": "OpenEXR Sequence", "Output File Info": { "File Name": "om_probe_[#####].exr", "Base Path": tmpDir.fsName } });
        tryIt("B", om, { "Format": 8, "Output File Info": { "File Name": "om_probe_[#####].exr", "Base Path": tmpDir.fsName } });
        // C
        out.push("--- C: file first, then Format");
        try { om.file = new File(exrPath); out.push("ok   C om.file = " + exrPath + "  ->  " + S(om)); }
        catch (eC) { out.push("no   C om.file (" + eC.message + ")"); }
        tryIt("C", om, { "Format": "OpenEXR Sequence" });
        tryIt("C", om, { "Format": 8 });
        // D
        out.push("--- D: built-in templates (applied to this temp item only)");
        var tpls = om.templates, found = [];
        for (i = 0; i < tpls.length; i++) {
            try {
                om.applyTemplate(tpls[i]);
                var before = S(om);
                var depthOk = false;
                var allowed = "";
                try { var sp = om.getSettings(GetSettingsFormat.SPEC); allowed = "  allowed Depth: " + (sp.Depth && sp.Depth.enums ? sp.Depth.enums.toSource() : "?"); } catch (eS) { allowed = ""; }
                try { om.setSettings({ "Depth": "Floating Point" }); depthOk = true; } catch (eD) { depthOk = false; }
                try { om.setSettings({ "Channels": "RGB + Alpha" }); } catch (eCh) { /* not for every format */ }
                var after = S(om);
                out.push("tpl  " + tpls[i] + "  :  " + before + (depthOk ? "   | after Depth=Floating Point + RGB + Alpha: " + after : "   | Depth Floating Point refused") + allowed);
                if (/exr/i.test(after)) { found.push("EXR via template \"" + tpls[i] + "\""); }
                if (/Depth=Floating/i.test(after) && /tiff/i.test(after)) { found.push("float TIFF via template \"" + tpls[i] + "\""); }
            } catch (eT) { out.push("tpl  " + tpls[i] + "  : applyTemplate failed (" + eT.message + ")"); }
        }
        // E (v4): float TIFF. AE lists Depth "Floating Point+" (128, with alpha) and "Floating Point" (96, RGB only).
        out.push("--- E: TIFF Sequence with Alpha + float depth (each try starts from the template again)");
        var floatTiff = [];
        var E = [
            ["RGB + Alpha, Depth by name", [{ "Channels": "RGB + Alpha" }, { "Depth": "Floating Point+" }]],
            ["RGB + Alpha, Depth by int 128", [{ "Channels": "RGB + Alpha" }, { "Depth": 128 }]],
            ["both in one call", [{ "Channels": "RGB + Alpha", "Depth": "Floating Point+" }]],
            ["Depth first, then Channels", [{ "Depth": "Floating Point+" }, { "Channels": "RGB + Alpha" }]],
            ["RGB only, Floating Point", [{ "Channels": "RGB" }, { "Depth": "Floating Point" }]],
            ["RGB only, int 96", [{ "Channels": "RGB" }, { "Depth": 96 }]],
            ["Trillions of Colors+ (16-bit int, reference)", [{ "Channels": "RGB + Alpha" }, { "Depth": "Trillions of Colors+" }]]
        ];
        for (i = 0; i < E.length; i++) {
            try { om.applyTemplate("TIFF Sequence with Alpha"); } catch (eA) { out.push("E   applyTemplate TIFF Sequence with Alpha failed: " + eA.message); break; }
            var allOk = true;
            for (var e2 = 0; e2 < E[i][1].length; e2++) { if (!tryIt("E " + E[i][0], om, E[i][1][e2])) { allOk = false; } }
            var res = S(om);
            out.push("E   " + E[i][0] + "  ->  " + res);
            if (allOk && /Depth=Floating Point/.test(res)) { floatTiff.push(E[i][0] + " (" + res + ")"); }
        }
        out.push("RESULT float TIFF by script (E): " + (floatTiff.length ? "YES: " + floatTiff.join("; ") : "no"));

        // F (v4): render ONE 16x16 frame with TIFF Sequence with Alpha + RGB + Alpha + Floating Point+ (or 128)
        // into %TEMP%\jd_EXRCompiler, read the TIFF header back, delete the file.
        out.push("--- F: render one frame, read the TIFF header");
        try {
            om.applyTemplate("TIFF Sequence with Alpha");
            tryIt("F", om, { "Channels": "RGB + Alpha" });
            if (!tryIt("F", om, { "Depth": "Floating Point+" })) { tryIt("F", om, { "Depth": 128 }); }
            tryIt("F RS", item, { "Color Depth": "32 bits per channel" });
            item.timeSpanStart = 0;
            item.timeSpanDuration = comp.frameDuration;
            om.file = new File(tmpDir.fsName + "/om_probe_tiff_[#####].tif");
            out.push("F   before render: " + S(om));
            var t0 = new Date().getTime();
            p.renderQueue.render();
            out.push("F   render: " + (new Date().getTime() - t0) + " ms, status " + item.status);
            var tifs = tmpDir.getFiles("om_probe_tiff_*.tif");
            if (!tifs.length) { out.push("RESULT render float TIFF: no file written"); }
            else {
                out.push("F   file: " + tifs[0].fsName + "  " + tifs[0].length + " bytes");
                var info = tiffInfo(tifs[0]);
                out.push("F   TIFF: " + info.text);
                out.push("RESULT render float TIFF: " + (info.bits === 32 && info.format === 3 ? "YES, 32-bit IEEE float, " + info.spp + " samples per pixel" :
                                                       "no (BitsPerSample " + info.bits + ", SampleFormat " + info.format + ")"));
                for (var t = 0; t < tifs.length; t++) { try { tifs[t].remove(); } catch (eRm) { /* temp */ } }
            }
        } catch (eF) { out.push("F   ERROR " + eF.message + " (line " + eF.line + ")"); }
        out.push("RESULT OpenEXR settable by script (A/B/C): " + (scriptable ? "YES (see ok lines)" : "no"));
        out.push("RESULT routes through built-in templates: " + (found.length ? found.join("; ") : "none"));
    } catch (err) {
        out.push("ERROR " + err.message + " (line " + err.line + ")");
    } finally {
        try { if (item) { item.remove(); } } catch (e2) { out.push("could not remove the queue item: " + e2.message); }
        try { if (comp) { comp.remove(); } } catch (e3) { out.push("could not remove the comp: " + e3.message); }
        finish();
    }
})();
