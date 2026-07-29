/**
 * jd_AOVcontactsheet_v1_0_0.jsx
 *
 * Builds a contact sheet comp named  <name>_AOVs_contactsheet  exposing every
 * AOV of a render, in either of two modes depending on what is selected.
 *
 * MODE 1 - LAYERS (one multi-layer EXR selected)
 *   Reads the channel list out of the EXR header, groups the channels into
 *   AOVs, and applies one animation preset per AOV.
 *
 *   Multi-part files carry a part name on every channel, so an AOV would read as
 *   "subimage03.DiffuseLighting". That prefix is stripped from the cell name -
 *   unless doing so would make two AOVs collide - which keeps labels readable
 *   and lets a preset named after the AOV alone match. The full channel name is
 *   recorded in each layer's comment.
 *
 *   EXtractoR keeps its channel selection in a custom-UI parameter that reports
 *   PropertyValueType.NO_VALUE. After Effects does not expose custom-value
 *   properties to scripting, so a script cannot set those dropdowns - not by
 *   name, not by index, not through the arbitrary data. An .ffx preset carries
 *   that data, and applyPreset() IS scriptable, so a library of hand-made
 *   presets is the way to automate this.
 *
 * MODE 2 - FILES (several files selected, or one non-EXR)
 *   Assumes each selected sequence is one AOV written to its own file. Any
 *   footage After Effects can read works here, not just EXR - PNG, TIFF, DPX,
 *   JPEG and so on are laid out natively with no preset involved.
 *
 *   The AOV name is read out of the filename by removing the whole names the
 *   filenames share, so shot_v03_diffuse.1001.exr / shot_v03_direct.1001.exr
 *   give "diffuse" / "direct". Comparison is by whole name token, never by
 *   character - a character-wise comparison would call the "di" in
 *   diffuse/direct shared and mangle both. Four other rules are available in
 *   the config panel, including a custom regex.
 *
 *   Each row has a Preset checkbox. It ticks itself when a matching .ffx is
 *   found, and can be forced on for any EXR row - which is how a separate
 *   Cryptomatte sequence gets its Cryptomatte effect instead of being shown
 *   as raw hash colours. Non-EXR rows cannot take a preset and are locked off.
 *
 * PRESET LIBRARY
 *   One .ffx per AOV in a single folder. The effect inside can be whatever
 *   suits that pass - EXtractoR for most, Cryptomatte for crypto layers.
 *   Default pattern:  extractAOV_<AOV>.ffx
 *
 *   Save presets with ONLY the effect selected. If a preset carries transform
 *   properties they are applied before the grid layout is set, so the layout
 *   still wins, but the preset will also drag along anything else it captured.
 *
 * Settings persist between runs via app.settings.
 *
 * Install:  copy to  .../Support Files/Scripts/ScriptUI Panels/  (or run via
 *           File > Scripts > Run Script File...)
 */

(function jd_AOVcontactsheet() {

    // ----------------------------------------------------------------------
    // Config
    // ----------------------------------------------------------------------

    var SCRIPT_NAME = "AOV Contact Sheet";
    var SCRIPT_VERSION = "1.0.0";
    var SETTINGS_SECTION = "jd_AOVcontactsheet";
    var COMP_SUFFIX = "_AOVs_contactsheet";
    var BEAUTY_NAME = "Beauty";
    var AOV_TOKEN = "<AOV>";

    var MAX_WINDOW_WIDTH = 1180;   // header text is clamped to this
    var ROW_HEIGHT = 24;           // approximate height of one AOV row, for wrap maths

    // "native" is a future-reserved word in ES3, so these keys must stay quoted.
    var STATUS_COLOR = {
        "native":  [0.60, 0.85, 0.60],   // light green
        "found":   [0.13, 0.82, 0.28],   // vivid green
        "missing": [1.00, 0.55, 0.10]    // orange
    };

    var BEAUTY_ALIASES = /^(beauty|rgba|rgb|main|combined|composite)$/i;
    var EXR_EXT = /\.exr$/i;

    // Only used to filter the file-open dialog. Selection in the Project panel
    // is accepted on hasVideo instead, so anything AE can read is fair game.
    var IMAGE_EXT = /\.(exr|png|jpe?g|jpe|tiff?|tga|targa|dpx|cin|hdr|rgbe|psd|psb|bmp|gif|sgi|rgb|rgba|pict|pct|iff|webp|jp2|j2k|jpf|heic|heif|dds|ppm|pgm|pbm|crw|cr2|cr3|nef|arw|dng|raf|orf|mov|mp4|mxf|avi)$/i;

    // Names are split on these before anything is compared or trimmed, so a
    // shared "bubbles_v03_" never eats into "diffuse" vs "direct".
    var TOKEN_SPLIT = /[._\-\s]+/;

    var NAME_SOURCES = ["shared", "lastname", "regex", "filename", "folder"];
    var NAME_SOURCE_LABELS = [
        "Strip what the filenames share (recommended)",
        "Last name before the frame number",
        "Custom pattern (regex)",
        "Whole filename",
        "Parent folder name"
    ];

    var DEFAULTS = {
        presetFolder: "",
        presetPattern: "extractAOV_" + AOV_TOKEN + ".ffx",
        matchCase: false,            // off = filename case does not have to match
        collapseCrypto: true,
        skipMissing: false,          // false = still create the cell, just unconfigured
        multiNameSource: "shared",
        multiNameRegex: "([^_.]+)[._]\\d+\\.exr$",
        sourceColumnWidth: 0,        // 0 = size the column to the longest name
        maxSheetWidth: 3840,
        addLabels: true,
        addBackground: true,
        labelFont: "",               // empty = leave the AE default font
        labelColor: "#FFFF00",
        labelStroke: false,          // black outline behind the label
        labelSize: 24,
        labelAutoSize: true,         // size follows the cell height instead of labelSize
        labelHeightRatio: 0.055
    };

    var SLOT_MAP = {
        "r": "R", "red": "R", "x": "R", "u": "R",
        "g": "G", "green": "G", "y": "G", "v": "G",
        "b": "B", "blue": "B", "z": "B", "w": "B",
        "a": "A", "alpha": "A"
    };

    var BARE_RGBA = { "R": 1, "G": 1, "B": 1, "A": 1, "r": 1, "g": 1, "b": 1, "a": 1 };

    // ----------------------------------------------------------------------
    // Settings persistence
    // ----------------------------------------------------------------------

    function loadSettings() {
        var s = {};
        for (var key in DEFAULTS) {
            if (!DEFAULTS.hasOwnProperty(key)) { continue; }
            var def = DEFAULTS[key];
            var raw = null;
            try {
                if (app.settings.haveSetting(SETTINGS_SECTION, key)) {
                    raw = app.settings.getSetting(SETTINGS_SECTION, key);
                }
            } catch (e) { raw = null; }

            if (raw === null) { s[key] = def; }
            else if (typeof def === "boolean") { s[key] = (raw === "true"); }
            else if (typeof def === "number") {
                var n = parseFloat(raw);
                s[key] = isNaN(n) ? def : n;
            } else { s[key] = raw; }
        }
        return s;
    }

    function saveSettings(s) {
        for (var key in DEFAULTS) {
            if (!DEFAULTS.hasOwnProperty(key)) { continue; }
            try { app.settings.saveSetting(SETTINGS_SECTION, key, String(s[key])); } catch (e) {}
        }
    }

    // ----------------------------------------------------------------------
    // Helpers
    // ----------------------------------------------------------------------

    function unique(arr) {
        var out = [], seen = {};
        for (var i = 0; i < arr.length; i++) {
            if (!seen.hasOwnProperty(arr[i])) { seen[arr[i]] = 1; out.push(arr[i]); }
        }
        return out;
    }

    function indexOfIn(arr, str) {
        for (var i = 0; i < arr.length; i++) { if (arr[i] === str) { return i; } }
        return -1;
    }

    function trim(s) { return String(s).replace(/^\s+/, "").replace(/\s+$/, ""); }
    function trimSeparators(s) { return String(s).replace(/^[._\-\s]+/, "").replace(/[._\-\s]+$/, ""); }

    function clamp(v, lo, hi) { return (v < lo) ? lo : ((v > hi) ? hi : v); }

    function hexToRgb(hex) {
        var h = trim(hex).replace(/^#/, "");
        if (h.length === 3) { h = h.charAt(0) + h.charAt(0) + h.charAt(1) + h.charAt(1) + h.charAt(2) + h.charAt(2); }
        if (!/^[0-9a-fA-F]{6}$/.test(h)) { return null; }
        return [parseInt(h.substring(0, 2), 16) / 255,
                parseInt(h.substring(2, 4), 16) / 255,
                parseInt(h.substring(4, 6), 16) / 255];
    }

    function setForeground(ctrl, rgb) {
        try {
            var g = ctrl.graphics;
            ctrl.graphics.foregroundColor = g.newPen(g.PenType.SOLID_COLOR, [rgb[0], rgb[1], rgb[2], 1], 1);
        } catch (e) {}
    }

    /**
     * The AOV list is a single column. It only splits into more when the list
     * would otherwise run off the bottom of the screen, since ScriptUI has no
     * scrollable container for arbitrary controls.
     */
    function maxRowsPerColumn() {
        var screenH = 1000;
        try {
            var screens = $.screens;
            if (screens && screens.length) {
                var best = screens[0];
                for (var i = 0; i < screens.length; i++) {
                    if (screens[i].primary) { best = screens[i]; break; }
                }
                screenH = best.bottom - best.top;
            }
        } catch (e) { screenH = 1000; }

        var usable = screenH - 300;   // title bar, headers, buttons, OS chrome
        return clamp(Math.floor(usable / ROW_HEIGHT), 6, 60);
    }

    /** Real pixel width of a string in the dialog's font, with a rough fallback. */
    function measureWidth(win, text) {
        try {
            var d = win.graphics.measureString(String(text));
            if (d && d.width) { return d.width; }
        } catch (e) {}
        return String(text).length * 7;
    }

    function namesAreUsable(names) {
        var seen = {};
        for (var i = 0; i < names.length; i++) {
            var t = trimSeparators(names[i]);
            if (t === "") { return false; }
            var key = t.toLowerCase();
            if (seen.hasOwnProperty(key)) { return false; }
            seen[key] = 1;
        }
        return true;
    }

    // ----------------------------------------------------------------------
    // Token utilities
    //
    // Everything below works on whole name tokens rather than characters. A
    // character-wise comparison would treat the "di" in diffuse/direct as
    // shared and mangle both names.
    // ----------------------------------------------------------------------

    function tokensOf(s) {
        var parts = String(s).split(TOKEN_SPLIT);
        var out = [];
        for (var i = 0; i < parts.length; i++) {
            if (parts[i] !== "") { out.push(parts[i]); }
        }
        return out;
    }

    function lastTokens(base, count) {
        var t = tokensOf(base);
        if (t.length === 0) { return ""; }
        var start = t.length - count;
        if (start < 0) { start = 0; }
        return t.slice(start).join("_");
    }

    function commonLeadingTokens(tokenLists) {
        if (tokenLists.length === 0) { return 0; }
        var minLen = tokenLists[0].length, i;
        for (i = 1; i < tokenLists.length; i++) {
            if (tokenLists[i].length < minLen) { minLen = tokenLists[i].length; }
        }
        var n = 0;
        while (n < minLen) {
            var t = tokenLists[0][n];
            var same = true;
            for (i = 1; i < tokenLists.length; i++) {
                if (tokenLists[i][n] !== t) { same = false; break; }
            }
            if (!same) { break; }
            n++;
        }
        if (n >= minLen) { n = minLen - 1; }   // never strip a name to nothing
        return (n < 0) ? 0 : n;
    }

    function commonTrailingTokens(tokenLists) {
        if (tokenLists.length === 0) { return 0; }
        var minLen = tokenLists[0].length, i;
        for (i = 1; i < tokenLists.length; i++) {
            if (tokenLists[i].length < minLen) { minLen = tokenLists[i].length; }
        }
        var n = 0;
        while (n < minLen) {
            var t = tokenLists[0][tokenLists[0].length - 1 - n];
            var same = true;
            for (i = 1; i < tokenLists.length; i++) {
                if (tokenLists[i][tokenLists[i].length - 1 - n] !== t) { same = false; break; }
            }
            if (!same) { break; }
            n++;
        }
        if (n >= minLen) { n = minLen - 1; }
        return (n < 0) ? 0 : n;
    }

    /** Character length of the first `count` tokens of `base`, separators included. */
    function tokenPrefixLength(base, count) {
        var idx = 0, taken = 0;
        while (taken < count && idx < base.length) {
            var sep = base.substring(idx).match(/^[._\-\s]+/);
            if (sep) { idx += sep[0].length; }
            var tok = base.substring(idx).match(/^[^._\-\s]+/);
            if (!tok) { break; }
            idx += tok[0].length;
            taken++;
        }
        return idx;
    }

    /** Character length of the last `count` tokens of `base`, separators included. */
    function tokenSuffixLength(base, count) {
        var idx = base.length, taken = 0;
        while (taken < count && idx > 0) {
            var head = base.substring(0, idx);
            var sep = head.match(/[._\-\s]+$/);
            if (sep) { idx -= sep[0].length; head = base.substring(0, idx); }
            var tok = head.match(/[^._\-\s]+$/);
            if (!tok) { break; }
            idx -= tok[0].length;
            taken++;
        }
        return base.length - idx;
    }

    // ----------------------------------------------------------------------
    // EXR header parsing
    // ----------------------------------------------------------------------

    function readBytes(file, maxBytes) {
        file.encoding = "BINARY";
        if (!file.open("r")) { throw new Error("Could not open " + file.fsName); }
        var buf;
        try { buf = file.read(maxBytes); } finally { file.close(); }
        return buf;
    }

    function readCString(buf, st) {
        var start = st.pos;
        while (st.pos < buf.length && buf.charCodeAt(st.pos) !== 0) { st.pos++; }
        var s = buf.substring(start, st.pos);
        st.pos++;
        return s;
    }

    function readInt32(buf, st) {
        var v = (buf.charCodeAt(st.pos)) |
                (buf.charCodeAt(st.pos + 1) << 8) |
                (buf.charCodeAt(st.pos + 2) << 16) |
                (buf.charCodeAt(st.pos + 3) << 24);
        st.pos += 4;
        return v;
    }

    function readExrChannels(file) {
        var buf = readBytes(file, 512 * 1024);
        if (buf.length < 8) { throw new Error("File is too small to be an EXR."); }
        if (buf.charCodeAt(0) !== 0x76 || buf.charCodeAt(1) !== 0x2F ||
            buf.charCodeAt(2) !== 0x31 || buf.charCodeAt(3) !== 0x01) {
            throw new Error("This file does not have an OpenEXR magic number.");
        }

        var st = { pos: 4 };
        var version = readInt32(buf, st);
        var isMultiPart = (version & 0x1000) !== 0;
        var channels = [], parts = [], partCount = 0;

        while (true) {
            var partName = "", partChannels = [];

            while (true) {
                if (st.pos >= buf.length) { break; }
                var attrName = readCString(buf, st);
                if (attrName === "") { break; }
                var attrType = readCString(buf, st);
                var attrSize = readInt32(buf, st);
                var attrStart = st.pos;

                if (attrSize < 0 || attrStart + attrSize > buf.length) {
                    throw new Error("EXR header is larger than the block that was read.");
                }

                if (attrName === "channels" && attrType === "chlist") {
                    var cs = { pos: attrStart };
                    while (cs.pos < attrStart + attrSize) {
                        var cName = readCString(buf, cs);
                        if (cName === "") { break; }
                        cs.pos += 16;
                        partChannels.push(cName);
                    }
                } else if (attrName === "name" && attrType === "string") {
                    partName = buf.substr(attrStart, attrSize);
                }
                st.pos = attrStart + attrSize;
            }

            if (isMultiPart && partName !== "") { parts.push(partName); }
            for (var i = 0; i < partChannels.length; i++) {
                channels.push((isMultiPart && partName !== "")
                    ? (partName + "." + partChannels[i]) : partChannels[i]);
            }

            if (!isMultiPart) { break; }
            if (st.pos >= buf.length) { break; }
            if (buf.charCodeAt(st.pos) === 0) { st.pos++; break; }
            if (++partCount > 128) { break; }
        }
        return { channels: unique(channels), parts: unique(parts) };
    }

    // ----------------------------------------------------------------------
    // Grouping (layers mode)
    // ----------------------------------------------------------------------

    function isKnownPart(name, parts) {
        for (var i = 0; i < parts.length; i++) { if (parts[i] === name) { return true; } }
        return false;
    }

    function groupAOVs(channels, parts) {
        var map = {}, order = [];

        function ensure(name) {
            if (!map.hasOwnProperty(name)) {
                map[name] = { name: name, R: null, G: null, B: null, A: null,
                              single: null, isBeauty: false };
                order.push(name);
            }
            return map[name];
        }

        for (var i = 0; i < channels.length; i++) {
            var ch = channels[i];
            var base = null, slot = null, beauty = false;
            var dot = ch.lastIndexOf(".");

            if (dot > 0 && dot < ch.length - 1) {
                var suf = ch.substring(dot + 1).toLowerCase();
                if (SLOT_MAP.hasOwnProperty(suf)) {
                    base = ch.substring(0, dot);
                    slot = SLOT_MAP[suf];
                    // In a multi-part file the beauty pass arrives as
                    // <part>.R / <part>.G / ... - the same case as a bare R in a
                    // single-part file, so treat it the same way.
                    if (BARE_RGBA.hasOwnProperty(ch.substring(dot + 1)) &&
                        isKnownPart(base, parts)) {
                        base = BEAUTY_NAME;
                        beauty = true;
                    }
                }
            } else if (dot === -1 && BARE_RGBA.hasOwnProperty(ch)) {
                base = BEAUTY_NAME;
                slot = SLOT_MAP[ch.toLowerCase()];
                beauty = true;
            }
            if (base === null) { base = ch; slot = null; }

            var g = ensure(base);
            if (beauty) { g.isBeauty = true; }
            if (slot === null) { if (g.single === null) { g.single = ch; } }
            else if (g[slot] === null) { g[slot] = ch; }
        }

        var aovs = [];
        for (var k = 0; k < order.length; k++) {
            var grp = map[order[k]];
            var hasColor = (grp.R !== null || grp.G !== null || grp.B !== null);
            if (!hasColor && grp.single === null) { continue; }
            aovs.push({ name: grp.name, isBeauty: grp.isBeauty });
        }

        return moveBeautyFirst(aovs);
    }

    function moveBeautyFirst(aovs) {
        for (var b = 0; b < aovs.length; b++) {
            if (aovs[b].isBeauty && b > 0) {
                var beautyAov = aovs[b];
                aovs.splice(b, 1);
                aovs.unshift(beautyAov);
                break;
            }
        }
        return aovs;
    }

    /**
     * A Cryptomatte effect reads crypto_object00/01/02 as one set, so the rank
     * layers do not each deserve a cell. A layer is treated as a rank when its
     * name is <base><two digits>, the un-suffixed <base> also exists, and there
     * are at least two such siblings - which keeps ordinary AOVs ending in
     * digits (light01 next to light) from being swallowed by accident.
     */
    function collapseCryptoRanks(aovs) {
        var names = {};
        var i;
        for (i = 0; i < aovs.length; i++) { names[aovs[i].name] = true; }

        var siblingCount = {};
        for (i = 0; i < aovs.length; i++) {
            var m = aovs[i].name.match(/^(.+?)(\d{2})$/);
            if (m && names.hasOwnProperty(m[1])) {
                siblingCount[m[1]] = (siblingCount[m[1]] || 0) + 1;
            }
        }

        var kept = [], dropped = [];
        for (i = 0; i < aovs.length; i++) {
            var mm = aovs[i].name.match(/^(.+?)(\d{2})$/);
            if (mm && names.hasOwnProperty(mm[1]) && siblingCount[mm[1]] >= 2) {
                dropped.push(aovs[i].name);
                continue;
            }
            kept.push(aovs[i]);
        }
        return { aovs: kept, dropped: dropped };
    }

    /**
     * Multi-part EXRs carry a part name on every channel, so an AOV comes out as
     * "subimage03.DiffuseLighting". The part prefix is noise in a cell label and
     * it stops a preset named after the AOV alone from matching, so it is removed
     * here - but only when every name stays unique afterwards, since two parts
     * could legitimately hold a layer of the same name.
     */
    function stripOnePartPrefix(name, parts) {
        for (var i = 0; i < parts.length; i++) {
            var p = parts[i];
            if (p && name.length > p.length + 1 &&
                name.substring(0, p.length + 1) === (p + ".")) {
                return name.substring(p.length + 1);
            }
        }
        return name;
    }

    function stripPartPrefixes(aovs, parts) {
        if (!parts || parts.length === 0) { return false; }

        var proposed = [], seen = {}, i;
        for (i = 0; i < aovs.length; i++) {
            var n = stripOnePartPrefix(aovs[i].name, parts);
            var key = n.toLowerCase();
            if (n === "" || seen.hasOwnProperty(key)) { return false; }
            seen[key] = 1;
            proposed.push(n);
        }

        var changed = false;
        for (i = 0; i < aovs.length; i++) {
            if (proposed[i] !== aovs[i].name) {
                aovs[i].fullName = aovs[i].name;   // kept for the layer comment
                aovs[i].name = proposed[i];
                changed = true;
            }
        }
        return changed;
    }

    // ----------------------------------------------------------------------
    // Per-file AOV names (files mode)
    // ----------------------------------------------------------------------

    /**
     * Strips the extension and the frame number. A frame number only counts as
     * one when a separator precedes it, so light01 keeps its digits while
     * diffuse_1001 and diffuse.1001 lose theirs. Unseparated padding of four
     * digits or more is stripped as a second pass.
     */
    function stripSequenceAndExt(fileName) {
        var n = decodeURI(fileName);
        n = n.replace(/\.[^.]+$/, "");             // extension
        n = n.replace(/[._-]?\[[^\]]*\]$/, "");    // AE's [0001-0100]
        n = n.replace(/[._-]#+$/, "");             // .#### padding
        if (/[._-]\d+$/.test(n)) { n = n.replace(/[._-]\d+$/, ""); }
        else { n = n.replace(/\d{4,}$/, ""); }
        return n;
    }

    function folderNameOf(item) {
        try { return decodeURI(item.mainSource.file.parent.name); } catch (e) { return ""; }
    }

    function fileNameOf(item) {
        try { return decodeURI(item.mainSource.file.name); } catch (e) { return ""; }
    }

    function deriveByLastName(bases) {
        for (var count = 1; count <= 4; count++) {
            var names = [], i;
            for (i = 0; i < bases.length; i++) { names.push(lastTokens(bases[i], count)); }
            if (namesAreUsable(names)) {
                return { names: names, how: (count === 1)
                    ? "last name before the frame number"
                    : ("last " + count + " names before the frame number") };
            }
        }
        return null;
    }

    function deriveByShared(bases) {
        var lists = [], i, names;
        for (i = 0; i < bases.length; i++) { lists.push(tokensOf(bases[i])); }

        var lead = commonLeadingTokens(lists);
        if (lead > 0) {
            names = [];
            for (i = 0; i < bases.length; i++) {
                names.push(trimSeparators(bases[i].substring(tokenPrefixLength(bases[i], lead))));
            }
            if (namesAreUsable(names)) { return { names: names, how: "shared leading names removed" }; }
        }

        var tail = commonTrailingTokens(lists);
        if (tail > 0) {
            names = [];
            for (i = 0; i < bases.length; i++) {
                var cut = bases[i].length - tokenSuffixLength(bases[i], tail);
                names.push(trimSeparators(bases[i].substring(0, cut)));
            }
            if (namesAreUsable(names)) { return { names: names, how: "shared trailing names removed" }; }
        }
        return null;
    }

    function deriveByRegex(items, pattern) {
        var re;
        try { re = new RegExp(pattern, "i"); }
        catch (e) { return { names: null, how: "custom pattern is not valid: " + e.toString() }; }

        var names = [], misses = 0;
        for (var i = 0; i < items.length; i++) {
            var m = fileNameOf(items[i]).match(re);
            if (!m) { names.push(""); misses++; }
            else { names.push((m.length > 1 && m[1] !== undefined) ? m[1] : m[0]); }
        }
        var how = "custom pattern";
        if (misses > 0) { how += " (" + misses + " filename(s) did not match)"; }
        else if (!namesAreUsable(names)) { how += " (results are blank or duplicated)"; }
        return { names: names, how: how };
    }

    /**
     * One AOV name per selected sequence. Each rule is validated - names must be
     * non-empty and unique - and falls through to the next when it fails, so a
     * bad guess degrades instead of producing nonsense.
     */
    function deriveAOVNames(items, mode, regexPattern) {
        var bases = [], folders = [], i;
        for (i = 0; i < items.length; i++) {
            bases.push(stripSequenceAndExt(fileNameOf(items[i])));
            folders.push(folderNameOf(items[i]));
        }

        var got;

        if (mode === "regex") {
            got = deriveByRegex(items, regexPattern);
            if (got.names) { return got; }
            return { names: bases, how: got.how + " - fell back to the whole filename" };
        }
        if (mode === "filename") { return { names: bases, how: "whole filename" }; }
        if (mode === "folder") {
            if (namesAreUsable(folders)) { return { names: folders, how: "parent folder" }; }
            return { names: bases, how: "parent folder names clashed - used the whole filename" };
        }
        if (mode === "lastname") {
            got = deriveByLastName(bases);
            if (got) { return got; }
            got = deriveByShared(bases);
            if (got) { return { names: got.names, how: got.how + " (last names clashed)" }; }
            if (namesAreUsable(folders)) { return { names: folders, how: "parent folder (last names clashed)" }; }
            return { names: bases, how: "whole filename (last names clashed)" };
        }

        // shared (default)
        got = deriveByShared(bases);
        if (got) { return got; }
        got = deriveByLastName(bases);
        if (got) { return { names: got.names, how: got.how + " (nothing shared to strip)" }; }
        if (namesAreUsable(folders)) { return { names: folders, how: "parent folder" }; }
        return { names: bases, how: "whole filename" };
    }

    function aovsFromFiles(items, settings) {
        var derived = deriveAOVNames(items, settings.multiNameSource, settings.multiNameRegex);
        var aovs = [];
        for (var i = 0; i < items.length; i++) {
            var nm = trimSeparators(derived.names[i]) || ("AOV " + (i + 1));
            aovs.push({ name: nm, isBeauty: BEAUTY_ALIASES.test(nm), item: items[i] });
        }
        return { aovs: moveBeautyFirst(aovs), how: derived.how };
    }

    // ----------------------------------------------------------------------
    // Preset resolution
    // ----------------------------------------------------------------------

    function presetFileNameFor(pattern, aovName) {
        var name = String(pattern).replace(/<AOV>/g, aovName);
        if (!/\.ffx$/i.test(name)) { name += ".ffx"; }
        return name;
    }

    function findPreset(folder, fileName, matchCase) {
        if (!folder || !folder.exists) { return null; }
        try {
            var direct = new File(folder.absoluteURI + "/" + encodeURI(fileName));
            if (direct.exists) { return direct; }
        } catch (e) {}

        if (!matchCase) {
            var all = [];
            try { all = folder.getFiles("*.ffx") || []; } catch (e2) { all = []; }
            for (var i = 0; i < all.length; i++) {
                if (decodeURI(all[i].name).toLowerCase() === fileName.toLowerCase()) { return all[i]; }
            }
        }
        return null;
    }

    function statusFor(row) {
        if (!row.usePreset) { return "native"; }
        return row.preset ? "found" : "missing";
    }

    /**
     * `forced` carries the per-row Preset checkbox across window rebuilds, keyed
     * by AOV name. Presets only make sense on EXR sources, so anything else is
     * locked to native regardless.
     */
    function resolveRows(aovs, settings, layersMode, defaultItem, forced) {
        var folder = trim(settings.presetFolder) ? new Folder(trim(settings.presetFolder)) : null;
        var rows = [];

        for (var i = 0; i < aovs.length; i++) {
            var aov = aovs[i];
            var item = aov.item || defaultItem;
            var isExr = EXR_EXT.test(fileNameOf(item));
            var canPreset = isExr;

            var expected = presetFileNameFor(settings.presetPattern, aov.name);
            var preset = canPreset ? findPreset(folder, expected, settings.matchCase) : null;

            var usePreset;
            if (!canPreset) { usePreset = false; }
            else if (layersMode) { usePreset = !aov.isBeauty; }
            else if (forced.hasOwnProperty(aov.name)) { usePreset = forced[aov.name]; }
            else { usePreset = !!preset; }          // ticks itself when one is found

            var row = {
                aov: aov, item: item, isExr: isExr, canPreset: canPreset,
                sourceName: fileNameOf(item),
                expected: expected, preset: preset, usePreset: usePreset
            };
            row.status = statusFor(row);
            rows.push(row);
        }
        return rows;
    }

    // ----------------------------------------------------------------------
    // Footage
    // ----------------------------------------------------------------------

    function isUsableFootage(item) {
        if (!(item instanceof FootageItem)) { return false; }
        var src = item.mainSource;
        if (!(src instanceof FileSource) || !src.file) { return false; }
        var hasVideo = false;
        try { hasVideo = item.hasVideo; } catch (e) { hasVideo = false; }
        return hasVideo;
    }

    function getSourceFootageItems() {
        var items = [], i;
        var sel = app.project.selection;
        for (i = 0; i < sel.length; i++) {
            if (isUsableFootage(sel[i])) { items.push(sel[i]); }
        }
        if (items.length > 0) { return items; }

        var picked = File.openDialog(
            "Choose one multi-layer EXR, or several single-AOV files",
            function (file) { return (file instanceof Folder) || IMAGE_EXT.test(file.name); },
            true);
        if (!picked) { return []; }
        if (!(picked instanceof Array)) { picked = [picked]; }

        for (i = 0; i < picked.length; i++) {
            var io = new ImportOptions(picked[i]);
            io.importAs = ImportAsType.FOOTAGE;
            var item = app.project.importFile(io);
            if (isUsableFootage(item)) { items.push(item); }
        }
        if (items.length === 0) { throw new Error("Nothing could be imported as usable footage."); }
        return items;
    }

    function baseNameOf(footageItem) {
        var n = stripSequenceAndExt(fileNameOf(footageItem));
        return n === "" ? "AOVs" : n;
    }

    /** Name for the sheet comp: the single file, or the names the set shares. */
    function sheetBaseName(items) {
        if (items.length === 1) { return baseNameOf(items[0]); }

        var bases = [], lists = [], i;
        for (i = 0; i < items.length; i++) {
            var b = stripSequenceAndExt(fileNameOf(items[i]));
            bases.push(b);
            lists.push(tokensOf(b));
        }

        var lead = commonLeadingTokens(lists);
        if (lead > 0) {
            var shared = trimSeparators(bases[0].substring(0, tokenPrefixLength(bases[0], lead)));
            if (shared !== "") { return shared; }
        }

        var folder = folderNameOf(items[0]);
        return folder !== "" ? folder : "AOVs";
    }

    // ----------------------------------------------------------------------
    // Default preset location
    // ----------------------------------------------------------------------

    /**
     * Where After Effects itself puts .ffx files when you use Save Animation
     * Preset - Documents/Adobe/After Effects <version>/User Presets. The
     * version in that path moves every year, so the newest-looking one wins.
     * Falls back to Documents, then to nothing.
     */
    function defaultPresetFolder() {
        var docs = null;
        try { docs = Folder.myDocuments; } catch (e) { docs = null; }
        if (!docs || !docs.exists) { return ""; }

        var adobe = new Folder(docs.absoluteURI + "/Adobe");
        if (adobe.exists) {
            var kids = [];
            try {
                kids = adobe.getFiles(function (f) {
                    return (f instanceof Folder) && /after\s*effects/i.test(decodeURI(f.name));
                }) || [];
            } catch (e2) { kids = []; }

            kids.sort(function (a, b) {
                var na = decodeURI(a.name), nb = decodeURI(b.name);
                return (na < nb) ? 1 : ((na > nb) ? -1 : 0);   // newest version first
            });

            for (var i = 0; i < kids.length; i++) {
                var up = new Folder(kids[i].absoluteURI + "/User Presets");
                if (up.exists) { return up.fsName; }
            }
            if (kids.length > 0) { return kids[0].fsName; }
        }
        return docs.fsName;
    }

    // ----------------------------------------------------------------------
    // Fonts
    // ----------------------------------------------------------------------

    /** Returns an array of PostScript names, or null when AE will not give one. */
    function getFontList() {
        var out = [];
        try {
            var all = app.fonts.allFonts;
            for (var i = 0; i < all.length; i++) {
                var ps = null;
                try { ps = all[i].postScriptName; } catch (e) { ps = null; }
                if (ps) { out.push(ps); }
            }
        } catch (e2) { return null; }

        if (out.length === 0) { return null; }
        out = unique(out);
        out.sort(function (a, b) {
            var la = a.toLowerCase(), lb = b.toLowerCase();
            return (la < lb) ? -1 : ((la > lb) ? 1 : 0);
        });
        return out;
    }

    // ----------------------------------------------------------------------
    // Building
    // ----------------------------------------------------------------------

    function applyPresetToLayer(comp, layer, presetFile) {
        var previous = comp.selectedLayers;
        var i;
        for (i = 0; i < previous.length; i++) { previous[i].selected = false; }
        layer.selected = true;
        try {
            layer.applyPreset(presetFile);
        } finally {
            layer.selected = false;
            for (i = 0; i < previous.length; i++) { previous[i].selected = true; }
        }
    }

    function addLabel(comp, text, cx, cellBottomY, cellH, settings) {
        var t = comp.layers.addText(text);
        var prop = t.property("ADBE Text Properties").property("ADBE Text Document");
        var td = prop.value;

        var size = settings.labelAutoSize
            ? Math.max(9, Math.round(cellH * settings.labelHeightRatio))
            : Math.max(1, Math.round(settings.labelSize));

        td.fontSize = size;
        if (trim(settings.labelFont)) {
            try { td.font = trim(settings.labelFont); } catch (e) { /* keep the default face */ }
        }

        var rgb = hexToRgb(settings.labelColor) || [1, 1, 1];
        td.applyFill = true;
        td.fillColor = rgb;
        td.applyStroke = !!settings.labelStroke;
        if (settings.labelStroke) {
            td.strokeColor = [0, 0, 0];
            td.strokeWidth = Math.max(1, Math.round(size * 0.12));
            td.strokeOverFill = false;
        }
        td.justification = ParagraphJustification.CENTER_JUSTIFY;
        prop.setValue(td);

        t.name = "label - " + text;
        t.transform.position.setValue([cx, cellBottomY - size * 0.6]);
        return t;
    }

    function buildSheet(rows, settings, compBaseName) {
        var n = rows.length, i;

        // Cells are sized off the largest source; anything smaller is fitted
        // inside its cell rather than cropped.
        var maxW = 1, maxH = 1, par = 1, fps = 0, dur = 0;
        for (i = 0; i < n; i++) {
            var it = rows[i].item;
            if (it.width > maxW) { maxW = it.width; }
            if (it.height > maxH) { maxH = it.height; }
            if (it.duration > dur) { dur = it.duration; }
            if (fps === 0 && it.frameRate > 0) { fps = it.frameRate; }
        }
        try { par = rows[0].item.pixelAspect; } catch (e) { par = 1; }
        if (fps === 0) { fps = 24; }
        if (dur === 0) { dur = 10 / fps; }

        var cols = Math.ceil(Math.sqrt(n));
        var rowCount = Math.ceil(n / cols);

        var k = 1;
        if (cols * maxW > settings.maxSheetWidth) { k = settings.maxSheetWidth / (cols * maxW); }

        var cellW = Math.max(2, Math.round(maxW * k));
        var cellH = Math.max(2, Math.round(maxH * k));
        var sheetW = cellW * cols, sheetH = cellH * rowCount;

        var comp = app.project.items.addComp(compBaseName + COMP_SUFFIX,
                                             sheetW, sheetH, par, dur, fps);
        comp.bgColor = [0, 0, 0];

        var textLayers = [];
        var report = { applied: 0, failed: [], missing: [], cells: 0 };

        for (i = 0; i < n; i++) {
            var row = rows[i];
            var col = i % cols, gridRow = Math.floor(i / cols);
            var cx = col * cellW + cellW / 2;
            var cy = gridRow * cellH + cellH / 2;

            var lay = comp.layers.add(row.item, dur);
            lay.name = row.aov.name;
            report.cells++;

            // Preset first: if it happens to carry transform properties, the
            // layout set below still wins.
            if (row.usePreset && row.preset) {
                try { applyPresetToLayer(comp, lay, row.preset); report.applied++; }
                catch (e) { report.failed.push(row.aov.name + ": " + e.toString()); }
            } else if (row.usePreset) {
                report.missing.push(row.aov.name + "  (expected " + row.expected + ")");
                lay.comment = "NO PRESET - expected " + row.expected;
            } else {
                lay.comment = row.isExr ? "shown natively - no preset applied"
                                        : "non-EXR source - shown natively";
            }
            if (row.aov.fullName) {
                lay.comment = "channels: " + row.aov.fullName + "   |   " + lay.comment;
            }

            var fit = Math.min(cellW / row.item.width, cellH / row.item.height);
            lay.transform.scale.setValue([fit * 100, fit * 100]);
            lay.transform.position.setValue([cx, cy]);

            if (settings.addLabels) {
                textLayers.push(addLabel(comp, row.aov.name, cx,
                                         gridRow * cellH + cellH, cellH, settings));
            }
        }

        for (var j = 0; j < textLayers.length; j++) { textLayers[j].moveToBeginning(); }

        if (settings.addBackground) {
            var bg = comp.layers.addSolid([0, 0, 0], "BG", sheetW, sheetH, par, dur);
            bg.moveToEnd();
            bg.locked = true;
        }

        comp.openInViewer();
        return { comp: comp, cols: cols, rows: rowCount, report: report };
    }

    // ----------------------------------------------------------------------
    // Configuration window
    // ----------------------------------------------------------------------

    function showConfig(settings) {
        var w = new Window("dialog", SCRIPT_NAME + " - Configuration");
        w.orientation = "column";
        w.alignChildren = ["fill", "top"];
        w.margins = 16;
        w.spacing = 12;

        // --- preset library ---
        var pPanel = w.add("panel", undefined, "Preset library");
        pPanel.orientation = "column";
        pPanel.alignChildren = ["fill", "top"];
        pPanel.margins = 14;
        pPanel.spacing = 8;

        var folderRow = pPanel.add("group");
        folderRow.add("statictext", undefined, "Folder:");
        var folderField = folderRow.add("edittext", undefined, settings.presetFolder);
        folderField.characters = 38;
        var browseBtn = folderRow.add("button", undefined, "Browse...");

        var patRow = pPanel.add("group");
        patRow.add("statictext", undefined, "Filename:");
        var patField = patRow.add("edittext", undefined, settings.presetPattern);
        patField.characters = 38;

        var previewText = pPanel.add("statictext", undefined, "");
        function refreshPreview() {
            previewText.text = "An AOV called \"diffuse\" looks for:  " +
                               presetFileNameFor(patField.text, "diffuse");
        }
        patField.onChanging = refreshPreview;
        refreshPreview();

        var cbCase = pPanel.add("checkbox", undefined, "Match filename case");
        cbCase.value = settings.matchCase;

        browseBtn.onClick = function () {
            var start = trim(folderField.text) ? new Folder(trim(folderField.text)) : null;
            var f = Folder.selectDialog("Folder containing your .ffx presets", start);
            if (f) { folderField.text = f.fsName; }
        };

        // --- naming for separate files ---
        var nPanel = w.add("panel", undefined, "Separate files - where the AOV name comes from");
        nPanel.orientation = "column";
        nPanel.alignChildren = ["left", "top"];
        nPanel.margins = 14;
        nPanel.spacing = 6;

        var nameRow = nPanel.add("group");
        nameRow.add("statictext", undefined, "Rule:").preferredSize.width = 54;
        var nameDrop = nameRow.add("dropdownlist", undefined, NAME_SOURCE_LABELS);
        nameDrop.preferredSize.width = 290;
        var nsIdx = indexOfIn(NAME_SOURCES, settings.multiNameSource);
        nameDrop.selection = (nsIdx === -1) ? 0 : nsIdx;

        var reRow = nPanel.add("group");
        reRow.add("statictext", undefined, "Pattern:").preferredSize.width = 54;
        var reField = reRow.add("edittext", undefined, settings.multiNameRegex);
        reField.characters = 40;

        var reHint = nPanel.add("statictext", undefined,
            "Regex matched against the filename; capture group 1 is the AOV name.", { multiline: true });
        reHint.preferredSize = [430, 16];

        function syncRegexEnabled() {
            reField.enabled = (NAME_SOURCES[nameDrop.selection.index] === "regex");
        }
        nameDrop.onChange = syncRegexEnabled;
        syncRegexEnabled();

        // --- aov handling ---
        var aPanel = w.add("panel", undefined, "AOV handling");
        aPanel.orientation = "column";
        aPanel.alignChildren = ["left", "top"];
        aPanel.margins = 14;
        aPanel.spacing = 6;

        var cbCrypto = aPanel.add("checkbox", undefined,
            "Collapse Cryptomatte rank layers (crypto_x00/01/02) into their base layer");
        cbCrypto.value = settings.collapseCrypto;

        var cbSkip = aPanel.add("checkbox", undefined, "Skip AOVs whose preset is missing");
        cbSkip.value = settings.skipMissing;

        var colRow = aPanel.add("group");
        colRow.add("statictext", undefined, "Source column width (px, 0 = auto):");
        var colField = colRow.add("edittext", undefined, String(settings.sourceColumnWidth));
        colField.characters = 6;

        // --- sheet ---
        var sPanel = w.add("panel", undefined, "Sheet");
        sPanel.orientation = "column";
        sPanel.alignChildren = ["left", "top"];
        sPanel.margins = 14;
        sPanel.spacing = 6;

        var wRow = sPanel.add("group");
        wRow.add("statictext", undefined, "Max sheet width (px):");
        var widthField = wRow.add("edittext", undefined, String(settings.maxSheetWidth));
        widthField.characters = 6;

        var cbBg = sPanel.add("checkbox", undefined, "Add a black background solid");
        cbBg.value = settings.addBackground;

        // --- labels ---
        var lPanel = w.add("panel", undefined, "Labels");
        lPanel.orientation = "column";
        lPanel.alignChildren = ["left", "top"];
        lPanel.margins = 14;
        lPanel.spacing = 6;

        var cbLabels = lPanel.add("checkbox", undefined, "Add a text label under each AOV");
        cbLabels.value = settings.addLabels;

        var fontRow = lPanel.add("group");
        fontRow.add("statictext", undefined, "Font:").preferredSize.width = 44;
        var fontList = getFontList();
        var fontDrop = null, fontField = null;

        if (fontList) {
            var choices = ["(After Effects default)"].concat(fontList);
            if (trim(settings.labelFont) && indexOfIn(fontList, trim(settings.labelFont)) === -1) {
                choices.splice(1, 0, trim(settings.labelFont));   // keep an unknown saved face usable
            }
            fontDrop = fontRow.add("dropdownlist", undefined, choices);
            fontDrop.preferredSize.width = 300;
            fontDrop.selection = 0;
            for (var fi = 0; fi < fontDrop.items.length; fi++) {
                if (fontDrop.items[fi].text === trim(settings.labelFont)) { fontDrop.selection = fi; break; }
            }
        } else {
            fontField = fontRow.add("edittext", undefined, settings.labelFont);
            fontField.characters = 34;
            fontRow.add("statictext", undefined, "PostScript name");
        }

        var colorRow = lPanel.add("group");
        colorRow.add("statictext", undefined, "Colour:").preferredSize.width = 44;
        var colorField = colorRow.add("edittext", undefined, settings.labelColor);
        colorField.characters = 9;
        var swatch = colorRow.add("panel");
        swatch.preferredSize = [34, 20];
        colorRow.add("statictext", undefined, "hex, e.g. #FFCC00");
        var cbStroke = colorRow.add("checkbox", undefined, "Black outline");
        cbStroke.value = settings.labelStroke;

        function paintSwatch() {
            var c = hexToRgb(colorField.text);
            if (!c) { return; }
            try {
                swatch.graphics.backgroundColor =
                    swatch.graphics.newBrush(swatch.graphics.BrushType.SOLID_COLOR, [c[0], c[1], c[2], 1]);
                swatch.hide(); swatch.show();
            } catch (e) {}
        }
        colorField.onChanging = paintSwatch;

        var sizeRow = lPanel.add("group");
        sizeRow.add("statictext", undefined, "Size:").preferredSize.width = 44;
        var sizeField = sizeRow.add("edittext", undefined, String(settings.labelSize));
        sizeField.characters = 5;
        sizeRow.add("statictext", undefined, "px");
        var cbAutoSize = sizeRow.add("checkbox", undefined, "Scale to cell height");
        cbAutoSize.value = settings.labelAutoSize;

        function syncSizeEnabled() { sizeField.enabled = !cbAutoSize.value; }
        cbAutoSize.onClick = syncSizeEnabled;
        syncSizeEnabled();

        // --- buttons ---
        var btns = w.add("group");
        btns.alignment = ["right", "top"];
        var resetBtn = btns.add("button", undefined, "Reset");
        btns.add("button", undefined, "Cancel", { name: "cancel" });
        var saveBtn = btns.add("button", undefined, "Save", { name: "ok" });

        resetBtn.onClick = function () {
            folderField.text = DEFAULTS.presetFolder;
            patField.text = DEFAULTS.presetPattern;
            cbCase.value = DEFAULTS.matchCase;
            nameDrop.selection = indexOfIn(NAME_SOURCES, DEFAULTS.multiNameSource);
            reField.text = DEFAULTS.multiNameRegex;
            cbCrypto.value = DEFAULTS.collapseCrypto;
            cbSkip.value = DEFAULTS.skipMissing;
            colField.text = String(DEFAULTS.sourceColumnWidth);
            widthField.text = String(DEFAULTS.maxSheetWidth);
            cbBg.value = DEFAULTS.addBackground;
            cbLabels.value = DEFAULTS.addLabels;
            if (fontDrop) { fontDrop.selection = 0; } else { fontField.text = DEFAULTS.labelFont; }
            colorField.text = DEFAULTS.labelColor;
            cbStroke.value = DEFAULTS.labelStroke;
            sizeField.text = String(DEFAULTS.labelSize);
            cbAutoSize.value = DEFAULTS.labelAutoSize;
            syncSizeEnabled();
            syncRegexEnabled();
            refreshPreview();
            paintSwatch();
        };

        var out = null;
        saveBtn.onClick = function () {
            var pattern = trim(patField.text);
            if (pattern.indexOf(AOV_TOKEN) === -1) {
                alert("The preset filename pattern must contain " + AOV_TOKEN + ".");
                return;
            }
            if (!hexToRgb(colorField.text)) {
                alert("The label colour must be a hex value such as #FFCC00.");
                return;
            }
            var namingRule = NAME_SOURCES[nameDrop.selection.index];
            if (namingRule === "regex") {
                try { new RegExp(trim(reField.text), "i"); }
                catch (e) { alert("That is not a valid regular expression:\n\n" + e.toString()); return; }
            }
            var mw = parseInt(trim(widthField.text), 10);
            if (isNaN(mw) || mw < 64) { mw = DEFAULTS.maxSheetWidth; }
            var ls = parseInt(trim(sizeField.text), 10);
            if (isNaN(ls) || ls < 1) { ls = DEFAULTS.labelSize; }
            var cw = parseInt(trim(colField.text), 10);
            if (isNaN(cw) || cw < 0) { cw = DEFAULTS.sourceColumnWidth; }

            var chosenFont = "";
            if (fontDrop) {
                chosenFont = (fontDrop.selection && fontDrop.selection.index > 0)
                    ? fontDrop.selection.text : "";
            } else {
                chosenFont = trim(fontField.text);
            }

            out = {
                presetFolder: trim(folderField.text),
                presetPattern: pattern,
                matchCase: cbCase.value,
                collapseCrypto: cbCrypto.value,
                skipMissing: cbSkip.value,
                multiNameSource: namingRule,
                multiNameRegex: trim(reField.text),
                sourceColumnWidth: cw,
                maxSheetWidth: mw,
                addLabels: cbLabels.value,
                addBackground: cbBg.value,
                labelFont: chosenFont,
                labelColor: trim(colorField.text),
                labelStroke: cbStroke.value,
                labelSize: ls,
                labelAutoSize: cbAutoSize.value,
                labelHeightRatio: settings.labelHeightRatio
            };
            saveSettings(out);
            w.close();
        };

        paintSwatch();
        w.show();
        return out;
    }

    // ----------------------------------------------------------------------
    // Main window
    // ----------------------------------------------------------------------

    /**
     * Returns { action: "build"|"config"|"refresh", rows: [...], forced: {} }
     * or null. The window is rebuilt on refresh, which keeps ScriptUI out of the
     * business of redrawing coloured rows in place; the Preset checkbox states
     * travel back out in `forced` so they survive that rebuild.
     */
    function showMain(sourceLine, rows, settings, layersMode) {
        var w = new Window("dialog", SCRIPT_NAME + " v" + SCRIPT_VERSION);
        w.orientation = "column";
        w.alignChildren = ["fill", "top"];
        w.margins = 16;
        w.spacing = 10;

        var i;
        var headline = w.add("statictext", undefined, sourceLine);
        var countLine = w.add("statictext", undefined, "");
        var folderLine = w.add("statictext", undefined,
            "Preset folder: " + (trim(settings.presetFolder) || "(not set - open Configure)"));

        // Columns are measured off the real strings so nothing gets truncated,
        // then the row count per column is chosen to keep the window sane.
        var nameW = 90, midW = 140;
        for (i = 0; i < rows.length; i++) {
            nameW = Math.max(nameW, measureWidth(w, rows[i].aov.name) + 30);
            var midText = layersMode ? (rows[i].preset ? decodeURI(rows[i].preset.name) : rows[i].expected)
                                     : rows[i].sourceName;
            midW = Math.max(midW, measureWidth(w, midText) + 14);
        }
        nameW = clamp(nameW, 110, 320);
        midW = (settings.sourceColumnWidth > 0) ? settings.sourceColumnWidth : clamp(midW, 150, 620);

        var presetW = layersMode ? 0 : 58;
        var statusW = 66;
        var colWidth = nameW + midW + presetW + statusW + 40;

        // One column, unless the list is taller than the screen can show.
        var perColumn = Math.min(rows.length, maxRowsPerColumn());
        if (perColumn < 1) { perColumn = 1; }
        var colCount = Math.ceil(rows.length / perColumn);

        var headerWidth = Math.min(MAX_WINDOW_WIDTH, colWidth * colCount);
        headline.preferredSize.width = headerWidth;
        countLine.preferredSize.width = headerWidth;
        folderLine.preferredSize.width = headerWidth;

        var listPanel = w.add("panel", undefined, "AOVs");
        listPanel.orientation = "row";
        listPanel.alignChildren = ["left", "top"];
        listPanel.margins = 12;
        listPanel.spacing = 18;

        var includeBoxes = [], presetBoxes = [], statusTexts = [];

        function refreshCounts() {
            var found = 0, missing = 0, nativeCount = 0;
            for (var q = 0; q < rows.length; q++) {
                if (rows[q].status === "found") { found++; }
                else if (rows[q].status === "missing") { missing++; }
                else { nativeCount++; }
            }
            countLine.text = rows.length + " cell(s):  " + found + " preset(s) found, " +
                             missing + " missing, " + nativeCount + " native";
        }

        for (var c = 0; c < colCount; c++) {
            var colGrp = listPanel.add("group");
            colGrp.orientation = "column";
            colGrp.alignChildren = ["left", "top"];
            colGrp.spacing = 3;

            var hdr = colGrp.add("group");
            hdr.orientation = "row";
            hdr.spacing = 8;
            hdr.add("statictext", undefined, "AOV").preferredSize.width = nameW;
            hdr.add("statictext", undefined, layersMode ? "Preset file" : "Source file")
                .preferredSize.width = midW;
            if (!layersMode) { hdr.add("statictext", undefined, "Preset").preferredSize.width = presetW; }
            hdr.add("statictext", undefined, "Status").preferredSize.width = statusW;

            var start = c * perColumn;
            var end = Math.min(start + perColumn, rows.length);

            for (i = start; i < end; i++) {
                var r = rows[i];
                var rowGrp = colGrp.add("group");
                rowGrp.orientation = "row";
                rowGrp.spacing = 8;

                var cb = rowGrp.add("checkbox", undefined, r.aov.name);
                cb.value = true;
                cb.preferredSize.width = nameW;
                cb.helpTip = r.aov.name;
                includeBoxes.push(cb);

                var middle = layersMode ? (r.preset ? decodeURI(r.preset.name) : r.expected)
                                        : r.sourceName;
                var midTxt = rowGrp.add("statictext", undefined, middle);
                midTxt.preferredSize.width = midW;
                midTxt.helpTip = layersMode ? ("source: " + r.sourceName)
                                            : ("preset looked for: " + r.expected);

                var pcb = null;
                if (!layersMode) {
                    pcb = rowGrp.add("checkbox", undefined, "");
                    pcb.preferredSize.width = presetW;
                    pcb.value = r.usePreset;
                    pcb.enabled = r.canPreset;
                    pcb.helpTip = r.canPreset
                        ? ("Apply " + r.expected + " to this AOV")
                        : "Presets only apply to EXR sources";
                }
                presetBoxes.push(pcb);

                var statusTxt = rowGrp.add("statictext", undefined, r.status);
                statusTxt.preferredSize.width = statusW;
                setForeground(statusTxt, STATUS_COLOR[r.status]);
                statusTexts.push(statusTxt);

                if (pcb) {
                    (function (row, box, label) {
                        box.onClick = function () {
                            row.usePreset = box.value;
                            row.status = statusFor(row);
                            label.text = row.status;
                            setForeground(label, STATUS_COLOR[row.status]);
                            refreshCounts();
                        };
                    })(r, pcb, statusTxt);
                }
            }
        }

        refreshCounts();

        // --- buttons ---
        var btns = w.add("group");
        btns.alignment = ["fill", "top"];

        var leftBtns = btns.add("group");
        leftBtns.alignment = ["left", "center"];
        var openBtn = leftBtns.add("button", undefined, "Open preset folder");
        var refreshBtn = leftBtns.add("button", undefined, "Refresh");
        var configBtn = leftBtns.add("button", undefined, "Configure...");

        var rightBtns = btns.add("group");
        rightBtns.alignment = ["right", "center"];
        rightBtns.add("button", undefined, "Cancel", { name: "cancel" });
        var ok = rightBtns.add("button", undefined, "Build Contact Sheet", { name: "ok" });

        var presetFolder = trim(settings.presetFolder) ? new Folder(trim(settings.presetFolder)) : null;
        openBtn.enabled = !!(presetFolder && presetFolder.exists);
        openBtn.onClick = function () {
            if (!presetFolder || !presetFolder.exists) {
                alert("The preset folder does not exist:\n" + trim(settings.presetFolder));
                return;
            }
            presetFolder.execute();
        };

        function collectForced() {
            var forced = {};
            for (var q = 0; q < rows.length; q++) {
                if (presetBoxes[q]) { forced[rows[q].aov.name] = presetBoxes[q].value; }
            }
            return forced;
        }

        var result = null;
        refreshBtn.onClick = function () {
            result = { action: "refresh", forced: collectForced() };
            w.close();
        };
        configBtn.onClick = function () {
            result = { action: "config", forced: collectForced() };
            w.close();
        };

        ok.onClick = function () {
            var chosen = [];
            for (var q = 0; q < includeBoxes.length; q++) {
                if (includeBoxes[q].value) { chosen.push(rows[q]); }
            }
            if (chosen.length === 0) { alert("Select at least one AOV."); return; }

            if (settings.skipMissing) {
                var kept = [];
                for (var j = 0; j < chosen.length; j++) {
                    if (!(chosen[j].usePreset && !chosen[j].preset)) { kept.push(chosen[j]); }
                }
                if (kept.length === 0) { alert("Every selected AOV is missing its preset."); return; }
                chosen = kept;
            }
            result = { action: "build", rows: chosen, forced: collectForced() };
            w.close();
        };

        w.show();
        return result;
    }

    // ----------------------------------------------------------------------
    // Main
    // ----------------------------------------------------------------------

    function main() {
        if (!app.project) { throw new Error("Open a project first."); }

        var settings = loadSettings();

        var isFirstRun = false;
        try { isFirstRun = !app.settings.haveSetting(SETTINGS_SECTION, "presetFolder"); }
        catch (e) { isFirstRun = false; }
        if (isFirstRun && !trim(settings.presetFolder)) {
            settings.presetFolder = defaultPresetFolder();
        }

        var items = getSourceFootageItems();
        if (items.length === 0) { return; }

        // The layer-splitting path only means anything for a lone multi-layer
        // EXR. Everything else - several files, or a single non-EXR - is laid
        // out one cell per file.
        var layersMode = (items.length === 1) && EXR_EXT.test(fileNameOf(items[0]));

        var channels = [], exrParts = [], partsStripped = false;
        var baseAOVs = [], nameHow = "";

        function rebuildFileAOVs() {
            var fromFiles = aovsFromFiles(items, settings);
            baseAOVs = fromFiles.aovs;
            nameHow = fromFiles.how;
        }

        if (layersMode) {
            var exr = readExrChannels(items[0].mainSource.file);
            channels = exr.channels;
            exrParts = exr.parts;
            if (channels.length === 0) { throw new Error("No channels could be read from this file."); }
            baseAOVs = groupAOVs(channels, exrParts);
            if (baseAOVs.length === 0) {
                throw new Error("No usable AOVs were found in " + channels.length + " channels.");
            }
            partsStripped = stripPartPrefixes(baseAOVs, exrParts);
        } else {
            rebuildFileAOVs();
        }

        // Show the config once on the very first run so the prefilled preset
        // folder can be confirmed. Outside layers mode presets are optional, so
        // this is skippable there.
        if (isFirstRun && layersMode) {
            var first = showConfig(settings);
            if (!first) { return; }
            settings = first;
        }

        function sourceLineText() {
            if (!layersMode) {
                return items.length + " file(s), one AOV each  -  names from " + nameHow;
            }
            var note = "";
            if (exrParts.length > 0) { note += ", " + exrParts.length + " parts"; }
            if (partsStripped) { note += ", part prefix stripped"; }
            return fileNameOf(items[0]) + "  -  " + channels.length + " channels" + note;
        }

        var droppedRanks = [];
        var forced = {};
        var chosen = null;

        while (true) {
            var aovs = baseAOVs;
            droppedRanks = [];
            if (settings.collapseCrypto) {
                var collapsed = collapseCryptoRanks(aovs);
                aovs = collapsed.aovs;
                droppedRanks = collapsed.dropped;
            }
            var rows = resolveRows(aovs, settings, layersMode, items[0], forced);

            var res = showMain(sourceLineText(), rows, settings, layersMode);
            if (!res) { return; }
            if (res.forced) { forced = res.forced; }

            if (res.action === "refresh") {
                if (!layersMode) { rebuildFileAOVs(); }
                continue;
            }
            if (res.action === "config") {
                var updated = showConfig(settings);
                if (updated) {
                    settings = updated;
                    if (!layersMode) { rebuildFileAOVs(); }   // the naming rule may have changed
                }
                continue;
            }
            chosen = res.rows;
            break;
        }

        app.beginUndoGroup(SCRIPT_NAME);
        var built;
        try { built = buildSheet(chosen, settings, sheetBaseName(items)); }
        finally { app.endUndoGroup(); }

        var rep = built.report;
        var msg = 'Created "' + built.comp.name + '"\n\n' +
                  (layersMode ? "Mode: one multi-layer EXR\n"
                              : ("Mode: " + items.length + " separate files\nNames from: " + nameHow + "\n")) +
                  built.cols + " x " + built.rows + " grid, " + rep.cells + " cell(s)\n" +
                  "Presets applied: " + rep.applied;

        if (partsStripped) {
            msg += "\n\nMulti-part EXR: the part prefix was stripped from the AOV names.";
        }
        if (droppedRanks.length) {
            msg += "\n\nCollapsed Cryptomatte rank layers:\n  " + droppedRanks.join(", ");
        }
        if (rep.missing.length) {
            msg += "\n\nPreset wanted but not found:\n  " + rep.missing.join("\n  ");
        }
        if (rep.failed.length) {
            msg += "\n\nPreset failed to apply:\n  " + rep.failed.join("\n  ");
        }

        alert(msg, SCRIPT_NAME);
    }

    try {
        main();
    } catch (err) {
        alert(SCRIPT_NAME + " error:\n\n" + err.toString() +
              (err.line ? ("\n(line " + err.line + ")") : ""), SCRIPT_NAME);
    }

})();
