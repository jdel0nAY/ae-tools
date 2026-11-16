/*
 * jd_EXRCompiler launcher probe: does jd_exrc really run (and report) from AE's system.callSystem?
 * Run in the tester's second AE instance: File > Scripts > Run Script File... > this file.
 * Uses the panel's own code (runCli: a .bat run with cmd /c) from ../jd_EXRCompiler.jsx, builds no UI,
 * touches no project and no prefs. Writes test-data/_generated/panel/launcher_probe.txt and shows it.
 *
 * PASS = "[bat] crypto --check: exit 0 ... events: start(...) frame(ok) check(PASS) done".
 */
(function () {
    var here = new File($.fileName).parent;
    var out = [], t0, res, headless = false;
    try { headless = !!app.isUISuppressed; } catch (e0) { headless = false; }
    try { if ($.getenv("JD_EXRC_HEADLESS")) { headless = true; } } catch (e1) { /* no env */ }
    var say = function (t) { if (!headless) { alert(t); } };   // headless AE never closes an alert
    out.push("jd_EXRCompiler launcher probe  " + new Date().toString() + "  AE " + app.version);

    // 0) raw callSystem: does it return cmd's own echo?
    t0 = new Date().getTime();
    var raw = system.callSystem("cmd /c echo hello-from-cmd");
    out.push("raw cmd echo (" + (new Date().getTime() - t0) + " ms): " + raw.replace(/\s+$/, ""));

    $.global.JD_EXRC_PROBE = true;
    var P;
    try { P = $.evalFile(new File(here.fsName + "/../jd_EXRCompiler.jsx")); }
    finally { $.global.JD_EXRC_PROBE = false; }
    if (!P || !P.runCli) { say("Could not load the panel code."); return; }
    out.push("panel " + P.core.VERSION);

    var exe = P.findExe();
    out.push("jd_exrc: " + (exe ? exe.fsName : "NOT FOUND"));
    if (!exe) { say(out.join("\n")); return; }

    var sample = new File(here.fsName + "/../../test-data/_generated/panel/mattes/rgbmatte.1001.exr");
    // The same kind of call Compile makes: build a Cryptomatte with --json-log --check.
    // (--version and info print to stdout only, not to --log-file, in jd_exrc 0.1.0.)
    if (sample.exists) {
        var outExr = P.tempFolder().fsName + "/probe_crypto.exr";
        res = P.runCli(exe, ["crypto", "-o", outExr, "--matte", "a,b,c=" + sample.fsName + ":RGB", "--json-log", "--check"]);
        var ev = [], k;
        for (k = 0; k < res.events.length; k++) { ev.push(res.events[k].event + (res.events[k].msg ? "(" + res.events[k].msg + ")" : "")); }
        out.push("[bat] crypto --check: exit " + res.exit + ", " + res.ms + " ms, events: " + (ev.length ? ev.join(" ") : "(none)"));
        try { new File(outExr).remove(); } catch (eR) { /* temp */ }
    } else {
        out.push("(sample EXR missing: " + sample.fsName + ")");
    }

    // For the record: calling the exe directly (no cmd, no file), what does callSystem return?
    t0 = new Date().getTime();
    var direct = system.callSystem("\"" + exe.fsName + "\" --version");
    out.push("direct exe call (" + (new Date().getTime() - t0) + " ms): " + String(direct).replace(/\s+$/, "") || "(empty)");
    out.push("details (each call, kept files on failure): " + P.tempFolder().fsName + "\\last_compile.log");

    var rep = new File(here.fsName + "/../../test-data/_generated/panel/launcher_probe.txt");
    rep.encoding = "UTF-8";
    if (rep.open("w")) { rep.write(out.join("\n") + "\n"); rep.close(); }
    say(out.join("\n"));
})();
