/*
 * jd_EXRCompiler panel 0.3 test projects (procedural only: solids, shapes and a testmatte EXR).
 * Run once in a SECOND After Effects instance whose project is empty and untitled:
 *   File > Scripts > Run Script File... > this file
 * It refuses to run in any other project. It writes only into
 *   jd_EXRCompiler/test-data/_generated/panel/rq/   (git-ignored):
 *   rq_unmanaged.aep      no working space, 32 bpc
 *   rq_workingspace.aep   working space sRGB IEC61966-2.1, 32 bpc
 *   rq_ocio.aep           OCIO with ocio/config.ocio (a copy of panel/tests/ocio_test_config.ocio)
 *   builder_notes.txt     what was set, what needs doing by hand, AE's colour properties
 * It never touches AE preferences, templates or any other folder.
 *
 * Comp "layers_test" (128x64, frames 1001-1010, 25 fps), top to bottom:
 *   disc     shape layer, ellipse 40x40 at (44,32)      -> matte from alpha
 *   box      shape layer, rectangle 40x30 at (70,32)    -> matte from alpha (overlaps disc)
 *   rgbmatte precomp: red / green / blue blocks          -> RGB split (R, G, B)
 *   lumaclip testmatte EXR footage (no alpha), 200 %     -> matte from luminance
 *   spec     solid (0.1, 0.2, 0.3)                       -> pass
 *   beauty   solid (0.5, 0.25, 0.125)                    -> pass, the main layer
 */
(function () {
    var p = app.project;
    if (!p || p.file !== null || p.numItems !== 0) {
        alert("Run this only in a new, empty, untitled project (a second AE instance). Nothing was changed.");
        return;
    }
    var here = new File($.fileName).parent;                                   // panel/tests
    var rq = new Folder(here.fsName + "/../../test-data/_generated/panel/rq");
    var ocioDir = new Folder(rq.fsName + "/ocio");
    if (!rq.exists && !rq.create()) { alert("Cannot create " + rq.fsName); return; }
    if (!ocioDir.exists) { ocioDir.create(); }
    var notes = [];
    var W = 128, H = 64, FPS = 25, N = 10;

    function propsMatching(obj, re) {
        var out = [], i, list;
        try { list = obj.reflect.properties; } catch (e) { return "(no reflect)"; }
        for (i = 0; i < list.length; i++) { if (re.test(list[i].name)) { out.push(list[i].name); } }
        return out.join(", ");
    }

    function trySet(label, fn) {
        try { fn(); notes.push("ok    " + label); return true; }
        catch (e) { notes.push("FAIL  " + label + ": " + e.message); return false; }
    }

    function buildContent() {
        var comp = p.items.addComp("layers_test", W, H, 1, N / FPS, FPS);
        comp.displayStartTime = 1001 / FPS;
        comp.bgColor = [0, 0, 0];
        comp.layers.addSolid([0.5, 0.25, 0.125], "beauty", W, H, 1, N / FPS);
        comp.layers.addSolid([0.1, 0.2, 0.3], "spec", W, H, 1, N / FPS);

        var exr = new File(here.fsName + "/../../test-data/_generated/panel/mattes/rgbmatte.1001.exr");
        if (exr.exists) {
            var io = new ImportOptions(exr);
            io.sequence = true;
            var fl = comp.layers.add(p.importFile(io));
            fl.name = "lumaclip";
            fl.property("ADBE Transform Group").property("ADBE Scale").setValue([200, 200]);
        } else {
            notes.push("lumaclip skipped: " + exr.fsName + " not found (run jd_exrc testmatte first)");
        }

        var rgb = p.items.addComp("rgbmatte_src", W, H, 1, N / FPS, FPS);
        rgb.displayStartTime = 1001 / FPS;
        var cols = [[1, 0, 0], [0, 1, 0], [0, 0, 1]], names = ["red", "green", "blue"], i;
        for (i = 0; i < 3; i++) {
            var sl = rgb.layers.addSolid(cols[i], names[i], 36, 48, 1, N / FPS);
            sl.property("ADBE Transform Group").property("ADBE Position").setValue([22 + i * 42, 32]);
        }
        comp.layers.add(rgb).name = "rgbmatte";

        var shape = function (name, kind, size, pos) {
            var l = comp.layers.addShape();
            l.name = name;
            var grp = l.property("ADBE Root Vectors Group").addProperty("ADBE Vector Group");
            var c = grp.property("ADBE Vectors Group");
            var s = c.addProperty(kind === "ellipse" ? "ADBE Vector Shape - Ellipse" : "ADBE Vector Shape - Rect");
            s.property(kind === "ellipse" ? "ADBE Vector Ellipse Size" : "ADBE Vector Rect Size").setValue(size);
            c.addProperty("ADBE Vector Graphic - Fill").property("ADBE Vector Fill Color").setValue([1, 1, 1]);
            l.property("ADBE Transform Group").property("ADBE Position").setValue(pos);
            return l;
        };
        shape("box", "rect", [40, 30], [70, 32]);
        shape("disc", "ellipse", [40, 40], [44, 32]);
        return comp;
    }

    try {
        notes.push("AE " + app.version + ", project colour properties: " + propsMatching(p, /color|colour|ocio|working|linear|bits/i));
        buildContent();
        p.bitsPerChannel = 32;

        // 1) unmanaged
        // A new project is unmanaged already; setting "None" by script pops a modal "Profile None is missing".
        notes.push("info  working space of the new project: \"" + (function () { try { return p.workingSpace; } catch (e) { return "?"; } })() + "\" (left as is)");
        trySet("save rq_unmanaged.aep", function () { p.save(new File(rq.fsName + "/rq_unmanaged.aep")); });

        // 2) working space
        trySet("working space = sRGB IEC61966-2.1", function () { p.workingSpace = "sRGB IEC61966-2.1"; });
        trySet("save rq_workingspace.aep", function () { p.save(new File(rq.fsName + "/rq_workingspace.aep")); });

        // 3) OCIO with the test config (copied next to the projects; per project, never in prefs)
        var src = new File(here.fsName + "/ocio_test_config.ocio"), cfg = new File(ocioDir.fsName + "/config.ocio");
        if (cfg.exists) { cfg.remove(); }
        if (!src.copy(cfg.fsName)) { notes.push("FAIL  copy OCIO config to " + cfg.fsName); }
        var ocioOk = trySet("colour engine = OCIO", function () { p.colorManagementSystem = 1; });
        var cfgOk = trySet("OCIO config file = " + cfg.fsName, function () { p.ocioConfigurationFile = cfg.fsName; });
        trySet("working colour space = Linear Rec.709", function () { p.workingColorSpace = "Linear Rec.709"; });
        if (!ocioOk || !cfgOk) {
            notes.push("BY HAND in rq_ocio.aep: Project Settings > Color > Color Engine: OCIO, config file: " + cfg.fsName +
                       ", working space: Linear Rec.709. Save.");
        }
        trySet("save rq_ocio.aep", function () { p.save(new File(rq.fsName + "/rq_ocio.aep")); });
        notes.push("NOTE  AE remembers the last project colour settings in its prefs (Prefs-indep-general.txt: " +
                   "colorManagementSystem + the OCIO config path). Restore the prefs snapshot after this run.");
    } catch (err) {
        notes.push("ERROR " + err.message + " (line " + err.line + ")");
    }

    var nf = new File(rq.fsName + "/builder_notes.txt");
    nf.encoding = "UTF-8";
    if (nf.open("w")) { nf.write(notes.join("\n") + "\n"); nf.close(); }
    alert("Test projects built in\n" + rq.fsName + "\n\n" + notes.join("\n"));
})();
