/*
 * jd_EXRCompiler 1.0.1 - After Effects panel
 *
 * Merges render passes into one multi-layer EXR and builds a Cryptomatte from
 * black-and-white or RGB mattes, by driving the jd_exrc command-line tool.
 *
 * Install: copy this file and the jd_exrc program into
 *   .../Support Files/Scripts/ScriptUI Panels/
 * (jd_exrc.exe next to the .jsx, or in a "jd_EXRCompiler" folder next to it),
 * restart After Effects, then open Window > jd_EXRCompiler.jsx.
 * It also runs as a floating window via File > Scripts > Run Script File...
 *
 * Needs: Edit > Preferences > Scripting & Expressions >
 *        "Allow Scripts to Write Files and Access Network".
 *
 * ExtendScript is ES3: no let/const/arrow functions, no JSON object, no
 * Array.indexOf, no String.trim, no Math.imul. Everything here sticks to that.
 *
 * The first half of this file is plain logic with no After Effects calls
 * (hash, JSON, job builder, naming). Under node it is exported for the tests in
 * panel/tests/; in After Effects the panel is built.
 */

(function jd_EXRCompilerPanel(thisObj) {

    var TOOL = "jd_EXRCompiler";
    var VERSION = "1.0.1";
    var SETUP_SUFFIX = ".jd_EXRCompiler.json";
    var SETTINGS_SECTION = "jd_EXRCompiler";
    var EXE_NAMES = ["jd_exrc.exe", "jd_exrc"];
    var MAX_CHUNKS = 10;              // the CLI runs in up to this many pieces, so the progress bar moves
    var MIN_CHUNK = 25;               // ...of at least this many frames: each call costs AE seconds
    var COMPRESSIONS = ["zip", "zips", "piz", "rle", "none"];   // lossless only (Cryptomatte-safe)
    var MODES = ["priority", "holdout", "normalise"];
    var MODE_LABELS = ["Priority", "Holdout", "Normalise"];
    var RANKS = [2, 4, 6, 8, 10, 12, 14, 16];
    var CRYPTO_CHANNELS = ["R", "G", "B", "A", "L"];
    var BEAUTY_ALIASES = /^(beauty|rgba|main|combined|composite|image)$/i;
    var DATA_PASSES = { "depth": 1, "z": 1, "position": 1, "p": 1, "pworld": 1, "pref": 1, "normal": 1, "n": 1,
                        "motion": 1, "mv": 1, "vector": 1, "velocity": 1, "uv": 1, "id": 1, "objectid": 1 };
    var EXIT_TEXT = { "0": "ok", "1": "check failed", "2": "bad job", "3": "file error" };

    // ======================================================================
    // Small ES3 helpers
    // ======================================================================

    function trim(s) { return String(s).replace(/^\s+/, "").replace(/\s+$/, ""); }

    function isArray(v) { return Object.prototype.toString.call(v) === "[object Array]"; }

    function inArray(arr, v) {
        for (var i = 0; i < arr.length; i++) { if (arr[i] === v) { return i; } }
        return -1;
    }

    function pad(n, width) {
        var s = String(n);
        while (s.length < width) { s = "0" + s; }
        return s;
    }

    function hex8(u) { return pad((u >>> 0).toString(16), 8); }

    function repeatStr(s, n) { var o = ""; for (var i = 0; i < n; i++) { o += s; } return o; }

    function fwd(path) { return String(path).replace(/\\/g, "/"); }

    function fileNameOf(path) {
        var p = fwd(path);
        return p.substr(p.lastIndexOf("/") + 1);
    }

    function folderOf(path) {
        var p = fwd(path);
        var i = p.lastIndexOf("/");
        return i < 0 ? "" : p.substr(0, i);
    }

    function copyObj(o) { return jsonParse(jsonStringify(o)); }

    // ======================================================================
    // JSON (ExtendScript has no JSON object)
    // ======================================================================

    var JSON_ESC_OUT = { "\"": "\\\"", "\\": "\\\\", "\b": "\\b", "\f": "\\f", "\n": "\\n", "\r": "\\r", "\t": "\\t" };
    var JSON_ESC_IN = { "\"": "\"", "\\": "\\", "/": "/", "b": "\b", "f": "\f", "n": "\n", "r": "\r", "t": "\t" };

    // Everything outside printable ASCII is written as \uXXXX, so the file is
    // plain ASCII and its text encoding can never get in the way.
    function jsonQuote(s) {
        return "\"" + String(s).replace(/[\\"\u0000-\u001f\u007f-\uffff]/g, function (c) {
            var e = JSON_ESC_OUT[c];
            return e ? e : "\\u" + pad(c.charCodeAt(0).toString(16), 4);
        }) + "\"";
    }

    function jsonStringify(v, indent, cur) {
        indent = indent || 0;
        cur = cur || "";
        var nl = indent ? "\n" : "";
        var step = repeatStr(" ", indent);
        var inner = cur + step;
        var i, parts, k;
        if (v === null || v === undefined) { return "null"; }
        if (typeof v === "boolean") { return v ? "true" : "false"; }
        if (typeof v === "number") { return isFinite(v) ? String(v) : "null"; }
        if (typeof v === "string") { return jsonQuote(v); }
        if (isArray(v)) {
            if (!v.length) { return "[]"; }
            parts = [];
            for (i = 0; i < v.length; i++) { parts.push(inner + jsonStringify(v[i], indent, inner)); }
            return "[" + nl + parts.join("," + nl) + nl + cur + "]";
        }
        parts = [];
        for (k in v) {
            if (v.hasOwnProperty(k) && v[k] !== undefined && typeof v[k] !== "function") {
                parts.push(inner + jsonQuote(k) + (indent ? ": " : ":") + jsonStringify(v[k], indent, inner));
            }
        }
        if (!parts.length) { return "{}"; }
        return "{" + nl + parts.join("," + nl) + nl + cur + "}";
    }

    function jsonParse(text) {
        text = String(text);
        var at = 0;
        var ch = text.charAt(0);

        function fail(m) { throw new Error("JSON: " + m + " at character " + at); }
        function next() { at++; ch = text.charAt(at); return ch; }
        function white() { while (ch === " " || ch === "\t" || ch === "\n" || ch === "\r" || ch === "\uFEFF") { next(); } }

        function str() {
            var s = "";
            if (ch !== "\"") { fail("string expected"); }
            next();
            while (at < text.length) {
                if (ch === "\"") { next(); return s; }
                if (ch === "\\") {
                    next();
                    if (ch === "u") {
                        var h = text.substr(at + 1, 4);
                        if (!/^[0-9a-fA-F]{4}$/.test(h)) { fail("bad \\u escape"); }
                        s += String.fromCharCode(parseInt(h, 16));
                        at += 4;
                    } else {
                        var m = JSON_ESC_IN[ch];
                        if (m === undefined) { fail("bad escape"); }
                        s += m;
                    }
                    next();
                } else {
                    s += ch;
                    next();
                }
            }
            fail("unterminated string");
        }

        function num() {
            var m = /^-?(0|[1-9]\d*)(\.\d+)?([eE][+\-]?\d+)?/.exec(text.substr(at));
            if (!m) { fail("bad number"); }
            at += m[0].length;
            ch = text.charAt(at);
            return parseFloat(m[0]);
        }

        function word() {
            if (text.substr(at, 4) === "true") { at += 4; ch = text.charAt(at); return true; }
            if (text.substr(at, 5) === "false") { at += 5; ch = text.charAt(at); return false; }
            if (text.substr(at, 4) === "null") { at += 4; ch = text.charAt(at); return null; }
            fail("unexpected '" + ch + "'");
        }

        function value() {
            white();
            if (ch === "{") { return obj(); }
            if (ch === "[") { return arr(); }
            if (ch === "\"") { return str(); }
            if (ch === "-" || (ch >= "0" && ch <= "9")) { return num(); }
            return word();
        }

        function arr() {
            var a = [];
            next(); white();
            if (ch === "]") { next(); return a; }
            while (at < text.length) {
                a.push(value());
                white();
                if (ch === "]") { next(); return a; }
                if (ch !== ",") { fail("',' or ']' expected"); }
                next();
            }
            fail("unterminated array");
        }

        function obj() {
            var o = {};
            next(); white();
            if (ch === "}") { next(); return o; }
            while (at < text.length) {
                white();
                var k = str();
                white();
                if (ch !== ":") { fail("':' expected"); }
                next();
                o[k] = value();
                white();
                if (ch === "}") { next(); return o; }
                if (ch !== ",") { fail("',' or '}' expected"); }
                next();
            }
            fail("unterminated object");
        }

        var result = value();
        white();
        if (at < text.length) { fail("trailing text"); }
        return result;
    }

    // ======================================================================
    // Cryptomatte hash: MurmurHash3_x86_32, seed 0, on the UTF-8 bytes
    // ======================================================================

    // 32-bit multiply without Math.imul: split a into 16-bit halves so every
    // partial product stays exact in a double.
    function mul32(a, b) {
        a = a >>> 0; b = b >>> 0;
        var lo = (a & 0xffff) * b;
        var hi = (((a >>> 16) * b) & 0xffff) << 16;
        return (lo + hi) >>> 0;
    }

    function rotl32(x, r) { return ((x << r) | (x >>> (32 - r))) >>> 0; }

    function utf8Bytes(s) {
        var out = [];
        s = String(s);
        for (var i = 0; i < s.length; i++) {
            var c = s.charCodeAt(i);
            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.length) {
                var d = s.charCodeAt(i + 1);
                if (d >= 0xDC00 && d <= 0xDFFF) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
                    i++;
                }
            }
            if (c < 0x80) { out.push(c); }
            else if (c < 0x800) { out.push(0xC0 | (c >> 6), 0x80 | (c & 63)); }
            else if (c < 0x10000) { out.push(0xE0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63)); }
            else { out.push(0xF0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63)); }
        }
        return out;
    }

    function murmur3(str, seed) {
        var b = utf8Bytes(str);
        var len = b.length;
        var h = (seed || 0) >>> 0;
        var c1 = 0xcc9e2d51, c2 = 0x1b873593;
        var nb = len - (len % 4);
        var i, k;
        for (i = 0; i < nb; i += 4) {
            k = (b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | (b[i + 3] << 24)) >>> 0;
            k = mul32(k, c1); k = rotl32(k, 15); k = mul32(k, c2);
            h = (h ^ k) >>> 0;
            h = rotl32(h, 13);
            h = (mul32(h, 5) + 0xe6546b64) >>> 0;
        }
        k = 0;
        var rest = len % 4;
        if (rest === 3) { k ^= b[nb + 2] << 16; }
        if (rest >= 2) { k ^= b[nb + 1] << 8; }
        if (rest >= 1) {
            k ^= b[nb];
            k = mul32(k >>> 0, c1); k = rotl32(k, 15); k = mul32(k, c2);
            h = (h ^ k) >>> 0;
        }
        h = (h ^ len) >>> 0;
        h = (h ^ (h >>> 16)) >>> 0;
        h = mul32(h, 0x85ebca6b);
        h = (h ^ (h >>> 13)) >>> 0;
        h = mul32(h, 0xc2b2ae35);
        h = (h ^ (h >>> 16)) >>> 0;
        return h;
    }

    // No denormal, inf or NaN IDs: flip one exponent bit when it is all 0 or all 1.
    function cryptoHash(name) {
        var h = murmur3(name, 0);
        var e = (h >>> 23) & 255;
        if (e === 0 || e === 255) { h = (h ^ (1 << 23)) >>> 0; }
        return h;
    }

    function cryptoHex(name) { return hex8(cryptoHash(name)); }
    function cryptoLayerKey(name) { return cryptoHex(name).substr(0, 7); }

    function cryptoColour(name) {
        var h = cryptoHash(name);
        return [h & 255, (h >>> 8) & 255, (h >>> 16) & 255];
    }

    // ======================================================================
    // Tiny PNG writer for the colour chips (stored deflate, no compression)
    // ======================================================================

    var CRC_TABLE = null;

    function crc32(s) {
        var c, n, k;
        if (!CRC_TABLE) {
            CRC_TABLE = [];
            for (n = 0; n < 256; n++) {
                c = n;
                for (k = 0; k < 8; k++) { c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1); }
                CRC_TABLE[n] = c >>> 0;
            }
        }
        c = 0xFFFFFFFF;
        for (n = 0; n < s.length; n++) { c = CRC_TABLE[(c ^ s.charCodeAt(n)) & 255] ^ (c >>> 8); }
        return (c ^ 0xFFFFFFFF) >>> 0;
    }

    function adler32(s) {
        var a = 1, b = 0;
        for (var i = 0; i < s.length; i++) { a = (a + s.charCodeAt(i)) % 65521; b = (b + a) % 65521; }
        return ((b * 65536) + a) >>> 0;
    }

    function be32(u) {
        u = u >>> 0;
        return String.fromCharCode((u >>> 24) & 255, (u >>> 16) & 255, (u >>> 8) & 255, u & 255);
    }

    function pngChunk(type, data) { return be32(data.length) + type + data + be32(crc32(type + data)); }

    // A w x h RGB swatch with a 1-pixel dark border, as a binary string.
    function pngChip(rgb, w, h) {
        var raw = "", x, y, edge;
        var fill = String.fromCharCode(rgb[0], rgb[1], rgb[2]);
        var line = String.fromCharCode(48, 48, 48);
        for (y = 0; y < h; y++) {
            raw += "\u0000";
            for (x = 0; x < w; x++) {
                edge = (x === 0 || y === 0 || x === w - 1 || y === h - 1);
                raw += edge ? line : fill;
            }
        }
        var n = raw.length;   // < 65535, so one stored block
        var z = "\u0078\u0001\u0001" + String.fromCharCode(n & 255, (n >>> 8) & 255, (~n) & 255, ((~n) >>> 8) & 255) +
                raw + be32(adler32(raw));
        return "\u0089PNG\r\n\u001a\n" +
               pngChunk("IHDR", be32(w) + be32(h) + "\u0008\u0002\u0000\u0000\u0000") +
               pngChunk("IDAT", z) + pngChunk("IEND", "");
    }

    // ======================================================================
    // Names, sequences, versions
    // ======================================================================

    // "beauty.1001.exr" -> {head:"beauty.", digits:"1001", ext:".exr"}; a version
    // like "_v04.exr" is not a frame number (needs . or _ right before the digits).
    function splitFrame(fileName) {
        var m = /^(.*[._])(\d+)(\.[A-Za-z0-9]+)$/.exec(fileName);
        if (!m) { return null; }
        return { head: m[1], digits: m[2], ext: m[3] };
    }

    function patternFromFrame(fileName) {
        var s = splitFrame(fileName);
        return s ? (s.head + repeatStr("#", s.digits.length) + s.ext) : fileName;
    }

    function stripFrameAndExt(fileName) {
        var s = String(fileName).replace(/\.[A-Za-z0-9]+$/, "");
        s = s.replace(/[._](#+|%0?\d*d|\d+)$/, "");
        return s;
    }

    function stripVersion(s) {
        return String(s).replace(/(^|[._\-])v\d+(?=$|[._\-])/gi, "$1")
                        .replace(/([._\-])[._\-]+/g, "$1")
                        .replace(/^[._\-]+/, "").replace(/[._\-]+$/, "");
    }

    function nameFromFile(fileName) {
        var s = stripVersion(stripFrameAndExt(fileName));
        return s === "" ? "layer" : s;
    }

    // Tokens and the separators between them: "sh010_beauty" -> ["sh010","_","beauty"].
    function tokenize(s) {
        var out = [], cur = "", sep = "", c;
        for (var i = 0; i < s.length; i++) {
            c = s.charAt(i);
            if (c === "." || c === "_" || c === "-") {
                if (cur !== "") { out.push(cur); cur = ""; }
                sep += c;
            } else {
                if (sep !== "") { if (out.length) { out.push(sep); } sep = ""; }
                cur += c;
            }
        }
        if (cur !== "") { out.push(cur); }
        return out;
    }

    // Drop leading tokens every name shares ("sh010_beauty", "sh010_depth" ->
    // "beauty", "depth"), but never empty a name.
    function stripSharedPrefix(names) {
        if (names.length < 2) { return names.slice(0); }
        var toks = [], i, k = 0, j, minTok = 1e9;
        for (i = 0; i < names.length; i++) {
            toks.push(tokenize(names[i]));
            minTok = Math.min(minTok, (toks[i].length + 1) / 2);
        }
        while (k < minTok - 1) {
            var t = toks[0][2 * k];
            for (j = 1; j < toks.length; j++) { if (toks[j][2 * k] !== t) { break; } }
            if (j < toks.length) { break; }
            k++;
        }
        var out = [];
        for (i = 0; i < toks.length; i++) { out.push(toks[i].slice(2 * k).join("")); }
        return out;
    }

    // Layer names become channel prefixes ("diffuse.R"): letters, digits, _ and -.
    function layerSafe(name) {
        var s = String(name).replace(/[^A-Za-z0-9_\-]+/g, "_").replace(/^_+/, "").replace(/_+$/, "");
        return s === "" ? "layer" : s;
    }

    // "renders/v004/sh010_beauty_v004.####.exr" -> "renders/v005/sh010_beauty_v005.####.exr".
    // The version is the last vNN token of the file name (or of the path if the
    // file name has none); every identical token in the path is bumped.
    function bumpVersionPath(path) {
        var re = /(^|[._\-\/\\])([vV])(\d+)(?=$|[._\-\/\\])/g;
        var name = fileNameOf(path);
        var m, last = null;
        while ((m = re.exec(name)) !== null) { last = m; }
        if (!last) {
            re.lastIndex = 0;
            while ((m = re.exec(path)) !== null) { last = m; }
        }
        re.lastIndex = 0;
        if (!last) { return null; }
        var digits = last[3];
        var next = pad(parseInt(digits, 10) + 1, digits.length);
        return String(path).replace(re, function (all, sep, v, d) {
            return d === digits ? (sep + v + next) : all;
        });
    }

    // Layers inside an EXR: "ViewLayer.Combined.R" -> layer "ViewLayer.Combined".
    function layersOfChannels(channels) {
        var out = [], i, p, l;
        for (i = 0; i < channels.length; i++) {
            p = channels[i].lastIndexOf(".");
            l = p < 0 ? "" : channels[i].substr(0, p);
            if (inArray(out, l) < 0) { out.push(l); }
        }
        return out;
    }

    function channelsOfLayer(channels, layer) {
        var out = [], i, p, l;
        for (i = 0; i < channels.length; i++) {
            p = channels[i].lastIndexOf(".");
            l = p < 0 ? "" : channels[i].substr(0, p);
            if (l === layer) { out.push(channels[i].substr(p + 1)); }
        }
        return out;
    }

    function upperSet(list) {
        var o = {};
        for (var i = 0; i < list.length; i++) {
            var u = String(list[i]).toUpperCase();
            if (u === "RED") { u = "R"; } else if (u === "GREEN") { u = "G"; }
            else if (u === "BLUE") { u = "B"; } else if (u === "ALPHA") { u = "A"; }
            o[u] = 1;
        }
        return o;
    }

    function hasRGB(chs) { var s = upperSet(chs); return !!(s.R && s.G && s.B); }

    // Channel map for a pass (CLI syntax): "RGBA", "RGB", "Z", or "a->a,b->b".
    function defaultMap(chs) {
        var s = upperSet(chs);
        if (s.R && s.G && s.B) { return s.A ? "RGBA" : "RGB"; }
        var allSingle = true, i;
        for (i = 0; i < chs.length; i++) { if (!/^[A-Za-z]$/.test(chs[i])) { allSingle = false; } }
        if (!chs.length) { return "RGBA"; }
        if (allSingle) { return chs.join(""); }
        var parts = [];
        for (i = 0; i < chs.length; i++) { parts.push(chs[i] + "->" + chs[i]); }
        return parts.join(",");
    }

    // Bare channels win; else the first layer with RGB; else the first layer.
    function defaultLayer(channels) {
        var layers = layersOfChannels(channels);
        if (!layers.length || inArray(layers, "") >= 0) { return ""; }
        for (var i = 0; i < layers.length; i++) {
            if (hasRGB(channelsOfLayer(channels, layers[i]))) { return layers[i]; }
        }
        return layers[0];
    }

    // Same rule as the CLI: data passes (depth, normals, ...) and Z maps are float.
    function defaultType(name, map) {
        if (DATA_PASSES[String(name).toLowerCase()]) { return "float"; }
        if (/(^|,|>)[Zz]$/.test(map) || /->[Zz](,|$)/.test(map) || map === "Z") { return "float"; }
        return "half";
    }

    // ======================================================================
    // Setup (what the panel edits) -> job (what the CLI runs)
    // ======================================================================

    function defaultState() {
        return {
            version: 1,
            passes: [],          // {name, src, layer, map, type, main}
            crypto: { name: "CryptoMatte", mode: "priority", ranks: 6, background: false,
                      backgroundName: "background", objects: [] },   // objects: {name, src, layer, channel}
            source: "files",     // "files" | "comp": what the Add buttons add
            omTemplate: "",      // optional AE output-module template for temp renders (comp mode)
            output: "",
            compression: "zip",
            multipart: false,
            importCheck: true
        };
    }

    // Fill holes in a setup read from disk, so old or hand-edited files still load.
    function normaliseState(s) {
        var d = defaultState(), k;
        if (!s || typeof s !== "object") { return d; }
        for (k in d) { if (d.hasOwnProperty(k) && s[k] === undefined) { s[k] = d[k]; } }
        for (k in d.crypto) { if (d.crypto.hasOwnProperty(k) && s.crypto[k] === undefined) { s.crypto[k] = d.crypto[k]; } }
        if (!(isArray(s.passes))) { s.passes = []; }
        if (!(isArray(s.crypto.objects))) { s.crypto.objects = []; }
        return s;
    }

    function makePass(fileName, src, channels) {
        var name = layerSafe(nameFromFile(fileName));
        var layer = defaultLayer(channels || []);
        var map = defaultMap(channelsOfLayer(channels || [], layer));
        return { name: name, src: fwd(src), layer: layer, map: map, type: defaultType(name, map),
                 main: BEAUTY_ALIASES.test(name) };
    }

    // Layer rows render to sequences, so they count as sequences.
    function anySequence(s) {
        var rows = s.passes.concat(s.crypto.objects), i;
        for (i = 0; i < rows.length; i++) { if (rows[i].ref || /#/.test(rows[i].src)) { return true; } }
        return false;
    }

    function validateState(s) {
        var errors = [], warnings = [], i, seen = {}, mains = 0, n;
        var objs = s.crypto.objects;
        if (!s.passes.length && !objs.length) { errors.push("Add at least one pass or one matte."); }
        if (trim(s.output) === "") { errors.push("Choose an output file."); }
        else if (!/\.exr$/i.test(s.output)) { errors.push("The output must end in .exr."); }
        if (anySequence(s) && s.output && !/#/.test(s.output)) {
            errors.push("The inputs are sequences: the output needs #### for the frame number.");
        }
        for (i = 0; i < s.passes.length; i++) {
            n = s.passes[i].name;
            if (!/^[A-Za-z0-9_\-]+$/.test(n)) { errors.push("Pass name \"" + n + "\": use letters, digits, _ or - only."); }
            if (seen[n]) { errors.push("Two passes are called \"" + n + "\"."); }
            seen[n] = 1;
            if (s.passes[i].main) { mains++; }
            if (s.passes[i].type !== "half" && s.passes[i].type !== "float") { errors.push("Pass \"" + n + "\": type must be half or float."); }
        }
        if (mains > 1) { errors.push("Only one pass can be the main (bare RGBA) layer."); }
        if (s.passes.length && !mains && !seen.beauty) {
            warnings.push("No main pass: every pass gets a layer prefix and the file has no bare RGBA.");
        }
        if (objs.length) {
            var c = s.crypto;
            if (!/^[A-Za-z0-9_]+$/.test(c.name)) { errors.push("Cryptomatte name: letters, digits and _ only."); }
            if (c.ranks < 2 || c.ranks > 16 || c.ranks % 2) { errors.push("Ranks must be an even number from 2 to 16."); }
            if (inArray(MODES, c.mode) < 0) { errors.push("Unknown overlap mode " + c.mode + "."); }
            var names = {}, hashes = {}, h, nm;
            var all = [];
            for (i = 0; i < objs.length; i++) { all.push(objs[i].name); }
            if (c.background) { all.push(c.backgroundName); }
            for (i = 0; i < all.length; i++) {
                nm = all[i];
                if (trim(nm) === "") { errors.push("A matte has no name."); continue; }
                if (names[nm]) { errors.push("Two Cryptomatte objects are called \"" + nm + "\"."); continue; }
                names[nm] = 1;
                h = cryptoHex(nm);
                if (hashes[h]) { errors.push("\"" + nm + "\" and \"" + hashes[h] + "\" get the same Cryptomatte ID; rename one."); }
                hashes[h] = nm;
            }
            for (i = 0; i < objs.length; i++) {
                if (inArray(CRYPTO_CHANNELS, objs[i].channel) < 0) { errors.push("Matte \"" + objs[i].name + "\": channel must be R, G, B, A or L."); }
            }
            if (s.multipart) { warnings.push("Multi-part output: check that your reader finds the Cryptomatte in a multi-part file."); }
        }
        return { errors: errors, warnings: warnings };
    }

    // SPEC section 5 field names, exactly.
    function buildJob(s) {
        var job = {
            version: 1,
            output: fwd(s.output),
            frames: "auto",
            compression: s.compression,
            multipart: !!s.multipart,
            layers: [],
            cryptomattes: []
        };
        var i, p, l, mainName = null;
        for (i = 0; i < s.passes.length; i++) {
            p = s.passes[i];
            l = { name: p.name };
            if (p.main) { l.main = true; mainName = p.name; }
            l.src = fwd(p.src);
            if (p.layer) { l.layer = p.layer; }
            l.map = p.map || "auto";
            l.type = p.type;
            job.layers.push(l);
        }
        if (mainName) { job.metadata_from = mainName; }
        var c = s.crypto;
        if (c.objects.length) {
            var cm = { name: c.name, ranks: c.ranks, mode: c.mode,
                       background: c.background ? c.backgroundName : null, preview: false, objects: [] };
            for (i = 0; i < c.objects.length; i++) {
                var o = { name: c.objects[i].name, src: fwd(c.objects[i].src), channel: c.objects[i].channel };
                if (c.objects[i].layer) { o.layer = c.objects[i].layer; }
                cm.objects.push(o);
            }
            job.cryptomattes.push(cm);
        }
        return job;
    }

    // ----------------------------------------------------------------------
    // Rows that come from comp layers (0.3): row.ref = {compId, compName, layerId,
    // layerIndex, layerName}; row.src stays "" until the layer has been rendered.
    // ----------------------------------------------------------------------

    function isLayerRow(r) { return !!(r && r.ref); }

    // Nothing worth saving yet (no rows, no output): such a setup is not written as a new entry.
    function isEmptySetup(s) { return !s.passes.length && !s.crypto.objects.length && trim(s.output || "") === ""; }

    // Is this layer (and, for mattes, this channel) already a row?
    function hasLayerRow(rows, ref, channel) {
        var k = refKey(ref), i;
        for (i = 0; i < rows.length; i++) {
            if (rows[i].ref && refKey(rows[i].ref) === k && (channel === undefined || rows[i].channel === channel)) { return true; }
        }
        return false;
    }

    function refKey(ref) { return ref.compId + ":" + (ref.layerId !== undefined && ref.layerId !== null ? ref.layerId : "i" + ref.layerIndex); }

    function makeLayerPass(ref, isFirstMain) {
        var name = layerSafe(stripVersion(ref.layerName));
        var main = !!isFirstMain && BEAUTY_ALIASES.test(name);
        var map = main ? "RGBA" : "RGB";
        return { name: name, src: "", layer: "", map: map, type: defaultType(name, map), main: main, ref: ref };
    }

    // mode "auto": alpha, or luminance when the layer has no alpha; "rgb": names = [r, g, b].
    function makeLayerMattes(ref, mode, hasAlpha, names) {
        var base = stripVersion(ref.layerName) || "matte";
        if (mode === "rgb") {
            var out = [], chs = ["R", "G", "B"], i;
            for (i = 0; i < 3; i++) {
                if (names && trim(names[i] || "") === "") { continue; }
                out.push({ name: names ? trim(names[i]) : base + "_" + chs[i], src: "", layer: "", channel: chs[i], ref: ref });
            }
            return out;
        }
        return [{ name: base, src: "", layer: "", channel: hasAlpha ? "A" : "L", ref: ref }];
    }

    // Distinct layers that need a temp render, in first-use order.
    function layerRefsOf(s) {
        var seen = {}, out = [], rows = s.passes.concat(s.crypto.objects), i, k;
        for (i = 0; i < rows.length; i++) {
            if (!isLayerRow(rows[i])) { continue; }
            k = refKey(rows[i].ref);
            if (!seen[k]) { seen[k] = 1; out.push(rows[i].ref); }
        }
        return out;
    }

    // A copy of the setup with every layer row pointing at its rendered sequence.
    function resolveLayerSources(s, patterns) {
        var r = jsonParse(jsonStringify(s)), rows = r.passes.concat(r.crypto.objects), i, k;
        for (i = 0; i < rows.length; i++) {
            if (!isLayerRow(rows[i])) { continue; }
            k = refKey(rows[i].ref);
            if (!patterns[k]) { throw new Error("No render for layer \"" + rows[i].ref.layerName + "\"."); }
            rows[i].src = patterns[k];
            rows[i].layer = "";
        }
        return r;
    }

    function expandFrame(pattern, frame) {
        var m = /#+/.exec(pattern);
        if (!m) { return pattern; }
        return pattern.substr(0, m.index) + pad(frame, m[0].length) + pattern.substr(m.index + m[0].length);
    }

    // Frames every sequence has (stills = null, ignored). null when there is no sequence.
    function intersectFrames(lists) {
        var acc = null, i, j, set;
        for (i = 0; i < lists.length; i++) {
            if (!lists[i]) { continue; }
            if (acc === null) { acc = lists[i].slice(0); continue; }
            set = {};
            for (j = 0; j < lists[i].length; j++) { set[lists[i][j]] = 1; }
            var keep = [];
            for (j = 0; j < acc.length; j++) { if (set[acc[j]]) { keep.push(acc[j]); } }
            acc = keep;
        }
        if (acc) { acc.sort(function (a, b) { return a - b; }); }
        return acc;
    }

    // AE's getSettings(GetSettingsFormat.SPEC) describes each key; the allowed strings sit in
    // "enums" ({name: int}, AE 2026) or "possible values". Walk path (e.g. ["Depth"]) and return
    // the first allowed value matching re, or null. Tolerant of the exact spec shape.
    function specNode(spec, path) {
        var n = spec, i;
        for (i = 0; i < path.length; i++) {
            if (!n || typeof n !== "object" || n[path[i]] === undefined) { return null; }
            n = n[path[i]];
        }
        return n;
    }

    function specValues(node) {
        if (!node) { return []; }
        if (isArray(node)) { return node; }
        if (typeof node !== "object") { return []; }
        var keys = ["possible values", "possibleValues", "values", "possible value"], i, k, out;
        if (node.enums && typeof node.enums === "object" && !isArray(node.enums)) {   // AE 2026: {type:"int", enums:{name: n}}
            out = [];
            for (k in node.enums) { if (node.enums.hasOwnProperty(k)) { out.push(k); } }
            return out;
        }
        for (i = 0; i < keys.length; i++) { if (isArray(node[keys[i]])) { return node[keys[i]]; } }
        return [];
    }

    function pickSettingValue(spec, path, re) {
        var vals = specValues(specNode(spec, path)), i;
        for (i = 0; i < vals.length; i++) { if (re.test(String(vals[i]))) { return String(vals[i]); } }
        return null;
    }

    // { "a": { "b": v } } from path ["a", "b"].
    function nestSetting(path, v) {
        var o = {}, cur = o, i;
        for (i = 0; i < path.length - 1; i++) { cur[path[i]] = {}; cur = cur[path[i]]; }
        cur[path[path.length - 1]] = v;
        return o;
    }

    // ======================================================================
    // Row moves (lists are reordered by buttons: ScriptUI has no drag and drop)
    // ======================================================================

    function contains(sortedIdx, v) { return inArray(sortedIdx, v) >= 0; }

    // Moves the rows at idx one step (dir -1 up, +1 down); returns their new indices.
    function moveRows(arr, idx, dir) {
        var sel = idx.slice(0).sort(function (a, b) { return a - b; });
        var i, j, t;
        if (dir < 0) {
            for (i = 0; i < sel.length; i++) {
                j = sel[i];
                if (j > 0 && !contains(sel, j - 1)) {
                    t = arr[j - 1]; arr[j - 1] = arr[j]; arr[j] = t; sel[i] = j - 1;
                }
            }
        } else {
            for (i = sel.length - 1; i >= 0; i--) {
                j = sel[i];
                if (j < arr.length - 1 && !contains(sel, j + 1)) {
                    t = arr[j + 1]; arr[j + 1] = arr[j]; arr[j] = t; sel[i] = j + 1;
                }
            }
        }
        return sel;
    }

    function moveRowsToTop(arr, idx) {
        var sel = idx.slice(0).sort(function (a, b) { return a - b; });
        var top = [], rest = [], i;
        for (i = 0; i < arr.length; i++) { if (contains(sel, i)) { top.push(arr[i]); } else { rest.push(arr[i]); } }
        var all = top.concat(rest);
        for (i = 0; i < all.length; i++) { arr[i] = all[i]; }
        var out = [];
        for (i = 0; i < top.length; i++) { out.push(i); }
        return out;
    }

    function removeRows(arr, idx) {
        var sel = idx.slice(0).sort(function (a, b) { return b - a; });
        for (var i = 0; i < sel.length; i++) { arr.splice(sel[i], 1); }
    }

    // ======================================================================
    // Talking to jd_exrc
    // ======================================================================

    // Frames -> "A-B" pieces (contiguous runs, at most about maxChunks pieces).
    // minSize: never fewer frames than this per piece (each call costs seconds in AE).
    function planChunks(frames, maxChunks, minSize) {
        var f = frames.slice(0).sort(function (a, b) { return a - b; });
        var u = [], i;
        for (i = 0; i < f.length; i++) { if (!u.length || u[u.length - 1] !== f[i]) { u.push(f[i]); } }
        if (!u.length) { return []; }
        var size = Math.max(minSize || 1, Math.ceil(u.length / Math.max(1, maxChunks)));
        var out = [], start = u[0], prev = u[0], count = 1;
        for (i = 1; i <= u.length; i++) {
            if (i === u.length || u[i] !== prev + 1 || count >= size) {
                out.push(start + "-" + prev);
                if (i === u.length) { break; }
                start = u[i]; count = 0;
            }
            prev = u[i]; count++;
        }
        return out;
    }

    function unescapeJson(s) {
        try { return jsonParse("\"" + s + "\""); } catch (e) { return s; }
    }

    // A log line the strict parser refused: pick out the known fields one by one.
    function looseEvent(l) {
        var e = /"event"\s*:\s*"([^"]*)"/.exec(l);
        if (!e) { return null; }
        var o = { event: e[1] }, m;
        m = /"frame"\s*:\s*(-?\d+)/.exec(l); if (m) { o.frame = parseInt(m[1], 10); }
        m = /"file"\s*:\s*"((?:[^"\\]|\\.)*)"/.exec(l); if (m) { o.file = unescapeJson(m[1]); }
        m = /"msg"\s*:\s*"((?:[^"\\]|\\.)*)"/.exec(l); if (m) { o.msg = unescapeJson(m[1]); }
        return o;
    }

    // jd_exrc --json-log output plus the exit marker the panel's wrapper echoes.
    function parseCliOutput(text) {
        var lines = String(text).split(/\r?\n/), i, l, m;
        var res = { events: [], other: [], exit: null };
        for (i = 0; i < lines.length; i++) {
            l = trim(String(lines[i]).replace(/^\uFEFF/, ""));
            if (l === "") { continue; }
            m = /^__JDEXRC_EXIT=(-?\d+)/.exec(l);
            if (m) { res.exit = parseInt(m[1], 10); continue; }
            if (l.charAt(0) === "{") {
                var ev = null;
                try { ev = jsonParse(l); } catch (e) { ev = looseEvent(l); }
                if (ev) { res.events.push(ev); continue; }
            }
            res.other.push(l);
        }
        return res;
    }

    // TIFF header (binary string of the first bytes): first IFD's width, height, BitsPerSample,
    // SamplesPerPixel, SampleFormat (1 uint, 3 IEEE float), Compression. null if not a TIFF.
    function tiffHeaderInfo(b) {
        if (b.length < 8 || (b.substr(0, 2) !== "II" && b.substr(0, 2) !== "MM")) { return null; }
        var le = b.substr(0, 2) === "II";
        var u16 = function (o) { var a = b.charCodeAt(o), c = b.charCodeAt(o + 1); return le ? (a | (c << 8)) : ((a << 8) | c); };
        var u32 = function (o) { return le ? ((u16(o) + u16(o + 2) * 65536) >>> 0) : ((u16(o) * 65536 + u16(o + 2)) >>> 0); };
        if (u16(2) !== 42) { return null; }
        var r = { width: 0, height: 0, bits: 0, spp: 1, format: 1, compression: 1 };
        var ifd = u32(4), n, k, e, tag, type, count, first;
        if (ifd + 2 > b.length) { return null; }
        n = u16(ifd);
        for (k = 0; k < n && ifd + 2 + k * 12 + 12 <= b.length; k++) {
            e = ifd + 2 + k * 12;
            tag = u16(e); type = u16(e + 2); count = u32(e + 4);
            first = (type === 3) ? ((count <= 2) ? u16(e + 8) : u16(u32(e + 8))) : u32(e + 8);   // SHORT inline or at offset
            if (tag === 256) { r.width = first; } else if (tag === 257) { r.height = first; }
            else if (tag === 258) { r.bits = first; } else if (tag === 259) { r.compression = first; }
            else if (tag === 277) { r.spp = first; } else if (tag === 339) { r.format = first; }
        }
        r.isFloat32 = r.bits === 32 && r.format === 3;
        return r;
    }

    // Calibration frame: a solid of CALIB_RGB rendered with the user's template must come back unchanged.
    // pixelLines = the text lines of "jd_exrc pixel" ("R   0.5  0x3f000000" ...).
    var CALIB_RGB = [0.5, 0.25, 0.125];
    function calibrationCheck(pixelLines) {
        var got = {}, i, m, names = ["R", "G", "B"], r = { ok: true, measured: [], note: "" };
        for (i = 0; i < pixelLines.length; i++) {
            m = /^(\S+)\s+(\S+)\s+0x[0-9a-fA-F]{8}/.exec(trim(pixelLines[i]));
            if (m) { got[m[1]] = parseFloat(m[2]); }
        }
        for (i = 0; i < 3; i++) {
            var v = got[names[i]];
            r.measured.push(v === undefined ? null : v);
            if (v === undefined || Math.abs(v - CALIB_RGB[i]) > 0.01) { r.ok = false; }   // 0.01: 8 bpc projects quantize
        }
        if (!r.ok && r.measured[0] !== null) {
            var lin = Math.pow((CALIB_RGB[0] + 0.055) / 1.055, 2.4);   // sRGB decode of 0.5 = 0.214
            r.note = Math.abs(r.measured[0] - lin) < 0.01 ? "values were converted sRGB -> linear" : "values were changed";
        }
        return r;
    }

    // --check: one {"event":"check","frame":N,"msg":"PASS" | "FAIL: ..."} per written frame.
    function checkSummary(events) {
        var r = { passed: 0, failed: 0, firstFail: "" }, i, e;
        for (i = 0; i < events.length; i++) {
            e = events[i];
            if (e.event !== "check") { continue; }
            if (/^PASS/.test(e.msg || "")) { r.passed++; }
            else {
                r.failed++;
                if (!r.firstFail) { r.firstFail = (e.frame !== undefined ? "frame " + e.frame + ": " : "") + (e.msg || "FAIL"); }
            }
        }
        return r;
    }

    function exitMeaning(code) {
        var t = EXIT_TEXT[String(code)];
        return t ? t : ("exit code " + code);
    }

    function eventsOf(res, kind) {
        var out = [];
        for (var i = 0; i < res.events.length; i++) { if (res.events[i].event === kind) { out.push(res.events[i]); } }
        return out;
    }

    function firstError(res) {
        var e = eventsOf(res, "error");
        if (e.length) {
            var x = e[0];
            return (x.frame !== undefined ? "frame " + x.frame + ": " : "") + (x.msg || "error") +
                   (x.file ? " (" + fileNameOf(x.file) + ")" : "");
        }
        return res.other.length ? res.other[res.other.length - 1] : "";
    }

    // jd_exrc writes its own log (--log-file, flushed per line): AE's system.callSystem gives
    // cmd no console, and then a console program started by cmd loses its stdout even with
    // "> file" (0.2.1, reproduced with a detached cmd). The wrappers only add the exit code,
    // to the log and to stdout (cmd's own echo does reach callSystem).
    function cliArgs(args, logPath) {
        if (!logPath) { throw new Error("jd_exrc call without a log path"); }   // never a file called "undefined"
        return args.concat(["--log-file", logPath]);
    }

    // Windows: a .bat run as cmd /c ""file.bat"" (the launch line AE 2026 ran fine in 0.2.1;
    // a PowerShell launch returned at once with nothing in AE). % in arguments are doubled.
    function windowsBatch(exe, args, logPath) {
        var q = function (a) { return "\"" + String(a).replace(/%/g, "%%") + "\""; };
        var all = cliArgs(args, logPath), parts = [q(exe)];
        for (var i = 0; i < all.length; i++) { parts.push(q(all[i])); }
        return "@echo off\r\nchcp 65001 >nul\r\n" + parts.join(" ") + " >nul 2>&1\r\n" +
               ">> " + q(logPath) + " echo __JDEXRC_EXIT=%errorlevel%\r\n" +
               "echo __JDEXRC_EXIT=%errorlevel%\r\n";
    }

    function windowsBatchLauncher(batPath) { return "cmd /c \"\"" + batPath + "\"\""; }

    function posixCommand(exe, args, logPath) {
        var q = function (a) { return "'" + String(a).replace(/'/g, "'\\''") + "'"; };
        var all = cliArgs(args, logPath), parts = [q(exe)];
        for (var i = 0; i < all.length; i++) { parts.push(q(all[i])); }
        return parts.join(" ") + " >/dev/null 2>&1; rc=$?; echo __JDEXRC_EXIT=$rc >> " + q(logPath) +
               "; echo __JDEXRC_EXIT=$rc";
    }

    function pngInfoFromBytes(buf) {
        var i = buf.indexOf("IHDR");
        if (buf.indexOf("PNG") < 0 || i < 0 || i + 14 > buf.length) { return null; }
        var be = function (p) {
            return ((buf.charCodeAt(p) << 24) | (buf.charCodeAt(p + 1) << 16) | (buf.charCodeAt(p + 2) << 8) | buf.charCodeAt(p + 3)) >>> 0;
        };
        var type = buf.charCodeAt(i + 13);
        var CH = { "0": ["Y"], "2": ["R", "G", "B"], "3": ["R", "G", "B"], "4": ["Y", "A"], "6": ["R", "G", "B", "A"] };
        var ch = CH[String(type)];
        if (!ch) { return null; }
        return { channels: ch, width: be(i + 4), height: be(i + 8), colourType: type };
    }

    var core = {
        VERSION: VERSION, jsonStringify: jsonStringify, jsonParse: jsonParse, murmur3: murmur3,
        cryptoHash: cryptoHash, cryptoHex: cryptoHex, cryptoLayerKey: cryptoLayerKey, cryptoColour: cryptoColour,
        utf8Bytes: utf8Bytes, mul32: mul32, crc32: crc32, adler32: adler32, pngChip: pngChip,
        splitFrame: splitFrame, patternFromFrame: patternFromFrame, nameFromFile: nameFromFile,
        stripSharedPrefix: stripSharedPrefix, layerSafe: layerSafe, bumpVersionPath: bumpVersionPath,
        layersOfChannels: layersOfChannels, channelsOfLayer: channelsOfLayer, defaultMap: defaultMap,
        defaultLayer: defaultLayer, defaultType: defaultType, defaultState: defaultState,
        normaliseState: normaliseState, makePass: makePass, validateState: validateState, buildJob: buildJob,
        moveRows: moveRows, moveRowsToTop: moveRowsToTop, removeRows: removeRows, planChunks: planChunks,
        parseCliOutput: parseCliOutput, exitMeaning: exitMeaning, eventsOf: eventsOf, firstError: firstError,
        posixCommand: posixCommand,
        windowsBatch: windowsBatch, windowsBatchLauncher: windowsBatchLauncher, cliArgs: cliArgs,
        pngInfoFromBytes: pngInfoFromBytes, looseEvent: looseEvent,
        isLayerRow: isLayerRow, refKey: refKey, makeLayerPass: makeLayerPass, makeLayerMattes: makeLayerMattes,
        layerRefsOf: layerRefsOf, resolveLayerSources: resolveLayerSources, expandFrame: expandFrame,
        intersectFrames: intersectFrames, anySequence: anySequence, isEmptySetup: isEmptySetup, hasLayerRow: hasLayerRow,
        specValues: specValues, pickSettingValue: pickSettingValue, nestSetting: nestSetting, checkSummary: checkSummary,
        tiffHeaderInfo: tiffHeaderInfo, calibrationCheck: calibrationCheck
    };

    // Under node (tests): export the logic and stop before any After Effects call.
    if (typeof app === "undefined") {
        if (typeof module !== "undefined" && module.exports) { module.exports = core; }
        return core;
    }

    // ======================================================================
    // After Effects from here on
    // ======================================================================

    var IS_WIN = $.os.indexOf("Windows") !== -1;
    var PREF_HELP = "After Effects does not let scripts write files or run programs yet.\n\n" +
        "Turn on: Edit > Preferences > Scripting & Expressions > " +
        "\"Allow Scripts to Write Files and Access Network\" (on macOS: After Effects > Settings), " +
        "then press Compile again.";

    // ---------------------------------------------------------------- files

    function tempFolder() {
        var f = new Folder(Folder.temp.fsName + "/jd_EXRCompiler");
        if (!f.exists) { f.create(); }
        return f;
    }

    // UTF-8 written byte by byte; a BOM only when asked (PowerShell scripts want one).
    function writeText(file, text, bom) {
        var b = utf8Bytes(text), s = bom ? "\u00EF\u00BB\u00BF" : "", i;
        for (i = 0; i < b.length; i++) { s += String.fromCharCode(b[i]); }
        file.encoding = "BINARY";
        if (!file.open("w")) { throw new Error("Cannot write " + file.fsName); }
        try { file.write(s); } finally { file.close(); }
    }

    function readText(file) {
        file.encoding = "UTF-8";
        if (!file.open("r")) { throw new Error("Cannot read " + file.fsName); }
        try { return file.read(); } finally { file.close(); }
    }

    function readBytes(file, maxBytes) {
        file.encoding = "BINARY";
        if (!file.open("r")) { throw new Error("Cannot open " + file.fsName); }
        try { return file.read(maxBytes); } finally { file.close(); }
    }

    // Probe: the pref, then an actual write (pref names differ between versions).
    function scriptsMayWriteFiles() {
        var sections = ["Main Pref Section v2", "Main Pref Section"], i;
        for (i = 0; i < sections.length; i++) {
            try {
                if (app.preferences.havePref(sections[i], "Pref_SCRIPTING_FILE_NETWORK_SECURITY")) {
                    return app.preferences.getPrefAsLong(sections[i], "Pref_SCRIPTING_FILE_NETWORK_SECURITY") === 1;
                }
            } catch (e) { /* try the next one */ }
        }
        try {
            var f = new File(tempFolder().fsName + "/probe.txt");
            writeText(f, "ok");
            f.remove();
            return true;
        } catch (e2) { return false; }
    }

    function findExe() {
        var here = null;
        try { here = new File($.fileName).parent; } catch (e) { here = null; }
        var cands = [], i, j, f;
        if (here) {
            for (j = 0; j < EXE_NAMES.length; j++) {
                cands.push(here.fsName + "/" + EXE_NAMES[j]);
                cands.push(here.fsName + "/jd_EXRCompiler/" + EXE_NAMES[j]);
                cands.push(here.fsName + "/../cli/build/cli/Release/" + EXE_NAMES[j]);
            }
        }
        for (i = 0; i < cands.length; i++) {
            f = new File(cands[i]);
            if (f.exists && (IS_WIN ? /\.exe$/i.test(f.name) : !/\.exe$/i.test(f.name))) { return f; }
        }
        return null;
    }

    var runCount = 0;

    // Runs jd_exrc and blocks AE until it returns (system.callSystem has no async form).
    // Every call of a compile is appended to temp/jd_EXRCompiler/last_compile.log (args, time, output).
    function debugLog(text, reset) {
        try {
            var f = new File(tempFolder().fsName + "/last_compile.log");
            f.encoding = "UTF-8";
            if (!f.open(reset ? "w" : "a")) { return; }
            try { f.write(text + "\n"); } finally { f.close(); }
        } catch (e) { /* diagnostics only */ }
    }

    function runCli(exe, args) {
        runCount++;
        var tmp = tempFolder().fsName;
        var log = new File(tmp + "/run_" + runCount + ".log");
        if (log.exists) { log.remove(); }
        var out, script = null, how, t0 = new Date().getTime();
        if (IS_WIN) {
            how = "bat";
            script = new File(tmp + "/run_" + runCount + ".bat");
            writeText(script, windowsBatch(exe.fsName, args, log.fsName));
            out = system.callSystem(windowsBatchLauncher(script.fsName));
        } else {
            how = "sh";
            out = system.callSystem("/bin/sh -c " + "'" + posixCommand(exe.fsName, args, log.fsName).replace(/'/g, "'\\''") + "'");
        }
        var ms = new Date().getTime() - t0;
        var text = "";
        try { if (log.exists) { text = readText(log); } } catch (e) { text = ""; }
        if (!/__JDEXRC_EXIT=/.test(text)) { text += "\n" + (out || ""); }   // no marker in the log: use stdout's
        debugLog("> jd_exrc " + args.join(" ") + "   (" + ms + " ms, via " + how + ")\n" + text +
                 "\n[callSystem stdout]\n" + (out || "") + "\n");
        var res = parseCliOutput(text);
        if (res.exit === null) {   // keep the files of a failed call for inspection
            debugLog("kept for inspection: " + (script ? script.fsName + "  " : "") + log.fsName + "\ncallSystem line: " +
                     (how === "bat" ? windowsBatchLauncher(script.fsName) : "(sh)"));
        } else {
            try { if (script) { script.remove(); } log.remove(); } catch (e2) { /* leave them in temp */ }
        }
        res.ms = ms;
        return res;
    }

    // ---------------------------------------------------------------- image headers

    function readCString(buf, st) {
        var start = st.pos;
        while (st.pos < buf.length && buf.charCodeAt(st.pos) !== 0) { st.pos++; }
        var s = buf.substring(start, st.pos);
        st.pos++;
        return s;
    }

    function readInt32(buf, pos) {
        return (buf.charCodeAt(pos)) | (buf.charCodeAt(pos + 1) << 8) |
               (buf.charCodeAt(pos + 2) << 16) | (buf.charCodeAt(pos + 3) << 24);
    }

    // EXR header: channel names (part-prefixed for multi-part) and display size.
    function readExrInfo(file) {
        var buf = readBytes(file, 512 * 1024);
        if (buf.length < 8 || buf.charCodeAt(0) !== 0x76 || buf.charCodeAt(1) !== 0x2F ||
            buf.charCodeAt(2) !== 0x31 || buf.charCodeAt(3) !== 0x01) {
            throw new Error(file.name + " is not an OpenEXR file.");
        }
        var multi = (readInt32(buf, 4) & 0x1000) !== 0;
        var st = { pos: 8 }, channels = [], width = 0, height = 0, guard = 0;
        while (st.pos < buf.length && guard++ < 128) {
            var partName = "", partCh = [], attrs = 0;
            while (st.pos < buf.length && attrs++ < 512) {
                var an = readCString(buf, st);
                if (an === "") { break; }
                var at = readCString(buf, st);
                var size = readInt32(buf, st.pos);
                var start = st.pos + 4;
                if (size < 0 || start + size > buf.length) { throw new Error(file.name + ": EXR header too large to read."); }
                if (an === "channels" && at === "chlist") {
                    var cs = { pos: start };
                    while (cs.pos < start + size) {
                        var cn = readCString(buf, cs);
                        if (cn === "") { break; }
                        cs.pos += 16;
                        partCh.push(cn);
                    }
                } else if (an === "name" && at === "string") {
                    partName = buf.substr(start, size);
                } else if (an === "displayWindow" && at === "box2i" && !width) {
                    width = readInt32(buf, start + 8) - readInt32(buf, start) + 1;
                    height = readInt32(buf, start + 12) - readInt32(buf, start + 4) + 1;
                }
                st.pos = start + size;
            }
            for (var i = 0; i < partCh.length; i++) {
                var full = (multi && partName !== "" && partCh[i].indexOf(partName + ".") !== 0) ?
                           partName + "." + partCh[i] : partCh[i];
                if (inArray(channels, full) < 0) { channels.push(full); }
            }
            if (!multi || st.pos >= buf.length || buf.charCodeAt(st.pos) === 0) { break; }
        }
        return { channels: channels, width: width, height: height };
    }

    // PNG: channels named the way the CLI names them (Y, YA, RGB, RGBA).
    function readPngInfo(file) {
        var info = pngInfoFromBytes(readBytes(file, 64));
        if (!info) { throw new Error(decodeURI(file.name) + ": not a PNG this panel can read (header not found)."); }
        return info;
    }

    // TIFF: channels named the way the CLI names them (Y, YA, RGB, RGBA).
    function tiffChannels(spp) {
        return [[], ["Y"], ["Y", "A"], ["R", "G", "B"], ["R", "G", "B", "A"]][spp] ||
               ["R", "G", "B", "A"].concat((function () { var x = [], i; for (i = 4; i < spp; i++) { x.push("C" + i); } return x; })());
    }
    function readTiffInfo(file) {
        var t = tiffHeaderInfo(readBytes(file, 65536));
        if (!t || !t.width) { throw new Error(decodeURI(file.name) + ": not a TIFF this panel can read (header not found)."); }
        var okInt = t.format === 1 && (t.bits === 8 || t.bits === 16), okFloat = t.format === 3 && (t.bits === 16 || t.bits === 32);
        if (!okInt && !okFloat) {
            throw new Error(decodeURI(file.name) + ": " + t.bits + "-bit " + (t.format === 3 ? "float" : "integer") +
                            " TIFF is not supported (8/16-bit integer, 16/32-bit float).");
        }
        return { channels: tiffChannels(t.spp), width: t.width, height: t.height };
    }

    function readImageInfo(file) {
        if (/\.exr$/i.test(file.name)) { return readExrInfo(file); }
        if (/\.png$/i.test(file.name)) { return readPngInfo(file); }
        if (/\.tiff?$/i.test(file.name)) { return readTiffInfo(file); }
        throw new Error(file.name + ": only EXR, PNG and TIFF are read.");
    }

    // ---------------------------------------------------------------- sequences

    function regexEscape(s) { return String(s).replace(/[\\^$.*+?()[\]{}|\/]/g, "\\$&"); }

    // A picked file -> its sequence pattern (if 2+ frames exist) and frame range.
    function sequenceOfFile(file) {
        var sp = splitFrame(file.name);
        var single = { src: fwd(file.fsName), first: null, last: null, count: 1, firstFile: file };
        if (!sp) { return single; }
        var re = new RegExp("^" + regexEscape(sp.head) + "(\\d{" + sp.digits.length + "})" + regexEscape(sp.ext) + "$", "i");
        var kids = file.parent.getFiles(), frames = [], firstName = null, i, m;
        for (i = 0; i < kids.length; i++) {
            if (!(kids[i] instanceof File)) { continue; }
            m = re.exec(decodeURI(kids[i].name));
            if (m) {
                var f = parseInt(m[1], 10);
                if (!frames.length || f < Math.min.apply(null, frames)) { firstName = kids[i]; }
                frames.push(f);
            }
        }
        if (frames.length < 2) { return single; }
        frames.sort(function (a, b) { return a - b; });
        return { src: fwd(file.parent.fsName) + "/" + patternFromFrame(file.displayName || decodeURI(file.name)),
                 first: frames[0], last: frames[frames.length - 1], count: frames.length, firstFile: firstName };
    }

    // A pattern on disk -> the same info (for rows loaded from a saved setup).
    function sequenceOfPattern(src) {
        if (!/#/.test(src)) {
            var f = new File(src);
            return f.exists ? { src: src, first: null, last: null, count: 1, firstFile: f } : null;
        }
        var folder = new Folder(folderOf(src));
        if (!folder.exists) { return null; }
        var name = fileNameOf(src);
        var a = name.indexOf("#"), b = name.lastIndexOf("#");
        var re = new RegExp("^" + regexEscape(name.substr(0, a)) + "(\\d{" + (b - a + 1) + "})" +
                            regexEscape(name.substr(b + 1)) + "$", "i");
        var kids = folder.getFiles(), frames = [], first = null, i, m, fr;
        for (i = 0; i < kids.length; i++) {
            if (!(kids[i] instanceof File)) { continue; }
            m = re.exec(decodeURI(kids[i].name));
            if (m) {
                fr = parseInt(m[1], 10);
                if (first === null || fr < first.frame) { first = { frame: fr, file: kids[i] }; }
                frames.push(fr);
            }
        }
        if (!frames.length) { return null; }
        frames.sort(function (x, y) { return x - y; });
        return { src: src, first: frames[0], last: frames[frames.length - 1], count: frames.length, firstFile: first.file,
                 frames: frames };
    }

    // Frames the CLI will compile, worked out here (replaces the old --dry-run call).
    // null = only stills; [] = the sequences share no frame (the CLI will say so).
    function framesOfState(s) {
        var lists = [], seen = {}, rows = s.passes.concat(s.crypto.objects), i, src, seq;
        for (i = 0; i < rows.length; i++) {
            src = rows[i].src;
            if (seen[src]) { continue; }
            seen[src] = 1;
            if (!/#/.test(src)) { lists.push(null); continue; }
            seq = sequenceOfPattern(src);
            lists.push(seq ? seq.frames : []);
        }
        return intersectFrames(lists);
    }

    var infoCache = {};

    function sourceInfo(src) {
        if (infoCache[src]) { return infoCache[src]; }
        var seq = sequenceOfPattern(src);
        if (!seq) { return null; }
        try { var hi = readImageInfo(seq.firstFile); seq.channels = hi.channels; seq.width = hi.width; seq.height = hi.height; }
        catch (e) { seq.channels = []; seq.error = e.message; }
        infoCache[src] = seq;
        return seq;
    }

    // ---------------------------------------------------------------- setups per comp

    function activeComp() {
        var it = app.project ? app.project.activeItem : null;
        return (it && it instanceof CompItem) ? it : null;
    }

    function setupFile() {
        if (app.project && app.project.file) {
            var pf = app.project.file;
            var base = decodeURI(pf.name).replace(/\.aepx?$/i, "");
            return new File(pf.parent.fsName + "/" + base + SETUP_SUFFIX);
        }
        var f = new Folder(Folder.userData.fsName + "/jd_EXRCompiler");
        if (!f.exists) { f.create(); }
        return new File(f.fsName + "/unsaved_project" + SETUP_SUFFIX);
    }

    function readStore() {
        var f = setupFile();
        if (!f.exists) { return { tool: TOOL, version: 1, setups: {} }; }
        try {
            var s = jsonParse(readText(f));
            if (!s.setups) { s.setups = {}; }
            return s;
        } catch (e) { return { tool: TOOL, version: 1, setups: {} }; }
    }

    function compKey(comp) { return comp ? ("comp_" + comp.id) : "no_comp"; }

    // ======================================================================
    // UI
    // ======================================================================

    var state = defaultState();
    var currentStoreFile = "";   // the setup file this state was loaded from (saved back only there)
    var currentKey = "no_comp";
    var currentCompName = "";
    var passView = [], cryptoView = [];
    var ui = {};

    function status(text, tip) {
        ui.status.text = text;
        ui.status.helpTip = tip || text;
    }

    function saveSetup() {
        try {
            var store = readStore();
            if (isEmptySetup(state) && !store.setups[currentKey]) { return true; }   // nothing to keep yet (0.3.5 finding)
            var s = copyObj(state);
            s.compName = currentCompName;
            s.savedWith = TOOL + " " + VERSION;
            store.setups[currentKey] = s;
            store.tool = TOOL;
            // Another project is open now: do not write this state into its file (0.3.3 finding).
            // (A never-saved project that got saved moves its setup from the prefs file along with it.)
            var target = setupFile().fsName;
            if (currentStoreFile && target !== currentStoreFile && !/unsaved_project/.test(currentStoreFile)) { return false; }
            writeText(setupFile(), jsonStringify(store, 2));
            currentStoreFile = target;
            return true;
        } catch (e) { return false; }   // usually the scripting pref is off; Compile explains that
    }

    function loadSetupForActiveComp(saveFirst) {
        if (saveFirst) { saveSetup(); }
        var comp = activeComp();
        currentKey = compKey(comp);
        currentCompName = comp ? comp.name : "";
        var store;
        try { store = readStore(); } catch (e) { store = { setups: {} }; }
        state = normaliseState(store.setups[currentKey] ? copyObj(store.setups[currentKey]) : defaultState());
        currentStoreFile = setupFile().fsName;
        syncControls();
        refreshAll();
        status(store.setups[currentKey] ? "Setup loaded for " + (currentCompName || "this project") + "." :
               "New setup for " + (currentCompName || "this project (no comp active)") + ".");
    }

    function lastFolder() {
        try {
            if (app.settings.haveSetting(SETTINGS_SECTION, "lastFolder")) {
                var f = new Folder(app.settings.getSetting(SETTINGS_SECTION, "lastFolder"));
                if (f.exists) { return f; }
            }
        } catch (e) { /* ignore */ }
        return Folder.desktop;
    }

    function rememberFolder(f) {
        try { app.settings.saveSetting(SETTINGS_SECTION, "lastFolder", f.fsName); } catch (e) { /* ignore */ }
    }

    function pickFiles(prompt, multi) {
        var probe = new File(lastFolder().fsName + "/pick");
        var filter = IS_WIN ? "Images:*.exr;*.png;*.tif;*.tiff,All files:*.*" : function (f) {
            return (f instanceof Folder) || /\.(exr|png|tiff?)$/i.test(f.name);
        };
        var res = probe.openDlg(prompt, filter, multi);
        if (!res) { return []; }
        if (!(isArray(res))) { res = [res]; }
        if (res.length) { rememberFolder(res[0].parent); }
        return res;
    }

    // ---------------------------------------------------------------- list helpers

    function selectedModel(list, view) {
        var s = list.selection, out = [], i;
        if (!s) { return out; }
        if (!(isArray(s))) { s = [s]; }
        for (i = 0; i < s.length; i++) { out.push(view[s[i].index]); }
        return out.sort(function (a, b) { return a - b; });
    }

    function restoreSelection(list, view, modelIdx) {
        var sel = [];
        for (var i = 0; i < view.length; i++) { if (inArray(modelIdx, view[i]) >= 0) { sel.push(i); } }
        if (sel.length) { try { list.selection = sel; } catch (e) { /* ignore */ } }
    }

    function shortSrc(src, layer) {
        return fileNameOf(src) + (layer ? "  [" + layer + "]" : "");
    }

    function rowSource(r) {
        return r.ref ? ("comp " + r.ref.compName + " > " + r.ref.layerName) : shortSrc(r.src, r.layer);
    }

    function chipImage(hex, rgb) {
        try {
            var dir = new Folder(tempFolder().fsName + "/chips");
            if (!dir.exists) { dir.create(); }
            var f = new File(dir.fsName + "/" + hex + ".png");
            if (!f.exists) {
                f.encoding = "BINARY";
                if (!f.open("w")) { return null; }
                try { f.write(pngChip(rgb, 18, 12)); } finally { f.close(); }
            }
            return ScriptUI.newImage(f);
        } catch (e) { return null; }
    }

    function fillPassList(keepSel) {
        var list = ui.passList, flt = trim(ui.passFilter.text).toLowerCase(), i, p, label, it;
        list.removeAll();
        passView = [];
        for (i = 0; i < state.passes.length; i++) {
            p = state.passes[i];
            label = p.name + "   \u2190  " + rowSource(p);
            if (flt && (label + " " + p.map + " " + p.type).toLowerCase().indexOf(flt) < 0) { continue; }
            it = list.add("item", p.main ? "\u2605" : "");
            it.subItems[0].text = label;
            it.subItems[1].text = p.map;
            it.subItems[2].text = p.type;
            passView.push(i);
        }
        if (keepSel) { restoreSelection(list, passView, keepSel); }
    }

    function fillCryptoList(keepSel) {
        var list = ui.cryptoList, flt = trim(ui.cryptoFilter.text).toLowerCase(), objs = state.crypto.objects;
        var i, o, label, it, hex, img;
        list.removeAll();
        cryptoView = [];
        for (i = 0; i < objs.length; i++) {
            o = objs[i];
            hex = cryptoHex(o.name);
            label = o.name + "   \u2190  " + rowSource(o);
            if (flt && (label + " " + o.channel + " " + hex).toLowerCase().indexOf(flt) < 0) { continue; }
            it = list.add("item", String(i + 1));
            img = chipImage(hex, cryptoColour(o.name));
            if (img) { try { it.subItems[0].image = img; } catch (e) { /* text only */ } }
            it.subItems[0].text = hex;
            it.subItems[1].text = label;
            it.subItems[2].text = o.channel;
            cryptoView.push(i);
        }
        if (keepSel) { restoreSelection(list, cryptoView, keepSel); }
    }

    function updateCounts() {
        ui.tabPasses.text = "Passes (" + state.passes.length + ")";
        ui.tabCrypto.text = "Cryptomatte (" + state.crypto.objects.length + ")";
    }

    function updateInfo() {
        var parts = ["Setup: " + (currentCompName ? currentCompName : "no comp active")];
        var row = state.passes.length ? state.passes[0] : (state.crypto.objects.length ? state.crypto.objects[0] : null);
        var src = row ? row.src : "";
        if (row && row.ref) {
            var lay = findLayer(row.ref);
            if (!lay) { parts.push("layer " + row.ref.layerName + " not found"); }
            else {
                var cf = compFrames(lay.containingComp);
                parts.push("comp " + lay.containingComp.name + ": frames " + cf.first + "-" + cf.last + " (" + cf.count + ")");
                parts.push(lay.containingComp.width + "x" + lay.containingComp.height);
            }
        } else if (src) {
            var inf = sourceInfo(src);
            if (!inf) { parts.push("first source not found"); }
            else {
                parts.push(inf.first !== null ? ("frames " + inf.first + "-" + inf.last + " (" + inf.count + ")") : "single frame");
                if (inf.width) { parts.push(inf.width + "x" + inf.height); }
            }
        }
        ui.info.text = parts.join("   |   ");
        ui.info.helpTip = ui.info.text;
    }

    function refreshAll(selP, selC) {
        fillPassList(selP);
        fillCryptoList(selC);
        updateCounts();
        updateInfo();
    }

    function changed(selP, selC) {
        refreshAll(selP, selC);
        saveSetup();
    }

    function syncControls() {
        setSource(state.source === "comp" ? "comp" : "files", true);
        ui.output.text = state.output;
        ui.compression.selection = Math.max(0, inArray(COMPRESSIONS, state.compression));
        ui.multipart.value = !!state.multipart;
        ui.importCheck.value = !!state.importCheck;
        ui.mode.selection = Math.max(0, inArray(MODES, state.crypto.mode));
        ui.ranks.selection = Math.max(0, inArray(RANKS, state.crypto.ranks));
        ui.bg.value = !!state.crypto.background;
        ui.bgName.text = state.crypto.backgroundName;
        ui.bgName.enabled = ui.bg.value;
        ui.cryptoName.text = state.crypto.name;
    }

    function suggestOutputForComp(comp) {
        if (trim(state.output) !== "") { return; }
        // Named after project + comp, so projects sharing a folder do not write over each other's output.
        var proj = app.project.file ? layerSafe(decodeURI(app.project.file.name).replace(/\.aepx?$/i, "")) : "";
        var dir = app.project.file ? fwd(app.project.file.parent.fsName) + "/jd_EXRCompiler" : fwd(Folder.desktop.fsName) + "/jd_EXRCompiler";
        state.output = dir + "/" + (proj ? proj + "_" : "") + layerSafe(comp.name) + "_compiled.####.exr";
        ui.output.text = state.output;
    }

    function suggestOutput(src) {
        if (trim(state.output) !== "") { return; }
        var base = layerSafe(nameFromFile(fileNameOf(src)));
        if (currentCompName) { base = layerSafe(currentCompName); }
        state.output = folderOf(src) + "/compiled/" + base + "_compiled" + (/#/.test(src) ? ".####" : "") + ".exr";
        ui.output.text = state.output;
    }

    // ---------------------------------------------------------------- passes actions

    function addPasses() {
        var files = pickFiles("Add passes (EXR, PNG or TIFF; pick one frame of each sequence)", true);
        if (!files.length) { return; }
        var newRows = [], names = [], i, seq, info;
        for (i = 0; i < files.length; i++) {
            seq = sequenceOfFile(files[i]);
            try { info = readImageInfo(seq.firstFile); } catch (e) { alert(e.message); continue; }
            var row = makePass(decodeURI(files[i].name), seq.src, info.channels);
            newRows.push(row);
            names.push(row.name);
        }
        names = stripSharedPrefix(names);
        var hasMain = false, sel = [];
        for (i = 0; i < state.passes.length; i++) { if (state.passes[i].main) { hasMain = true; } }
        for (i = 0; i < newRows.length; i++) {
            newRows[i].name = layerSafe(names[i]);
            if (BEAUTY_ALIASES.test(newRows[i].name) && !hasMain) { newRows[i].main = true; hasMain = true; }
            else { newRows[i].main = false; }
            newRows[i].type = defaultType(newRows[i].name, newRows[i].map);
            sel.push(state.passes.length);
            state.passes.push(newRows[i]);
        }
        if (newRows.length) { suggestOutput(newRows[0].src); }
        ui.passFilter.text = "";
        changed(sel, null);
        status("Added " + newRows.length + " pass(es). Double-click a row to edit it.");
    }

    function removePasses() {
        var sel = selectedModel(ui.passList, passView);
        if (!sel.length) { status("Select passes to remove."); return; }
        removeRows(state.passes, sel);
        changed(null, null);
    }

    function movePasses(dir) {
        var sel = selectedModel(ui.passList, passView);
        if (!sel.length) { return; }
        changed(moveRows(state.passes, sel, dir), null);
    }

    function sourceExists(src) {
        var inf = sequenceOfPattern(src);
        return !!inf;
    }

    function bumpPasses() {
        var sel = selectedModel(ui.passList, passView);
        if (!sel.length) { status("Select the passes to move to the next version."); return; }
        var done = 0, missing = [], noVer = [], i, p, nxt;
        for (i = 0; i < sel.length; i++) {
            p = state.passes[sel[i]];
            if (p.ref) { noVer.push(p.name + " (comp layer)"); continue; }
            nxt = bumpVersionPath(p.src);
            if (!nxt) { noVer.push(p.name); continue; }
            if (!sourceExists(nxt)) { missing.push(fileNameOf(nxt)); continue; }
            p.src = nxt;
            done++;
        }
        infoCache = {};
        changed(sel, null);
        var msg = "Version bumped on " + done + " pass(es).";
        if (missing.length) { msg += " Not found (left as is): " + missing.join(", ") + "."; }
        if (noVer.length) { msg += " No _vNN in: " + noVer.join(", ") + "."; }
        status(msg);
    }

    function layerDropdown(parent, channels, current) {
        var layers = layersOfChannels(channels || []);
        if (inArray(layers, current) < 0) { layers.push(current); }
        var labels = [];
        for (var i = 0; i < layers.length; i++) { labels.push(layers[i] === "" ? "(bare channels)" : layers[i]); }
        var dd = parent.add("dropdownlist", undefined, labels);
        dd.selection = Math.max(0, inArray(layers, current));
        dd.layers = layers;
        return dd;
    }

    function labelled(parent, label, w) {
        var g = parent.add("group");
        g.orientation = "row";
        g.alignChildren = ["left", "center"];
        var t = g.add("statictext", undefined, label);
        t.preferredSize.width = w || 70;
        return g;
    }

    function editPass() {
        var sel = selectedModel(ui.passList, passView);
        if (sel.length !== 1) { status("Select one pass to edit."); return; }
        var p = state.passes[sel[0]];
        if (p.ref) { editLayerPass(p, sel); return; }
        var inf = sourceInfo(p.src);
        var d = new Window("dialog", "Edit pass");
        d.orientation = "column";
        d.alignChildren = ["fill", "top"];
        var gN = labelled(d, "Name");
        var eName = gN.add("edittext", undefined, p.name); eName.characters = 30;
        var gS = labelled(d, "Source");
        var eSrc = gS.add("edittext", undefined, p.src); eSrc.characters = 40;
        var bSrc = gS.add("button", undefined, "Browse...");
        var gL = labelled(d, "Layer in file");
        var dLayer = layerDropdown(gL, inf ? inf.channels : [], p.layer);
        var gC = labelled(d, "Channels");
        var eMap = gC.add("edittext", undefined, p.map); eMap.characters = 20;
        gC.add("statictext", undefined, "RGBA, RGB, Z or R->Z,G->W");
        var gT = labelled(d, "Type");
        var dType = gT.add("dropdownlist", undefined, ["half", "float"]);
        dType.selection = p.type === "float" ? 1 : 0;
        var cMain = d.add("checkbox", undefined, "\u2605 Main layer (written as bare R G B A)");
        cMain.value = !!p.main;
        var gB = d.add("group"); gB.alignment = ["right", "top"];
        gB.add("button", undefined, "Cancel", { name: "cancel" });
        var ok = gB.add("button", undefined, "OK", { name: "ok" });

        bSrc.onClick = function () {
            var f = pickFiles("Pick one frame of the new source", false);
            if (!f.length) { return; }
            var seq = sequenceOfFile(f[0]);
            eSrc.text = seq.src;
            try {
                var hi = readImageInfo(seq.firstFile);
                var ly = defaultLayer(hi.channels);
                var parent = dLayer.parent;
                parent.remove(dLayer);
                dLayer = layerDropdown(parent, hi.channels, ly);
                eMap.text = defaultMap(channelsOfLayer(hi.channels, ly));
                d.layout.layout(true);
            } catch (e) { alert(e.message); }
        };
        ok.onClick = function () {
            var nm = layerSafe(trim(eName.text));
            for (var i = 0; i < state.passes.length; i++) {
                if (i !== sel[0] && state.passes[i].name === nm) { alert("Another pass is already called " + nm + "."); return; }
            }
            p.name = nm;
            p.src = fwd(trim(eSrc.text));
            p.layer = dLayer.layers[dLayer.selection ? dLayer.selection.index : 0];
            p.map = trim(eMap.text) || "auto";
            p.type = dType.selection.text;
            if (cMain.value) { for (var j = 0; j < state.passes.length; j++) { state.passes[j].main = false; } }
            p.main = cMain.value;
            d.close(1);
        };
        if (d.show() === 1) { infoCache = {}; changed(sel, null); }
    }

    // ---------------------------------------------------------------- cryptomatte actions

    // RGB matte: split into objects R/G/B (+A), or one object (R or luma).
    // namesOnly (comp layers): only the R/G/B names are asked.
    function askMatteSplit(fileLabel, baseName, hasA, namesOnly) {
        var d = new Window("dialog", "Matte: " + fileLabel);
        d.orientation = "column";
        d.alignChildren = ["fill", "top"];
        // Two plain lines with a fixed width: a single long statictext was clipped in AE 2026.
        var head1 = d.add("statictext", undefined, fileLabel);
        var head2 = d.add("statictext", undefined, namesOnly ? "Names of its R, G, B objects:" :
                                                    "has R, G and B channels. How should it become objects?");
        // minimumSize too: with the other choices hidden (namesOnly) AE 2026 shrank the line to its fill width.
        head1.preferredSize = head1.minimumSize = [420, 18];
        head2.preferredSize = head2.minimumSize = [420, 18];
        d.minimumSize = [460, 100];
        var rSplit = d.add("radiobutton", undefined, "One object per channel (RGB matte)");
        var gNames = d.add("panel", undefined, "Object names");
        gNames.alignChildren = ["fill", "top"];
        var fields = [], chs = ["R", "G", "B", "A"], i;
        for (i = 0; i < 4; i++) {
            var g = labelled(gNames, chs[i], 20);
            fields.push(g.add("edittext", undefined, baseName + "_" + chs[i]));
            fields[i].characters = 28;
        }
        var cA = gNames.add("checkbox", undefined, "Also use A as a fourth object");
        cA.enabled = hasA;
        fields[3].enabled = false;
        var rBW = d.add("radiobutton", undefined, "One object from R (black-and-white matte)");
        var rL = d.add("radiobutton", undefined, "One object from luminance (L)");
        var gOne = labelled(d, "Name");
        var eOne = gOne.add("edittext", undefined, baseName); eOne.characters = 28;
        rSplit.value = true;
        if (namesOnly) { rSplit.visible = rBW.visible = rL.visible = gOne.visible = false; cA.visible = false; fields[3].parent.visible = false; }
        var sync = function () {
            for (var k = 0; k < 3; k++) { fields[k].enabled = rSplit.value; }
            cA.enabled = rSplit.value && hasA;
            fields[3].enabled = cA.enabled && cA.value;
            eOne.enabled = !rSplit.value;
        };
        // The radios are not neighbours (the names panel sits between them), so ScriptUI
        // does not make them exclusive by itself: each click clears the other two.
        var pick = function (on) {
            rSplit.value = (on === rSplit); rBW.value = (on === rBW); rL.value = (on === rL);
            sync();
        };
        rSplit.onClick = function () { pick(rSplit); };
        rBW.onClick = function () { pick(rBW); };
        rL.onClick = function () { pick(rL); };
        cA.onClick = sync;
        sync();
        var gB = d.add("group"); gB.alignment = ["right", "top"];
        gB.add("button", undefined, namesOnly ? "Skip layer" : "Skip file", { name: "cancel" });
        gB.add("button", undefined, "Add", { name: "ok" });
        if (d.show() !== 1) { return null; }
        var out = [];
        if (rSplit.value) {
            for (i = 0; i < 3; i++) { out.push({ name: trim(fields[i].text), channel: chs[i] }); }
            if (cA.value && hasA) { out.push({ name: trim(fields[3].text), channel: "A" }); }
        } else {
            out.push({ name: trim(eOne.text), channel: rL.value ? "L" : "R" });
        }
        return out;
    }

    function uniqueObjectName(name) {
        var taken = {}, i, n = name, k = 2;
        for (i = 0; i < state.crypto.objects.length; i++) { taken[state.crypto.objects[i].name] = 1; }
        while (taken[n]) { n = name + "_" + (k++); }
        return n;
    }

    function addMattes() {
        var files = pickFiles("Add mattes (EXR, PNG or TIFF; pick one frame of each sequence)", true);
        if (!files.length) { return; }
        var sel = [], added = 0, i, j;
        for (i = 0; i < files.length; i++) {
            var seq = sequenceOfFile(files[i]), info;
            try { info = readImageInfo(seq.firstFile); } catch (e) { alert(e.message); continue; }
            var layer = defaultLayer(info.channels);
            var chs = channelsOfLayer(info.channels, layer);
            var base = nameFromFile(decodeURI(files[i].name));
            var rows;
            if (hasRGB(chs)) {
                rows = askMatteSplit(decodeURI(files[i].name), base, !!upperSet(chs).A);
                if (!rows) { continue; }
            } else {
                rows = [{ name: base, channel: "R" }];   // BW / single channel / Y: the CLI reads it as R
            }
            for (j = 0; j < rows.length; j++) {
                if (rows[j].name === "") { continue; }
                sel.push(state.crypto.objects.length);
                state.crypto.objects.push({ name: uniqueObjectName(rows[j].name), src: seq.src, layer: layer, channel: rows[j].channel });
                added++;
            }
            if (!state.passes.length && added) { suggestOutput(seq.src); }
        }
        ui.cryptoFilter.text = "";
        changed(null, sel);
        status("Added " + added + " matte object(s). Top row = highest priority.");
    }

    function removeMattes() {
        var sel = selectedModel(ui.cryptoList, cryptoView);
        if (!sel.length) { status("Select mattes to remove."); return; }
        removeRows(state.crypto.objects, sel);
        changed(null, null);
    }

    function moveMattes(dir) {
        var sel = selectedModel(ui.cryptoList, cryptoView);
        if (!sel.length) { return; }
        changed(null, dir === 0 ? moveRowsToTop(state.crypto.objects, sel) : moveRows(state.crypto.objects, sel, dir));
    }

    function editMatte() {
        var sel = selectedModel(ui.cryptoList, cryptoView);
        if (sel.length !== 1) { status("Select one matte to edit."); return; }
        var o = state.crypto.objects[sel[0]];
        if (o.ref) { editLayerMatte(o, sel); return; }
        var inf = sourceInfo(o.src);
        var d = new Window("dialog", "Edit matte object");
        d.orientation = "column";
        d.alignChildren = ["fill", "top"];
        var gN = labelled(d, "Object");
        var eName = gN.add("edittext", undefined, o.name); eName.characters = 30;
        var gS = labelled(d, "Source");
        var eSrc = gS.add("edittext", undefined, o.src); eSrc.characters = 40;
        var bSrc = gS.add("button", undefined, "Browse...");
        var gL = labelled(d, "Layer in file");
        var dLayer = layerDropdown(gL, inf ? inf.channels : [], o.layer);
        var gC = labelled(d, "Channel");
        var dCh = gC.add("dropdownlist", undefined, CRYPTO_CHANNELS);
        dCh.selection = Math.max(0, inArray(CRYPTO_CHANNELS, o.channel));
        gC.add("statictext", undefined, "L = luminance");
        var gB = d.add("group"); gB.alignment = ["right", "top"];
        gB.add("button", undefined, "Cancel", { name: "cancel" });
        var ok = gB.add("button", undefined, "OK", { name: "ok" });
        bSrc.onClick = function () {
            var f = pickFiles("Pick one frame of the new matte", false);
            if (!f.length) { return; }
            var seq = sequenceOfFile(f[0]);
            eSrc.text = seq.src;
            try {
                var hi = readImageInfo(seq.firstFile);
                var parent = dLayer.parent;
                parent.remove(dLayer);
                dLayer = layerDropdown(parent, hi.channels, defaultLayer(hi.channels));
                d.layout.layout(true);
            } catch (e) { alert(e.message); }
        };
        ok.onClick = function () {
            var nm = trim(eName.text);
            if (nm === "") { alert("The object needs a name."); return; }
            for (var i = 0; i < state.crypto.objects.length; i++) {
                if (i !== sel[0] && state.crypto.objects[i].name === nm) { alert("Another object is already called " + nm + "."); return; }
            }
            o.name = nm;
            o.src = fwd(trim(eSrc.text));
            o.layer = dLayer.layers[dLayer.selection ? dLayer.selection.index : 0];
            o.channel = dCh.selection.text;
            d.close(1);
        };
        if (d.show() === 1) { infoCache = {}; changed(null, sel); }
    }

    // ---------------------------------------------------------------- compile

    function setProgress(v, text) {
        ui.progress.value = v;
        if (text) { status(text); }
        try { if (ui.win.update) { ui.win.update(); } } catch (e) { /* docked panels may not repaint mid-script */ }
    }

    function findFootage(file) {
        var want = fwd(file.fsName).toLowerCase(), i, it;
        for (i = 1; i <= app.project.numItems; i++) {
            it = app.project.item(i);
            if (it instanceof FootageItem && it.mainSource && it.mainSource.file &&
                fwd(it.mainSource.file.fsName).toLowerCase() === want) { return it; }
        }
        return null;
    }

    // Import (or reload) the result; in the active comp, add it with AE's Cryptomatte effect.
    function importResult(firstOut, isSeq, hasCrypto) {
        var file = new File(firstOut), note = [];
        app.beginUndoGroup(TOOL + ": import result");
        try {
            var item = findFootage(file);
            if (item) { item.mainSource.reload(); note.push("reloaded " + item.name); }
            else {
                var io = new ImportOptions(file);
                if (isSeq) { io.sequence = true; }
                item = app.project.importFile(io);
                note.push("imported " + item.name);
            }
            var comp = activeComp();
            if (comp) {
                var layer = null, i;
                for (i = 1; i <= comp.numLayers; i++) { if (comp.layer(i).source && comp.layer(i).source.id === item.id) { layer = comp.layer(i); break; } }
                if (!layer) { layer = comp.layers.add(item); note.push("added to " + comp.name); }
                if (hasCrypto) {
                    var fx = layer.property("ADBE Effect Parade"), found = false, names = ["Cryptomatte", "ADBE Cryptomatte"];
                    for (i = 1; i <= fx.numProperties; i++) {
                        if (fx.property(i).matchName === names[0] || fx.property(i).matchName === names[1]) { found = true; }
                    }
                    for (i = 0; i < names.length && !found; i++) {
                        if (fx.canAddProperty(names[i])) { fx.addProperty(names[i]); found = true; note.push("Cryptomatte effect added"); }
                    }
                    if (!found) { note.push("could not add the Cryptomatte effect"); }
                }
            }
        } catch (e) {
            note.push("import failed: " + e.message);
        } finally {
            app.endUndoGroup();
        }
        return note.join(", ");
    }

    // ---------------------------------------------------------------- comp layers (0.3): rows

    function setSource(src, quiet) {
        state.source = src;
        ui.rComp.value = src === "comp";
        ui.rFiles.value = src !== "comp";
        ui.addPass.text = src === "comp" ? "Add selected" : "Add files...";
        ui.addMatte.text = src === "comp" ? "Add selected" : "Add mattes...";
        ui.addPass.helpTip = ui.addMatte.helpTip = src === "comp" ?
            "Add the layers selected in the active comp. They are rendered to temporary EXRs when you press Compile." :
            "Pick one frame of each EXR or PNG sequence.";
        if (!quiet) {
            saveSetup();
            status(src === "comp" ? "Select layers in the comp, then press Add selected." : "Add files: pick one frame of each sequence.");
        }
    }

    function selectedCompLayers() {
        var comp = activeComp(), out = [], i, l;
        if (!comp) { status("Open the comp and select its layers first."); return null; }
        for (i = 0; i < comp.selectedLayers.length; i++) {
            l = comp.selectedLayers[i];
            if (l instanceof CameraLayer || l instanceof LightLayer) { continue; }
            out.push(l);
        }
        if (!out.length) { status("Select one or more layers in " + comp.name + " first."); return null; }
        return { comp: comp, layers: out };
    }

    function addLayerPasses() {
        var sel = selectedCompLayers();
        if (!sel) { return; }
        var hasMain = false, rows = [], i;
        for (i = 0; i < state.passes.length; i++) { if (state.passes[i].main) { hasMain = true; } }
        var skipped = 0;
        for (i = 0; i < sel.layers.length; i++) {
            if (hasLayerRow(state.passes, refOfLayer(sel.layers[i]))) { skipped++; continue; }   // already a pass
            var row = makeLayerPass(refOfLayer(sel.layers[i]), !hasMain);
            if (row.main) { hasMain = true; }
            rows.push(state.passes.length);
            state.passes.push(row);
        }
        suggestOutputForComp(sel.comp);
        ui.passFilter.text = "";
        changed(rows, null);
        status("Added " + (sel.layers.length - skipped) + " layer pass(es)" + (skipped ? " (" + skipped + " already in the list)" : "") +
               ". They render when you press Compile.");
    }

    // One question for the batch: alpha/luminance (automatic), or RGB split per layer.
    function askLayerMatteMode(count) {
        var d = new Window("dialog", "Mattes from " + count + " layer(s)");
        d.orientation = "column";
        d.alignChildren = ["fill", "top"];
        var rAuto = d.add("radiobutton", undefined, "One object per layer: its alpha (luminance for footage without alpha)");
        var rRGB = d.add("radiobutton", undefined, "RGB matte: split each layer into R, G and B objects");
        rAuto.value = true;
        var gB = d.add("group"); gB.alignment = ["right", "top"];
        gB.add("button", undefined, "Cancel", { name: "cancel" });
        gB.add("button", undefined, "Add", { name: "ok" });
        if (d.show() !== 1) { return null; }
        return rRGB.value ? "rgb" : "auto";
    }

    function addLayerMattes() {
        var sel = selectedCompLayers();
        if (!sel) { return; }
        var mode = askLayerMatteMode(sel.layers.length);
        if (!mode) { return; }
        var rowsIdx = [], i, j, rows, names;
        for (i = 0; i < sel.layers.length; i++) {
            var ref = refOfLayer(sel.layers[i]);
            names = null;
            if (mode === "rgb") {
                var picked = askMatteSplit(sel.layers[i].name, stripVersion(sel.layers[i].name) || "matte", false, true);
                if (!picked) { continue; }
                names = [];
                for (j = 0; j < 3; j++) { names.push(picked[j] ? picked[j].name : ""); }
            }
            rows = makeLayerMattes(ref, mode, layerHasAlpha(sel.layers[i]), names);
            for (j = 0; j < rows.length; j++) {
                if (hasLayerRow(state.crypto.objects, ref, rows[j].channel)) { continue; }   // same layer + channel already there
                rows[j].name = uniqueObjectName(rows[j].name);
                rowsIdx.push(state.crypto.objects.length);
                state.crypto.objects.push(rows[j]);
            }
        }
        if (!state.passes.length) { suggestOutputForComp(sel.comp); }
        ui.cryptoFilter.text = "";
        changed(null, rowsIdx);
        status("Added " + rowsIdx.length + " matte object(s) from layers. Top row = highest priority.");
    }

    function layerRowDialog(title, row) {
        var d = new Window("dialog", title);
        d.orientation = "column";
        d.alignChildren = ["fill", "top"];
        var gN = labelled(d, "Name");
        var eName = gN.add("edittext", undefined, row.name); eName.characters = 30;
        var gS = labelled(d, "Layer");
        var st = gS.add("statictext", undefined, row.ref.compName + " > " + row.ref.layerName);
        st.preferredSize = [320, 18];
        return { d: d, eName: eName };
    }

    function editLayerPass(p, sel) {
        var w = layerRowDialog("Edit pass (comp layer)", p), d = w.d;
        var gC = labelled(d, "Channels");
        var eMap = gC.add("edittext", undefined, p.map); eMap.characters = 20;
        gC.add("statictext", undefined, "RGBA, RGB, A, or R->Z");
        var gT = labelled(d, "Type");
        var dType = gT.add("dropdownlist", undefined, ["half", "float"]);
        dType.selection = p.type === "float" ? 1 : 0;
        var cMain = d.add("checkbox", undefined, "\u2605 Main layer (written as bare R G B A)");
        cMain.value = !!p.main;
        var gB = d.add("group"); gB.alignment = ["right", "top"];
        gB.add("button", undefined, "Cancel", { name: "cancel" });
        var ok = gB.add("button", undefined, "OK", { name: "ok" });
        ok.onClick = function () {
            var nm = layerSafe(trim(w.eName.text));
            for (var i = 0; i < state.passes.length; i++) {
                if (i !== sel[0] && state.passes[i].name === nm) { alert("Another pass is already called " + nm + "."); return; }
            }
            p.name = nm;
            p.map = trim(eMap.text) || "RGBA";
            p.type = dType.selection.text;
            if (cMain.value) { for (var j = 0; j < state.passes.length; j++) { state.passes[j].main = false; } }
            p.main = cMain.value;
            d.close(1);
        };
        if (d.show() === 1) { changed(sel, null); }
    }

    function editLayerMatte(o, sel) {
        var w = layerRowDialog("Edit matte object (comp layer)", o), d = w.d;
        var gC = labelled(d, "Channel");
        var dCh = gC.add("dropdownlist", undefined, CRYPTO_CHANNELS);
        dCh.selection = Math.max(0, inArray(CRYPTO_CHANNELS, o.channel));
        gC.add("statictext", undefined, "A = alpha, L = luminance");
        var gB = d.add("group"); gB.alignment = ["right", "top"];
        gB.add("button", undefined, "Cancel", { name: "cancel" });
        var ok = gB.add("button", undefined, "OK", { name: "ok" });
        ok.onClick = function () {
            var nm = trim(w.eName.text);
            if (nm === "") { alert("The object needs a name."); return; }
            for (var i = 0; i < state.crypto.objects.length; i++) {
                if (i !== sel[0] && state.crypto.objects[i].name === nm) { alert("Another object is already called " + nm + "."); return; }
            }
            o.name = nm;
            o.channel = dCh.selection.text;
            d.close(1);
        };
        if (d.show() === 1) { changed(null, sel); }
    }

    // ---------------------------------------------------------------- comp layers (0.3): renders

    function findLayer(ref) {
        var comp = null, layer = null, i;
        try { comp = app.project.itemByID(ref.compId); } catch (e) { comp = null; }
        if (!comp || !(comp instanceof CompItem)) { return null; }
        if (ref.layerId !== undefined && ref.layerId !== null) {
            try { layer = comp.layerByID(ref.layerId); } catch (e2) { layer = null; }
        }
        if (!layer && ref.layerIndex >= 1 && ref.layerIndex <= comp.numLayers && comp.layer(ref.layerIndex).name === ref.layerName) {
            layer = comp.layer(ref.layerIndex);
        }
        if (!layer) {
            for (i = 1; i <= comp.numLayers; i++) { if (comp.layer(i).name === ref.layerName) { layer = comp.layer(i); break; } }
        }
        return layer;
    }

    function refOfLayer(layer) {
        var comp = layer.containingComp, id = null;
        try { id = layer.id; } catch (e) { id = null; }
        return { compId: comp.id, compName: comp.name, layerId: (id === undefined ? null : id),
                 layerIndex: layer.index, layerName: layer.name };
    }

    // Footage files with no alpha give luminance mattes; everything else (shapes, text, solids
    // with masks, precomps, footage with alpha) gives its alpha.
    function layerHasAlpha(layer) {
        try {
            var src = layer.source;
            if (src && src instanceof FootageItem && src.mainSource instanceof FileSource) { return !!src.mainSource.hasAlpha; }
        } catch (e) { /* default below */ }
        return true;
    }

    function compFrames(comp) {
        var fd = comp.frameDuration;
        var start = Math.round((comp.displayStartTime + comp.workAreaStart) / fd);
        return { first: start, last: start + Math.round(comp.workAreaDuration / fd) - 1, count: Math.round(comp.workAreaDuration / fd) };
    }

    function trySettings(target, list, log) {
        for (var i = 0; i < list.length; i++) {
            try { target.setSettings(list[i]); log.push("ok   " + jsonStringify(list[i])); return true; }
            catch (e) { log.push("no   " + jsonStringify(list[i]) + "  (" + e.message + ")"); }
        }
        return false;
    }

    function settingsText(target) {
        try { return jsonStringify(target.getSettings(GetSettingsFormat.STRING), 2); } catch (e) { return "(getSettings: " + e.message + ")"; }
    }

    function specOf(target, log, label) {
        try { return target.getSettings(GetSettingsFormat.SPEC); }
        catch (e) { log.push("no   " + label + " getSettings(SPEC): " + e.message); return null; }
    }

    // Set one setting from AE's own list of allowed values (falls back to the given guesses).
    function setFromSpec(target, spec, path, re, guesses, log) {
        var tries = [], v = spec ? pickSettingValue(spec, path, re) : null, i;
        if (v !== null) { tries.push(nestSetting(path, v)); }
        else if (spec) { log.push("     " + path.join(" > ") + ": allowed = " + jsonStringify(specValues(specNode(spec, path)))); }
        for (i = 0; i < guesses.length; i++) { tries.push(guesses[i]); }
        return trySettings(target, tries, log);
    }

    // Duplicate the layer's comp (same size, pixel aspect, rate, work area, colour), switch off
    // every other layer except cameras, lights and the layer's track matte, render it to a
    // 32-bit OpenEXR sequence in tmpDir. The user's comps are never changed; the temp comps,
    // their render-queue items and the project folder holding them are removed afterwards.
    // patterns is filled as items are set up, so the caller can clean up after a failed render.
    var COLOUR_HELP = "The output template changes colour values (a solid of 0.5 / 0.25 / 0.125 came back as %M: %N). " +
        "The panel needs raw values. Edit > Templates > Output Module..., select \"jd_exrc EXR32\", Edit..., " +
        "Color Management tab: tick Preserve RGB (no output conversion). Then Compile again.";
    var TEMPLATE_HELP = "Rendering comp layers needs an output-module template made once by hand " +
        "(After Effects does not let scripts choose OpenEXR or float depth):\n\n" +
        "Edit > Templates > Output Module..., New, name it \"%T\", Format: OpenEXR " +
        "(Format Options: 32-bit float, a lossless compression such as ZIP), Channels: RGB + Alpha, " +
        "Depth: Floating Point+, Color: Premultiplied; Color Management tab: tick Preserve RGB. Click OK, then Compile again.";
    var renderNotes = [];                              // filled by renderLayerSources, shown after Compile


    function renderLayerSources(refs, tmpDir, patterns, exe) {
        renderNotes = [];
        var calib = null;
        var rq = app.project.renderQueue, log = [], made = [], items = [], parked = [], i, j;
        var folder = app.project.items.addFolder("jd_EXRCompiler temp (safe to delete)");
        try {
            for (i = 1; i <= rq.numItems; i++) {
                if (rq.item(i).status === RQItemStatus.QUEUED && rq.item(i).render) { rq.item(i).render = false; parked.push(rq.item(i)); }
            }
            for (i = 0; i < refs.length; i++) {
                var layer = findLayer(refs[i]);
                if (!layer) { throw new Error("Layer \"" + refs[i].layerName + "\" in comp \"" + refs[i].compName + "\" was not found."); }
                var comp = layer.containingComp, idx = layer.index;
                var matte = null;
                try { matte = layer.trackMatteLayer; } catch (eM) { matte = null; }
                var dup = comp.duplicate();
                made.push(dup);
                dup.name = "jd_exrc_tmp_" + (i + 1) + "_" + layer.name;
                dup.parentFolder = folder;
                for (j = 1; j <= dup.numLayers; j++) {
                    var l = dup.layer(j);
                    try { l.solo = false; } catch (eS) { /* not soloable */ }
                    if (j === idx) { l.enabled = true; continue; }
                    if (l instanceof CameraLayer || l instanceof LightLayer) { continue; }
                    if (matte && j === matte.index) { continue; }
                    if (j === idx - 1 && l.isTrackMatte) { continue; }   // legacy track matte above the layer
                    try { l.enabled = false; } catch (eE) { /* some layers cannot be switched */ }
                }
                var item = rq.items.add(dup);
                items.push(item);
                log.push("--- " + dup.name + " (" + comp.width + "x" + comp.height + ", " + compFrames(comp).count + " frames)");
                try { item.applyTemplate("Best Settings"); log.push("ok   render template Best Settings"); }
                catch (eT) { log.push("no   render template Best Settings (" + eT.message + ")"); }
                var rsSpec = specOf(item, log, "render settings");
                setFromSpec(item, rsSpec, ["Color Depth"], /32/, [{ "Color Depth": "32 bits per channel" }], log);
                var om = item.outputModule(1), usedTemplate = false;
                var tpl = trim(state.omTemplate || "") || "jd_exrc EXR32";
                for (j = 0; j < om.templates.length; j++) {
                    if (om.templates[j] === tpl) { om.applyTemplate(tpl); usedTemplate = true; log.push("ok   output template " + tpl); }
                }
                // AE 2026 lets a script set none of Format, Depth or Channels on an output module
                // (all "read-only"; probes v1-v4, 16 Nov), and no built-in template is OpenEXR or float.
                // So the temp renders need a template the user made once: OpenEXR (or TIFF) 32-bit float,
                // RGB + Alpha. The panel only applies it; it never creates or changes templates.
                if (!usedTemplate) { throw new Error(TEMPLATE_HELP.replace("%T", tpl)); }
                var fmtNow = "";
                try { fmtNow = String(om.getSettings(GetSettingsFormat.STRING).Format); } catch (eF) { fmtNow = ""; }
                var ext = /exr/i.test(fmtNow) ? "exr" : (/tif/i.test(fmtNow) ? "tif" : "");
                if (!ext) { throw new Error("The output template \"" + tpl + "\" writes " + (fmtNow || "an unknown format") + ": it must be OpenEXR (or TIFF), 32-bit float, RGB + Alpha."); }
                var base = "L" + (i + 1) + "_" + layerSafe(layer.name);
                om.file = new File(tmpDir.fsName + "/" + base + "_[#####]." + ext);
                log.push("render settings:\n" + settingsText(item) + "\noutput module:\n" + settingsText(om));
                patterns[refKey(refs[i])] = fwd(tmpDir.fsName) + "/" + base + "_#####." + ext;
            }
            // Calibration first, on its own: a 4x4 one-frame solid of CALIB_RGB with the same template.
            // If the template changes values, nothing else is rendered (0.3.4 finding: it rendered all
            // layers before checking). Checked on every compile: the first jd_exrc call of a compile costs
            // AE ~6 s and later ones ~0.5 s, so this adds little.
            if (items.length) {
                var cc = app.project.items.addComp("jd_exrc_tmp_calibration", 4, 4, 1, 1 / 25, 25);
                made.push(cc);
                cc.parentFolder = folder;
                cc.layers.addSolid(CALIB_RGB, "calibration", 4, 4, 1, 1 / 25);
                var citem = rq.items.add(cc);
                items.push(citem);
                try { citem.applyTemplate("Best Settings"); } catch (eCT) { /* default */ }
                var com = citem.outputModule(1);
                com.applyTemplate(trim(state.omTemplate || "") || "jd_exrc EXR32");
                var cext = String(patterns[refKey(refs[0])]).replace(/^.*\./, "");
                com.file = new File(tmpDir.fsName + "/calibration_[#####]." + cext);
                patterns.__calibration = fwd(tmpDir.fsName) + "/calibration_#####." + cext;
                calib = { pattern: patterns.__calibration };
            }
            debugLog(log.join("\n"));
            log = null;   // written
            if (calib) {
                for (i = 0; i < refs.length; i++) { items[i].render = false; }   // pass 1: calibration only
                setProgress(3, "Checking the output template (calibration frame)...");
                rq.render();
                var cseq = sequenceOfPattern(calib.pattern);
                if (!cseq) { throw new Error("The calibration frame was not rendered (see last_compile.log)."); }
                var cres = runCli(exe, ["pixel", cseq.firstFile.fsName, "1", "1"]);
                var cc2 = calibrationCheck(cres.other);
                debugLog("calibration " + decodeURI(cseq.firstFile.name) + ": expected " + CALIB_RGB.join(" ") + ", measured " +
                         cc2.measured.join(" ") + (cc2.ok ? "  OK" : "  (" + cc2.note + ")"));
                if (!cc2.ok) { throw new Error(COLOUR_HELP.replace("%M", cc2.measured.join(" / ")).replace("%N", cc2.note)); }
                for (i = 0; i < refs.length; i++) { items[i].render = true; }    // pass 2: the layers
            }
            setProgress(5, "Rendering " + refs.length + " layer(s) in the render queue...");
            rq.render();
            for (i = 0; i < refs.length; i++) {
                if (items[i].status !== RQItemStatus.DONE) { throw new Error("The temp render of \"" + refs[i].layerName + "\" did not finish."); }
            }
            for (i = 0; i < refs.length; i++) {
                var seqR = sequenceOfPattern(patterns[refKey(refs[i])]);
                if (!seqR) { throw new Error("No frames were rendered for \"" + refs[i].layerName + "\"."); }
                if (/\.tif$/i.test(seqR.firstFile.name)) {   // check the depth AE really wrote
                    var th = null;
                    try { th = tiffHeaderInfo(readBytes(seqR.firstFile, 65536)); } catch (eH) { th = null; }
                    debugLog("temp TIFF " + decodeURI(seqR.firstFile.name) + ": " + jsonStringify(th));
                    if (th && !th.isFloat32) {
                        renderNotes.push("\"" + refs[i].layerName + "\" rendered as " + th.bits + "-bit " + (th.format === 3 ? "float" : "integer") +
                                         " TIFF, not 32-bit float");
                    }
                }
            }
            return patterns;
        } finally {
            if (log) { debugLog(log.join("\n")); }   // the settings tried, also when setting up failed
            for (i = items.length - 1; i >= 0; i--) { try { items[i].remove(); } catch (e1) { /* gone */ } }
            for (i = made.length - 1; i >= 0; i--) { try { made[i].remove(); } catch (e2) { /* gone */ } }
            try { folder.remove(); } catch (e3) { /* gone */ }
            for (i = 0; i < parked.length; i++) { try { parked[i].render = true; } catch (e4) { /* changed meanwhile */ } }
        }
    }

    // Removes only the frames the panel rendered (names matching its own patterns, directly in
    // its own temp folder), then the folder if nothing else is left in it. Never recursive.
    function deleteTempRenders(tmpDir, patterns) {
        var res = [], kids = tmpDir.getFiles(), n = 0, kept = 0, i, j, k, name, a, b;
        for (k in patterns) {
            if (!patterns.hasOwnProperty(k)) { continue; }
            name = fileNameOf(patterns[k]);
            a = name.indexOf("#"); b = name.lastIndexOf("#");
            res.push(new RegExp("^" + regexEscape(name.substr(0, a)) + "\\d{" + (b - a + 1) + "}" + regexEscape(name.substr(b + 1)) + "$"));
        }
        for (i = 0; i < kids.length; i++) {
            var ours = false;
            if (kids[i] instanceof File) {
                for (j = 0; j < res.length; j++) { if (res[j].test(decodeURI(kids[i].name))) { ours = true; } }
            }
            if (ours && kids[i].remove()) { n++; } else { kept++; }
        }
        if (!kept) { tmpDir.remove(); }
        return { removed: n, kept: kept };
    }

    function compile() {
        state.output = fwd(trim(ui.output.text));
        var v = validateState(state);
        if (v.errors.length) {
            status(v.errors[0]);
            alert("Cannot compile yet:\n\n- " + v.errors.join("\n- "));
            return;
        }
        if (!scriptsMayWriteFiles()) { status("Scripting pref is off: see the message."); alert(PREF_HELP); return; }
        var exe = findExe();
        if (!exe) {
            status("jd_exrc not found next to the panel.");
            alert("jd_exrc" + (IS_WIN ? ".exe" : "") + " was not found.\n\nPut it next to jd_EXRCompiler.jsx, " +
                  "or in a folder called jd_EXRCompiler next to it.");
            return;
        }
        saveSetup();
        var outFolder = new Folder(folderOf(state.output));
        if (!outFolder.exists && !outFolder.create()) { status("Cannot create " + outFolder.fsName); return; }
        var stamp = new Date().getTime();
        var jobFile = new File(tempFolder().fsName + "/job_" + stamp + ".json");
        var refs = layerRefsOf(state), tmpDir = null, rendered = {}, job;
        renderNotes = [];

        ui.compile.enabled = false;
        try {
            debugLog(TOOL + " " + VERSION + "  " + new Date().toString() + "\nexe: " + exe.fsName + "\n", true);
            var resolved = state;
            if (refs.length) {
                tmpDir = new Folder(outFolder.fsName + "/_jd_exrc_tmp_" + stamp);
                if (!tmpDir.create()) { status("Cannot create " + tmpDir.fsName); return; }
                // No undo group around a render; the temp comps and queue items are removed in a finally.
                renderLayerSources(refs, tmpDir, rendered, exe);
                resolved = resolveLayerSources(state, rendered);
            }
            job = buildJob(resolved);
            writeText(jobFile, jsonStringify(job, 2));
            debugLog("job: " + jobFile.fsName + "\n" + jsonStringify(job, 2) + "\n");

            // Frames are worked out here, so there is no separate dry-run call.
            var frames = framesOfState(resolved), i;
            if (frames && !frames.length) { status("The input sequences share no frame number."); return; }
            var chunks = frames ? planChunks(frames, MAX_CHUNKS, MIN_CHUNK) : [null];
            var firstOut = frames ? expandFrame(fwd(state.output), frames[0]) : fwd(state.output);
            var doneFrames = 0, dropped = 0, checks = { passed: 0, failed: 0, firstFail: "" };
            var withCheck = state.importCheck && job.cryptomattes.length > 0;   // jd_exrc checks Cryptomattes
            for (i = 0; i < chunks.length; i++) {
                setProgress(10 + Math.round(90 * i / chunks.length),
                    "Compiling " + (chunks[i] ? "frames " + chunks[i] : "the file") + "  (" + (i + 1) + "/" + chunks.length +
                    ")... After Effects is busy until it finishes.");
                var args = ["run", jobFile.fsName, "--json-log"];
                if (chunks[i]) { args.push("--frames", chunks[i]); }
                if (withCheck) { args.push("--check"); }   // write + check in one call (each call costs AE ~5 s)
                var res = runCli(exe, args);
                if (res.exit === null) { status("jd_exrc did not run: " + (res.other.join(" ") || "no output") + "  (see last_compile.log)"); return; }
                var fr = eventsOf(res, "frame");
                doneFrames += fr.length;
                for (var k = 0; k < fr.length; k++) { if (/^dropped/.test(fr[k].msg || "")) { dropped++; } }
                var cs = checkSummary(res.events);
                checks.passed += cs.passed; checks.failed += cs.failed;
                if (!checks.firstFail) { checks.firstFail = cs.firstFail; }
                if (res.exit === 1 && withCheck) { continue; }   // frames written, a check failed: report it, keep going
                if (res.exit !== 0) {
                    status((res.exit === 2 ? "Job refused" : "Failed") + " (" + exitMeaning(res.exit) + "): " + firstError(res) +
                           "  [" + doneFrames + " frame(s) written]");
                    setProgress(Math.round(100 * i / chunks.length));
                    return;
                }
            }
            setProgress(100);
            if (!doneFrames) {   // no frame events read: count what is on disk
                var written = sequenceOfPattern(fwd(state.output));
                if (written) { doneFrames = written.count; if (!frames) { firstOut = fwd(written.firstFile.fsName); } }
            }
            var msg = "Done: " + doneFrames + " frame(s) -> " + fileNameOf(state.output) +
                      (dropped ? "  (" + dropped + " frame(s) had more objects than ranks)" : "");
            if (!new File(firstOut).exists) {
                status(msg + ".  No output frame found on disk - see last_compile.log in the temp folder.");
                return;
            }
            if (state.importCheck) {
                var chkText = !withCheck ? "no Cryptomatte to check" :
                              checks.failed ? ("check FAILED on " + checks.failed + " frame(s): " + checks.firstFail) :
                              ("check passed (" + checks.passed + " frame(s))");
                msg += ".  " + chkText + ";  " + importResult(firstOut, !!(frames && frames.length > 1), job.cryptomattes.length > 0);
            }
            if (v.warnings.length) { msg += "   Note: " + v.warnings.join(" "); }
            if (renderNotes.length) { msg += "   Temp renders: " + renderNotes.join("; ") + "."; }
            status(msg);
        } catch (err) {
            status("Error: " + err.message);
            if (/needs an output-module template|must be OpenEXR|changes colour values/.test(err.message)) { alert(err.message); }
            debugLog("ERROR: " + err.message + (err.line ? " (line " + err.line + ")" : ""));
        } finally {
            ui.compile.enabled = true;
            try { jobFile.remove(); } catch (e2) { /* temp */ }
            if (tmpDir && tmpDir.exists && rendered.__calibration) {   // the calibration frame is the panel's own scratch
                try { deleteTempRenders(tmpDir, { c: rendered.__calibration }); } catch (eC) { /* kept */ }
                delete rendered.__calibration;
            }
            if (tmpDir && tmpDir.exists && !tmpDir.getFiles().length) {
                tmpDir.remove();   // nothing was rendered: no question, no empty folder left behind
            } else if (tmpDir && tmpDir.exists) {
                if (confirm("Delete the temporary layer renders?\n\n" + tmpDir.fsName +
                            "\n\n(They are only needed to compile again without rendering.)")) {
                    try {
                        var del = deleteTempRenders(tmpDir, rendered);
                        if (del.kept) { status(ui.status.text + "  (temp folder kept: it holds " + del.kept + " file(s) the panel did not write)"); }
                    } catch (e3) { status(ui.status.text + "  (could not delete the temp renders)"); }
                }
            }
        }
    }

    // ---------------------------------------------------------------- layout

    function buttonRow(parent, labels) {
        var g = parent.add("group");
        g.orientation = "row";
        g.alignment = ["fill", "bottom"];
        g.spacing = 4;
        var out = [];
        for (var i = 0; i < labels.length; i++) { out.push(g.add("button", undefined, labels[i])); }
        return out;
    }

    function filterRow(parent) {
        var g = parent.add("group");
        g.alignment = ["fill", "top"];
        g.add("statictext", undefined, "Filter");
        var e = g.add("edittext", undefined, "");
        e.alignment = ["fill", "center"];
        e.characters = 20;
        return e;
    }

    function addList(parent, titles, widths) {
        var l = parent.add("listbox", undefined, [], {
            multiselect: true, numberOfColumns: titles.length, showHeaders: true,
            columnTitles: titles, columnWidths: widths
        });
        l.alignment = ["fill", "fill"];
        l.preferredSize = [420, 9 * 21 + 24];   // about 9 rows, then scroll
        l.minimumSize = [260, 120];
        return l;
    }

    function buildUI(thisObj) {
        var win = (thisObj instanceof Panel) ? thisObj :
                  new Window("palette", TOOL + " " + VERSION, undefined, { resizeable: true });
        ui.win = win;
        win.orientation = "column";
        win.alignChildren = ["fill", "top"];
        win.spacing = 6;
        win.margins = 8;

        // top
        var top = win.add("group");
        top.alignChildren = ["left", "center"];
        var title = top.add("statictext", undefined, TOOL + "  " + VERSION);
        try { title.graphics.font = ScriptUI.newFont(title.graphics.font.name, "BOLD", title.graphics.font.size); } catch (e) { /* default font */ }
        ui.rComp = top.add("radiobutton", undefined, "From comp layers");
        ui.rFiles = top.add("radiobutton", undefined, "From files");
        ui.rFiles.value = true;
        ui.rComp.onClick = function () { setSource("comp"); };
        ui.rFiles.onClick = function () { setSource("files"); };
        var infoRow = win.add("group");
        infoRow.alignment = ["fill", "top"];
        ui.info = infoRow.add("statictext", undefined, "", { truncate: "end" });
        ui.info.alignment = ["fill", "center"];
        ui.info.characters = 40;
        var useComp = infoRow.add("button", undefined, "Use active comp");
        useComp.helpTip = "Save this setup, then load the setup of the comp that is active now.";
        useComp.onClick = function () { loadSetupForActiveComp(true); };

        // tabs
        var tabs = win.add("tabbedpanel");
        tabs.alignment = ["fill", "fill"];
        tabs.alignChildren = ["fill", "fill"];

        var tp = tabs.add("tab", undefined, "Passes (0)");
        ui.tabPasses = tp;
        tp.orientation = "column";
        tp.alignChildren = ["fill", "top"];
        ui.passFilter = filterRow(tp);
        ui.passList = addList(tp, ["\u2605", "Name  \u2190  source", "Channels", "Type"], [24, 280, 80, 50]);
        var pb = buttonRow(tp, ["Add files...", "Remove", "Up", "Down", "Edit...", "Bump version"]);
        ui.addPass = pb[0];
        pb[0].onClick = function () { if (state.source === "comp") { addLayerPasses(); } else { addPasses(); } };
        pb[1].onClick = removePasses;
        pb[2].onClick = function () { movePasses(-1); };
        pb[3].onClick = function () { movePasses(1); };
        pb[4].onClick = editPass;
        pb[5].onClick = bumpPasses;
        pb[5].helpTip = "Repoint the selected rows to the next version (_v04 -> _v05) when that version exists.";
        ui.passList.onDoubleClick = editPass;
        ui.passFilter.onChanging = function () { fillPassList(); };

        var tc = tabs.add("tab", undefined, "Cryptomatte (0)");
        ui.tabCrypto = tc;
        tc.orientation = "column";
        tc.alignChildren = ["fill", "top"];
        ui.cryptoFilter = filterRow(tc);
        ui.cryptoList = addList(tc, ["#", "ID", "Object  \u2190  source", "Channel"], [30, 90, 250, 60]);
        var cb = buttonRow(tc, ["Add mattes...", "Remove", "Up", "Down", "Move to top", "Edit..."]);
        ui.addMatte = cb[0];
        cb[0].onClick = function () { if (state.source === "comp") { addLayerMattes(); } else { addMattes(); } };
        cb[1].onClick = removeMattes;
        cb[2].onClick = function () { moveMattes(-1); };
        cb[3].onClick = function () { moveMattes(1); };
        cb[4].onClick = function () { moveMattes(0); };
        cb[5].onClick = editMatte;
        ui.cryptoList.onDoubleClick = editMatte;
        ui.cryptoFilter.onChanging = function () { fillCryptoList(); };

        var opt = tc.add("group");
        opt.alignment = ["fill", "bottom"];
        opt.add("statictext", undefined, "Overlap");
        ui.mode = opt.add("dropdownlist", undefined, MODE_LABELS);
        ui.mode.helpTip = "Priority: higher rows win where mattes overlap, soft edges kept.\n" +
                          "Holdout: classic multiply holdout.\nNormalise: overlaps are shared, no priority.";
        opt.add("statictext", undefined, "Ranks");
        var rankLabels = [];
        for (var r = 0; r < RANKS.length; r++) { rankLabels.push(String(RANKS[r])); }
        ui.ranks = opt.add("dropdownlist", undefined, rankLabels);
        ui.bg = opt.add("checkbox", undefined, "Background");
        ui.bgName = opt.add("edittext", undefined, "background");
        ui.bgName.characters = 12;
        var opt2 = tc.add("group");
        opt2.alignment = ["fill", "bottom"];
        opt2.add("statictext", undefined, "Cryptomatte name");
        ui.cryptoName = opt2.add("edittext", undefined, "CryptoMatte");
        ui.cryptoName.characters = 16;
        ui.mode.onChange = function () { state.crypto.mode = MODES[ui.mode.selection.index]; saveSetup(); };
        ui.ranks.onChange = function () { state.crypto.ranks = RANKS[ui.ranks.selection.index]; saveSetup(); };
        ui.bg.onClick = function () { state.crypto.background = ui.bg.value; ui.bgName.enabled = ui.bg.value; saveSetup(); };
        ui.bgName.onChange = function () { state.crypto.backgroundName = trim(ui.bgName.text) || "background"; saveSetup(); };
        ui.cryptoName.onChange = function () { state.crypto.name = trim(ui.cryptoName.text) || "CryptoMatte"; saveSetup(); };

        // bottom
        var outRow = win.add("group");
        outRow.alignment = ["fill", "bottom"];
        outRow.add("statictext", undefined, "Output");
        ui.output = outRow.add("edittext", undefined, "");
        ui.output.alignment = ["fill", "center"];
        ui.output.characters = 30;
        ui.output.helpTip = "Use #### for the frame number, e.g. D:/shot/comp/sh010_compiled.####.exr";
        var browse = outRow.add("button", undefined, "Browse...");
        ui.output.onChange = function () { state.output = fwd(trim(ui.output.text)); saveSetup(); };
        browse.onClick = function () {
            var start = new File(state.output ? folderOf(state.output) + "/x.exr" : lastFolder().fsName + "/compiled.exr");
            var f = start.saveDlg("Output EXR (#### is added for sequences)", IS_WIN ? "OpenEXR:*.exr" : undefined);
            if (!f) { return; }
            var p = fwd(f.fsName);
            if (!/\.exr$/i.test(p)) { p += ".exr"; }
            if (!/#/.test(p) && anySequence(state)) {
                p = p.replace(/\.exr$/i, ".####.exr");
            }
            state.output = p;
            ui.output.text = p;
            saveSetup();
        };

        var optRow = win.add("group");
        optRow.alignment = ["fill", "bottom"];
        optRow.add("statictext", undefined, "Compression");
        ui.compression = optRow.add("dropdownlist", undefined, COMPRESSIONS);
        ui.multipart = optRow.add("checkbox", undefined, "Multi-part");
        ui.importCheck = optRow.add("checkbox", undefined, "Import and check");
        ui.importCheck.helpTip = "After compiling: run jd_exrc check, import the result and, in the active comp, " +
                                 "add it with the Cryptomatte effect.";
        ui.compression.onChange = function () { state.compression = COMPRESSIONS[ui.compression.selection.index]; saveSetup(); };
        ui.multipart.onClick = function () { state.multipart = ui.multipart.value; saveSetup(); };
        ui.importCheck.onClick = function () { state.importCheck = ui.importCheck.value; saveSetup(); };

        ui.progress = win.add("progressbar", undefined, 0, 100);
        ui.progress.alignment = ["fill", "bottom"];
        ui.progress.preferredSize.height = 8;
        ui.compile = win.add("button", undefined, "Compile");
        ui.compile.alignment = ["fill", "bottom"];
        ui.compile.onClick = compile;
        ui.status = win.add("statictext", undefined, "", { truncate: "end" });
        ui.status.alignment = ["fill", "bottom"];
        ui.status.characters = 50;

        win.onResizing = win.onResize = function () { this.layout.resize(); };
        return win;
    }

    // Probe hook (tests/ae_launcher_probe.jsx): with $.global.JD_EXRC_PROBE set, $.evalFile of this
    // file returns the launcher pieces instead of building the panel.
    if ($.global.JD_EXRC_PROBE) {
        return { core: core, runCli: runCli, findExe: findExe, tempFolder: tempFolder };
    }

    var w = buildUI(thisObj);
    loadSetupForActiveComp(false);
    if (!scriptsMayWriteFiles()) { status("Turn on \"Allow Scripts to Write Files and Access Network\" (Preferences > Scripting & Expressions)."); }
    if (w instanceof Window) { w.center(); w.show(); } else { w.layout.layout(true); w.layout.resize(); }

})(this);
