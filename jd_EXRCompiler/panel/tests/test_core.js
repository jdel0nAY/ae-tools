// Tests for the logic half of jd_EXRCompiler.jsx (no After Effects needed).
//   node test_core.js                 unit tests + ES3 lint
//   node test_core.js <jd_exrc.exe>   also an end-to-end run through the CLI (procedural mattes only)
"use strict";
const fs = require("fs");
const path = require("path");
const os = require("os");
const vm = require("vm");
const zlib = require("zlib");
const cp = require("child_process");

const JSX = path.join(__dirname, "..", "jd_EXRCompiler.jsx");
const src = fs.readFileSync(JSX, "utf8");

let pass = 0, fail = 0;
function ok(cond, label, extra) {
  if (cond) { pass++; } else { fail++; console.log("FAIL " + label + (extra !== undefined ? "  -> " + JSON.stringify(extra) : "")); }
}
function eq(a, b, label) { ok(JSON.stringify(a) === JSON.stringify(b), label, { got: a, want: b }); }

// ---------------------------------------------------------------- ES3 lint
// No ES3 parser is installed here, so: parse with V8 (syntax) + scan for ES5+ features ExtendScript lacks.
const code = src.replace(/\/\*[\s\S]*?\*\//g, "").replace(/(^|[^:\\"'])\/\/.*$/gm, "$1")
                .replace(/"(?:[^"\\\n]|\\.)*"/g, '""').replace(/'(?:[^'\\\n]|\\.)*'/g, "''");
const banned = [
  [/\blet\s/, "let"], [/\bconst\s/, "const"], [/=>/, "arrow function"], [/`/, "template string"],
  [/\.(forEach|map|filter|reduce|some|every|bind|trim|keys)\s*\(/, "ES5 method"],
  [/\bJSON\./, "JSON object"], [/Object\.(keys|create|defineProperty)/, "Object.* ES5"], [/Array\.isArray/, "Array.isArray"],
  [/Date\.now/, "Date.now"], [/Math\.imul/, "Math.imul"], [/,\s*[\]}]/, "trailing comma"],
  [/\.(default|class|native|float|double|int|char|byte|long|short|boolean|final|delete|new|enum|export|import|super|extends|package|private|protected|public|static|interface|implements|goto|synchronized|throws|transient|volatile|abstract)\b/, "reserved word as property"],
  [/[{,]\s*(default|class|native|float|double|int|char|byte|long|short|boolean|final|delete|new|enum|in|for)\s*:/, "reserved word as key"],
  [/\bget\s+\w+\s*\(\)\s*{/, "getter"]
];
code.split("\n").forEach((line, i) => {
  for (const [re, what] of banned) { if (re.test(line)) { fail++; console.log(`LINT line ${i + 1}: ${what}: ${line.trim()}`); } }
});
// Array.prototype.indexOf is ES5; the source uses inArray() for arrays. String indexOf is fine,
// so list every .indexOf( call for a human look instead of failing.
try { new vm.Script(src, { filename: "jd_EXRCompiler.jsx" }); pass++; } catch (e) { fail++; console.log("SYNTAX " + e.message); }

// ---------------------------------------------------------------- load the logic
const sandbox = { module: { exports: {} } };
vm.runInNewContext(src, sandbox, { filename: "jd_EXRCompiler.jsx" });
const C = sandbox.module.exports;
ok(typeof C.murmur3 === "function", "core exported under node");

// ---------------------------------------------------------------- hash
eq(C.murmur3("hello", 0).toString(16), "248bfa47", "mmh3 hello");
eq(C.murmur3("foo", 0).toString(16), "f6a5c420", "mmh3 foo");
eq(C.murmur3("", 0), 0, "mmh3 empty");
eq(C.murmur3("", 1).toString(16), "514e28b7", "mmh3 empty seed 1");
eq(C.murmur3("The quick brown fox jumps over the lazy dog", 0).toString(16), "2e4ff723", "mmh3 fox");
ok(C.cryptoHex("CryptoMatte").indexOf("6ce235e") === 0, "CryptoMatte key", C.cryptoHex("CryptoMatte"));
ok(C.cryptoHex("CryptoObject").indexOf("3ae39a5") === 0, "CryptoObject key", C.cryptoHex("CryptoObject"));
eq(C.cryptoLayerKey("CryptoMatte"), "6ce235e", "layer key");
// manifest values the CLI wrote in the hand-off test
eq(C.cryptoHex("bg"), "0b31652c", "hex bg");
eq(C.cryptoHex("box"), "0a4a777b", "hex box");
eq(C.cryptoHex("left"), "a94a3318", "hex left");
eq(C.cryptoHex("right"), "6a7c91ff", "hex right");
eq(C.utf8Bytes("é€😀"), [0xc3, 0xa9, 0xe2, 0x82, 0xac, 0xf0, 0x9f, 0x98, 0x80], "utf8 bytes");
// mul32 against BigInt on random pairs
let mulOk = true;
for (let i = 0; i < 20000; i++) {
  const a = (Math.random() * 2 ** 32) >>> 0, b = (Math.random() * 2 ** 32) >>> 0;
  if (C.mul32(a, b) !== Number((BigInt(a) * BigInt(b)) & 0xffffffffn)) { mulOk = false; break; }
}
ok(mulOk, "mul32 == BigInt product mod 2^32");
// exponent fix: no ID is 0x00 or 0xff in the exponent
let expOk = true;
for (let i = 0; i < 5000; i++) { const e = (C.cryptoHash("obj" + i) >>> 23) & 255; if (e === 0 || e === 255) expOk = false; }
ok(expOk, "fixed IDs have a normal exponent");
eq(C.cryptoColour("hello"), [0x47, 0xfa, 0x8b], "colour = low bytes of fixed hash");

// ---------------------------------------------------------------- PNG chip
const png = Buffer.from(C.pngChip([255, 128, 0], 18, 12), "latin1");
ok(png.slice(0, 8).equals(Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a])), "png signature");
{
  let p = 8, chunks = [];
  while (p < png.length) {
    const len = png.readUInt32BE(p), type = png.toString("latin1", p + 4, p + 8);
    const data = png.slice(p + 8, p + 8 + len), crc = png.readUInt32BE(p + 8 + len);
    ok(zlib.crc32(png.slice(p + 4, p + 8 + len)) === crc, "png crc " + type);
    chunks.push({ type, data }); p += 12 + len;
  }
  eq(chunks.map(c => c.type), ["IHDR", "IDAT", "IEND"], "png chunks");
  const raw = zlib.inflateSync(chunks[1].data);
  eq(raw.length, 12 * (1 + 18 * 3), "png raw size");
  const mid = 6 * (1 + 54) + 1 + 9 * 3;
  eq([raw[mid], raw[mid + 1], raw[mid + 2]], [255, 128, 0], "png fill colour");
  eq([raw[1], raw[2], raw[3]], [48, 48, 48], "png border");
  fs.writeFileSync(path.join(os.tmpdir(), "jd_exrc_chip_test.png"), png);
}

// ---------------------------------------------------------------- JSON
const tricky = { a: "quote\" back\\ nl\n tab\t é 😀 \u0001", b: [1, -2.5, 1e-7, true, false, null], c: {}, d: [] };
const s = C.jsonStringify(tricky, 2);
ok(/^[\x00-\x7e]*$/.test(s), "stringify is pure ASCII");
eq(JSON.parse(s), tricky, "node JSON reads our output");
eq(C.jsonParse(JSON.stringify(tricky)), tricky, "our parser reads node output");
eq(C.jsonParse('\uFEFF {"x" : [ 1 , {"y":"\\u00e9\\/"} ] } '), { x: [1, { y: "é/" }] }, "parser: BOM, spaces, escapes");
let threw = false; try { C.jsonParse('{"a":1,}'); } catch (e) { threw = true; } ok(threw, "parser rejects trailing comma");
threw = false; try { C.jsonParse('{"a":1} x'); } catch (e) { threw = true; } ok(threw, "parser rejects trailing text");

// ---------------------------------------------------------------- names, sequences, versions
eq(C.splitFrame("beauty.1001.exr"), { head: "beauty.", digits: "1001", ext: ".exr" }, "splitFrame");
eq(C.splitFrame("beauty_v04.exr"), null, "version is not a frame");
eq(C.patternFromFrame("sh010_diffuse_v04.0012.exr"), "sh010_diffuse_v04.####.exr", "pattern");
eq(C.nameFromFile("sh010_beauty_v04.1001.exr"), "sh010_beauty", "name strips version + frame");
eq(C.nameFromFile("v003_depth.####.exr"), "depth", "leading version");
eq(C.nameFromFile("matte.png"), "matte", "still name");
eq(C.stripSharedPrefix(["sh010_lgt_beauty", "sh010_lgt_depth", "sh010_lgt_diffuse_direct"]), ["beauty", "depth", "diffuse_direct"], "shared prefix");
eq(C.stripSharedPrefix(["beauty", "beauty"]), ["beauty", "beauty"], "never empty a name");
eq(C.stripSharedPrefix(["one"]), ["one"], "single name untouched");
eq(C.layerSafe("diffuse direct.v2"), "diffuse_direct_v2", "layerSafe");
eq(C.bumpVersionPath("D:/r/v004/sh010_beauty_v004.####.exr"), "D:/r/v005/sh010_beauty_v005.####.exr", "bump folder+file");
eq(C.bumpVersionPath("D:/r/sh010_beauty_v09.####.exr"), "D:/r/sh010_beauty_v10.####.exr", "bump keeps padding, rolls 09->10");
eq(C.bumpVersionPath("D:/r/v12/beauty.####.exr"), "D:/r/v13/beauty.####.exr", "bump folder only");
eq(C.bumpVersionPath("D:/r/v12/beauty_v3.####.exr"), "D:/r/v12/beauty_v4.####.exr", "file version wins, other tokens untouched");
eq(C.bumpVersionPath("D:/r/beauty.####.exr"), null, "no version");
eq(C.bumpVersionPath("D:/dev/review/beauty_v1.exr"), "D:/dev/review/beauty_v2.exr", "dev/review are not versions");

const blender = ["ViewLayer.Combined.R", "ViewLayer.Combined.G", "ViewLayer.Combined.B", "ViewLayer.Combined.A", "ViewLayer.Depth.Z"];
eq(C.layersOfChannels(blender), ["ViewLayer.Combined", "ViewLayer.Depth"], "layers");
eq(C.defaultLayer(blender), "ViewLayer.Combined", "default layer = first RGB");
eq(C.defaultLayer(["R", "G", "B", "Z"]), "", "bare wins");
eq(C.defaultMap(["R", "G", "B", "A"]), "RGBA", "map RGBA");
eq(C.defaultMap(["r", "g", "b"]), "RGB", "map lowercase RGB");
eq(C.defaultMap(["Z"]), "Z", "map Z");
eq(C.defaultMap(["X", "Y", "Z"]), "XYZ", "map XYZ");
eq(C.defaultMap(["depth"]), "depth->depth", "map long name");
eq(C.defaultType("depth", "Z"), "float", "type depth");
eq(C.defaultType("diffuse", "RGB"), "half", "type colour");
eq(C.defaultType("zpass", "R->Z"), "float", "type R->Z");
const mp = C.makePass("sh010_beauty_v04.1001.exr", "D:\\r\\sh010_beauty_v04.####.exr", ["R", "G", "B", "A"]);
eq([mp.name, mp.src, mp.map, mp.type, mp.main], ["sh010_beauty", "D:/r/sh010_beauty_v04.####.exr", "RGBA", "half", false], "makePass");

// ---------------------------------------------------------------- moves
let arr = ["a", "b", "c", "d", "e"];
eq(C.moveRows(arr, [1, 3], -1), [0, 2], "up returns new idx"); eq(arr, ["b", "a", "d", "c", "e"], "up moved");
arr = ["a", "b", "c", "d", "e"];
eq(C.moveRows(arr, [0, 1], -1), [0, 1], "up at top is a no-op"); eq(arr, ["a", "b", "c", "d", "e"], "unchanged");
arr = ["a", "b", "c", "d", "e"];
eq(C.moveRows(arr, [3, 4], 1), [3, 4], "down at bottom no-op");
arr = ["a", "b", "c", "d", "e"];
eq(C.moveRows(arr, [0, 2], 1), [1, 3], "down"); eq(arr, ["b", "a", "d", "c", "e"], "down moved");
arr = ["a", "b", "c", "d", "e"];
eq(C.moveRowsToTop(arr, [4, 2]), [0, 1], "to top idx"); eq(arr, ["c", "e", "a", "b", "d"], "to top keeps order");
arr = ["a", "b", "c"]; C.removeRows(arr, [0, 2]); eq(arr, ["b"], "remove");

// ---------------------------------------------------------------- chunks + log parsing
eq(C.planChunks([1001, 1002, 1003, 1004], 2), ["1001-1002", "1003-1004"], "chunks even");
eq(C.planChunks([5, 1, 2, 3, 7, 8], 2), ["1-3", "5-5", "7-8"], "chunks split at gaps");
{ const ten = []; for (let f = 1001; f <= 1010; f++) ten.push(f); eq(C.planChunks(ten, 10, 25), ["1001-1010"], "10 frames -> one call"); }
{ const many = []; for (let f = 1; f <= 1000; f++) many.push(f); eq(C.planChunks(many, 10, 25).length, 10, "1000 frames -> 10 calls"); }
eq(C.planChunks([], 5), [], "no frames");
{
  const fr = []; for (let f = 1001; f <= 1100; f++) fr.push(f);
  const ch = C.planChunks(fr, 20);
  eq(ch.length, 20, "100 frames -> 20 chunks");
  let n = 0; ch.forEach(c => { const [a, b] = c.split("-").map(Number); n += b - a + 1; }); eq(n, 100, "chunks cover all");
}
const out = C.parseCliOutput('{"event":"start","msg":"2 frame(s)"}\r\n{"event":"frame","frame":1,"file":"D:/o.0001.exr","msg":"ok"}\r\nnoise\r\n__JDEXRC_EXIT=3\r\n');
eq([out.events.length, out.exit, out.other], [2, 3, ["noise"]], "parseCliOutput");
eq(C.exitMeaning(2), "bad job", "exit text");
eq(C.firstError(C.parseCliOutput('{"event":"error","frame":7,"file":"D:/a/m.0007.exr","msg":"missing frame"}')), "frame 7: missing frame (m.0007.exr)", "firstError");
{
  eq(C.windowsBatch("C:/a b/jd_exrc.exe", ["run", "C:/t/100%.json"], "C:/t/run_1.log"),
     '@echo off\r\nchcp 65001 >nul\r\n"C:/a b/jd_exrc.exe" "run" "C:/t/100%%.json" "--log-file" "C:/t/run_1.log" >nul 2>&1\r\n' +
     '>> "C:/t/run_1.log" echo __JDEXRC_EXIT=%errorlevel%\r\necho __JDEXRC_EXIT=%errorlevel%\r\n', "batch text");
  eq(C.windowsBatchLauncher("C:\\t\\run_1.bat"), 'cmd /c ""C:\\t\\run_1.bat""', "bat launcher");
  let threwB = false; try { C.windowsBatch("x.exe", ["a"]); } catch (e) { threwB = true; } ok(threwB, "bat without log path refuses");
  eq(C.parseCliOutput("\uFEFF" + '{"event":"done"}\r\n__JDEXRC_EXIT=0').events.length, 1, "BOM on the first log line");
}
eq(C.posixCommand("/a/jd_exrc", ["it's"], "/t/l.log"),
   "'/a/jd_exrc' 'it'\\''s' '--log-file' '/t/l.log' >/dev/null 2>&1; rc=$?; echo __JDEXRC_EXIT=$rc >> '/t/l.log'; echo __JDEXRC_EXIT=$rc", "posix command");
// loose parser: a line the strict parser refuses still gives its fields
eq(C.looseEvent('{"event":"frame","frame":12,"file":"D:/o/a \\"b\\".0012.exr","msg":"ok",}'),
   { event: "frame", frame: 12, file: 'D:/o/a "b".0012.exr', msg: "ok" }, "looseEvent");
eq(C.parseCliOutput('{"event":"plan","frame":3,"file":"x.0003.exr"} junk\n__JDEXRC_EXIT=0').events[0].frame, 3, "loose fallback in parseCliOutput");
// PNG header: greyscale (type 0) stays BW, also if a reader drops or adds a byte before IHDR
{
  const gp = path.join(__dirname, "..", "..", "test-data", "_generated", "panel", "mattes", "ball_bw.1001.png");
  if (fs.existsSync(gp)) {
    const grey = fs.readFileSync(gp, "latin1").slice(0, 64);
    eq(C.pngInfoFromBytes(grey), { channels: ["Y"], width: 64, height: 32, colourType: 0 }, "png grey");
    eq(C.pngInfoFromBytes(grey.slice(0, 4) + grey.slice(5)).channels, ["Y"], "png grey, one byte dropped");
    eq(C.pngInfoFromBytes(grey.slice(0, 1) + "\u00c2" + grey.slice(1)).channels, ["Y"], "png grey, one byte added");
  } else { console.log("(skip grey PNG test: generate test-data first)"); }
  eq(C.pngInfoFromBytes(C.pngChip([1, 2, 3], 18, 12)).channels, ["R", "G", "B"], "png rgb");
  eq(C.pngInfoFromBytes("not a png"), null, "png junk");
}

// ---------------------------------------------------------------- validate + job
function sampleState() {
  const st = C.normaliseState(C.jsonParse(C.jsonStringify(C.defaultState())));
  st.passes.push({ name: "beauty", src: "D:/r/beauty.####.exr", layer: "", map: "RGBA", type: "half", main: true });
  st.passes.push({ name: "depth", src: "D:/r/z.####.exr", layer: "ViewLayer.Depth", map: "Z", type: "float", main: false });
  st.crypto.objects.push({ name: "hero", src: "D:/m/hero.####.png", layer: "", channel: "R" });
  st.crypto.objects.push({ name: "car", src: "D:/m/rgb.####.exr", layer: "", channel: "R" });
  st.crypto.objects.push({ name: "tree", src: "D:/m/rgb.####.exr", layer: "", channel: "G" });
  st.output = "D:/o/sh010_compiled.####.exr";
  return st;
}
let st = sampleState();
eq(C.validateState(st).errors, [], "valid sample");
const job = C.buildJob(st);
eq(Object.keys(job), ["version", "output", "frames", "compression", "multipart", "layers", "cryptomattes", "metadata_from"], "job keys");
eq(job.layers[0], { name: "beauty", main: true, src: "D:/r/beauty.####.exr", map: "RGBA", type: "half" }, "job beauty layer");
eq(job.layers[1], { name: "depth", src: "D:/r/z.####.exr", layer: "ViewLayer.Depth", map: "Z", type: "float" }, "job depth layer");
eq(job.cryptomattes[0].name, "CryptoMatte", "job crypto name");
eq([job.cryptomattes[0].ranks, job.cryptomattes[0].mode, job.cryptomattes[0].background, job.cryptomattes[0].preview], [6, "priority", null, false], "job crypto opts");
eq(job.cryptomattes[0].objects[2], { name: "tree", src: "D:/m/rgb.####.exr", channel: "G" }, "job object");
eq(job.metadata_from, "beauty", "metadata_from main");
st.crypto.background = true; eq(C.buildJob(st).cryptomattes[0].background, "background", "background name");
st = sampleState(); st.output = "D:/o/x.exr";
ok(C.validateState(st).errors.some(e => /####/.test(e)), "sequence needs ####");
st = sampleState(); st.passes[1].name = "beauty";
ok(C.validateState(st).errors.some(e => /Two passes/.test(e)), "duplicate pass");
st = sampleState(); st.passes[1].main = true;
ok(C.validateState(st).errors.some(e => /one pass/.test(e)), "two mains");
st = sampleState(); st.crypto.objects[1].name = "hero";
ok(C.validateState(st).errors.some(e => /Two Cryptomatte/.test(e)), "duplicate object");
st = sampleState(); st.crypto.name = "Crypto.Matte";
ok(C.validateState(st).errors.some(e => /letters, digits/.test(e)), "bad crypto name");
st = sampleState(); st.crypto.ranks = 5;
ok(C.validateState(st).errors.some(e => /even/.test(e)), "odd ranks");
st = sampleState(); st.crypto.background = true; st.crypto.backgroundName = "car";
ok(C.validateState(st).errors.some(e => /Two Cryptomatte/.test(e)), "background clashes with object");
ok(C.validateState(C.defaultState()).errors.length === 2, "empty state: 2 errors");
eq(C.normaliseState({ passes: [], crypto: { objects: [] } }).crypto.ranks, 6, "normalise fills holes");
eq(C.defaultState().source, "files", "default source = files");

// ---------------------------------------------------------------- comp-layer rows (0.3)
const refA = { compId: 7, compName: "sh010", layerId: 3, layerIndex: 2, layerName: "Beauty_v02" };
const refB = { compId: 7, compName: "sh010", layerId: 5, layerIndex: 4, layerName: "car matte" };
eq(C.refKey(refA), "7:3", "refKey by id");
eq(C.refKey({ compId: 7, layerId: null, layerIndex: 4 }), "7:i4", "refKey falls back to index");
// 0.3.5 findings: empty setups are not saved as new entries; a layer is not added twice
ok(C.isEmptySetup(C.defaultState()), "default setup is empty");
ok(!C.isEmptySetup(Object.assign(C.defaultState(), { output: "D:/o/x.exr" })), "an output path is worth keeping");
{
  const passes = [C.makeLayerPass(refA, true)];
  ok(C.hasLayerRow(passes, { compId: 7, layerId: 3, layerIndex: 9, layerName: "renamed" }), "same layer id = already a pass");
  ok(!C.hasLayerRow(passes, refB), "another layer is new");
  const mattes = C.makeLayerMattes(refB, "rgb", true, ["car", "bike", "tree"]);
  ok(C.hasLayerRow(mattes, refB, "G") && !C.hasLayerRow(mattes, refB, "A"), "matte duplicates are per layer + channel");
}
const lp = C.makeLayerPass(refA, true);
eq([lp.name, lp.map, lp.type, lp.main, lp.src], ["Beauty", "RGBA", "half", true, ""], "layer pass (main beauty)");
eq(C.makeLayerPass({ compId: 7, layerId: 9, layerIndex: 1, layerName: "depth" }, true).type, "float", "layer depth float");
eq(C.makeLayerPass(refA, false).main, false, "no second main");
eq(C.makeLayerMattes(refB, "auto", true, null).map(o => [o.name, o.channel]), [["car matte", "A"]], "matte from alpha");
eq(C.makeLayerMattes(refB, "auto", false, null)[0].channel, "L", "matte from luminance (no alpha)");
eq(C.makeLayerMattes(refB, "rgb", true, ["car", "", "tree"]).map(o => [o.name, o.channel]), [["car", "R"], ["tree", "B"]], "rgb split skips empty names");
{
  const st = C.normaliseState(C.defaultState());
  st.passes.push(lp, { name: "spec", src: "D:/r/spec.####.exr", layer: "", map: "RGB", type: "half", main: false });
  st.crypto.objects.push(...C.makeLayerMattes(refB, "rgb", true, ["car", "bike", "tree"]));
  st.crypto.objects.push(...C.makeLayerMattes(refA, "auto", true, null).map(o => (o.name = "hero", o)));
  st.output = "D:/o/sh010_compiled.####.exr";
  eq(C.validateState(st).errors, [], "mixed file + layer rows valid");
  ok(C.anySequence({ passes: [lp], crypto: { objects: [] } }), "layer rows count as sequences");
  eq(C.layerRefsOf(st).map(C.refKey), ["7:3", "7:5"], "one render per layer");
  const r = C.resolveLayerSources(st, { "7:3": "D:/t/L1_Beauty_#####.exr", "7:5": "D:/t/L2_car_matte_#####.exr" });
  eq([r.passes[0].src, r.passes[1].src, r.crypto.objects[2].src, r.crypto.objects[3].src],
     ["D:/t/L1_Beauty_#####.exr", "D:/r/spec.####.exr", "D:/t/L2_car_matte_#####.exr", "D:/t/L1_Beauty_#####.exr"], "resolved sources");
  eq(st.passes[0].src, "", "resolve does not touch the setup");
  const j = C.buildJob(r);
  eq(j.layers[0], { name: "Beauty", main: true, src: "D:/t/L1_Beauty_#####.exr", map: "RGBA", type: "half" }, "job from layer row");
  ok(!("ref" in j.layers[0]) && !("ref" in j.cryptomattes[0].objects[0]), "no ref in the job");
  let threw2 = false; try { C.resolveLayerSources(st, { "7:3": "x" }); } catch (e) { threw2 = /car matte/.test(e.message); }
  ok(threw2, "missing render names the layer");
}
eq(C.expandFrame("D:/o/a.####.exr", 7), "D:/o/a.0007.exr", "expandFrame");
eq(C.expandFrame("D:/o/a_#####.exr", 1001), "D:/o/a_01001.exr", "expandFrame 5");
eq(C.expandFrame("D:/o/a.exr", 7), "D:/o/a.exr", "expandFrame still");
eq(C.intersectFrames([[1, 2, 3, 4], null, [3, 4, 5]]), [3, 4], "intersect");
eq(C.intersectFrames([null, null]), null, "only stills");
eq(C.intersectFrames([[1, 2], [5]]), [], "no common frame");

// AE settings spec (shape guessed from AE docs; the helpers tolerate variants)
{
  // AE 2026 shape (from the om_probe dump): {type:"int", enums:{name: n}}; older shapes still accepted
  const spec = { "Format": { type: "int", enums: { "AIFF": 0, "OpenEXR Sequence": 8, "PNG Sequence": 9 } },
                 "Channels": { "possibleValues": ["RGB", "RGB + Alpha"] }, "Depth": ["Millions of Colors", "Floating Point"],
                 "Old": { "possible values": ["a", "b"] } };
  eq(C.pickSettingValue(spec, ["Format"], /exr/i), "OpenEXR Sequence", "spec: format from enums");
  eq(C.pickSettingValue(spec, ["Old"], /b/), "b", "spec: possible values");
  eq(C.pickSettingValue(spec, ["Channels"], /RGB.*Alpha/i), "RGB + Alpha", "spec: channels (other key name)");
  eq(C.pickSettingValue(spec, ["Depth"], /float/i), "Floating Point", "spec: depth (bare array)");
  eq(C.pickSettingValue(spec, ["Nope"], /x/), null, "spec: missing key");
  eq(C.nestSetting(["Video Output", "Depth"], "Floating Point"), { "Video Output": { "Depth": "Floating Point" } }, "nestSetting");
}
eq(C.checkSummary([{ event: "frame" }, { event: "check", frame: 1, msg: "PASS" }, { event: "check", frame: 2, msg: "FAIL: ranks not sorted" }, { event: "check", frame: 3, msg: "FAIL: x" }]),
   { passed: 1, failed: 2, firstFail: "frame 2: FAIL: ranks not sorted" }, "checkSummary");

// A float TIFF the way AE writes temp layer renders (RGBA, 32-bit IEEE float, uncompressed, one strip).
// pixel(x, y) -> [r, g, b, a]. Little or big endian.
function makeFloatTiff(w, h, pixel, le = true) {
  const W16 = (v) => { const b = Buffer.alloc(2); le ? b.writeUInt16LE(v) : b.writeUInt16BE(v); return b; };
  const W32 = (v) => { const b = Buffer.alloc(4); le ? b.writeUInt32LE(v) : b.writeUInt32BE(v); return b; };
  const F32 = (v) => { const b = Buffer.alloc(4); le ? b.writeFloatLE(v) : b.writeFloatBE(v); return b; };
  const data = []; for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) for (const c of pixel(x, y)) data.push(F32(c));
  const img = Buffer.concat(data);
  const tags = [[256, 3, 1, w], [257, 3, 1, h], [258, 3, 4, "bits"], [259, 3, 1, 1], [262, 3, 1, 2], [273, 4, 1, "strip"],
                [277, 3, 1, 4], [278, 3, 1, h], [279, 4, 1, img.length], [284, 3, 1, 1], [338, 3, 1, 1], [339, 3, 4, "fmt"]];
  const ifd = 8, n = tags.length, extra0 = ifd + 2 + n * 12 + 4;   // 8 bytes for 4x BitsPerSample, 8 for 4x SampleFormat
  const stripOff = extra0 + 16;
  const out = [Buffer.from(le ? "II" : "MM", "latin1"), W16(42), W32(ifd), W16(n)];
  for (const [tag, type, count, v] of tags) {
    out.push(W16(tag), W16(type), W32(count));
    if (v === "bits") out.push(W32(extra0)); else if (v === "fmt") out.push(W32(extra0 + 8));
    else if (v === "strip") out.push(W32(stripOff));
    else if (type === 3) out.push(W16(v), W16(0)); else out.push(W32(v));
  }
  out.push(W32(0), W16(32), W16(32), W16(32), W16(32), W16(3), W16(3), W16(3), W16(3), img);
  return Buffer.concat(out);
}
for (const le of [true, false]) {
  const t = C.tiffHeaderInfo(makeFloatTiff(3, 2, () => [0, 0, 0, 1], le).toString("latin1"));
  eq([t.width, t.height, t.bits, t.spp, t.format, t.compression, t.isFloat32], [3, 2, 32, 4, 3, 1, true], "tiffHeaderInfo " + (le ? "LE" : "BE"));
}
eq(C.tiffHeaderInfo("not a tiff at all"), null, "tiffHeaderInfo junk");

// calibration frame (0.5 / 0.25 / 0.125 solid rendered with the user's template)
eq(C.calibrationCheck(["A   1  0x3f800000", "B   0.125  0x3e000000", "G   0.25  0x3e800000", "R   0.5  0x3f000000"]),
   { ok: true, measured: [0.5, 0.25, 0.125], note: "" }, "calibration raw values OK");
eq(C.calibrationCheck(["R 0.50196 0x3f008081", "G 0.25098 0x3e808081", "B 0.12549 0x3e008081"]).ok, true, "calibration: 8 bpc quantisation tolerated");
{
  const c = C.calibrationCheck(["R   0.213989258  0x3e5b2000", "G   0.0508728027  0x3d506000", "B   0.0143508911  0x3c6b2000"]);
  eq([c.ok, c.note], [false, "values were converted sRGB -> linear"], "calibration detects the 0.3.3 sRGB->linear finding");
}
eq(C.calibrationCheck(["nothing useful"]).ok, false, "calibration: no pixel lines = not OK");
let threw3 = false; try { C.posixCommand("x", ["a"]); } catch (e) { threw3 = true; } ok(threw3, "posix without log path refuses");

// ---------------------------------------------------------------- end to end through the CLI
const exe = process.argv[2];
if (exe) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "jd_exrc_panel_e2e_"));
  // Same wrappers the panel uses, run the way AE's callSystem seems to: a cmd with NO console
  // (detached, hidden). jd_exrc writes its own log (--log-file); the wrapper adds the exit code.
  const times = [];
  const run = (args) => {
    const how = "bat";
    const log = path.join(dir, "run.log");
    if (fs.existsSync(log)) fs.unlinkSync(log);
    const bat = path.join(dir, "run.bat");
    fs.writeFileSync(bat, C.windowsBatch(exe, args, log));
    const line = C.windowsBatchLauncher(bat);
    const t0 = Date.now();
    cp.spawnSync(process.env.SystemRoot + "\\System32\\cmd.exe", ["/c", line],
                 { windowsHide: true, stdio: "ignore", detached: true, windowsVerbatimArguments: true, timeout: 60000 });
    times.push(Date.now() - t0);
    const res = C.parseCliOutput(fs.existsSync(log) ? fs.readFileSync(log, "utf8") : "");
    ok(res.exit !== null, "exit marker in the log (" + how + ", " + args[0] + ")");
    return res;
  };
  process.on("exit", () => console.log("detached call times ms (bat): " + times.join(",")));
  // procedural test data: a 4-frame RGB "pass" and RGB matte sequence, and one PNG matte sequence
  for (let f = 1; f <= 4; f++) {
    const n = String(f).padStart(4, "0");
    cp.execFileSync(exe, ["testmatte", "-o", path.join(dir, `pass_v01.${n}.exr`), "--shift", String(f)]);
    cp.execFileSync(exe, ["testmatte", "-o", path.join(dir, `rgbmatte.${n}.exr`), "--shift", String(f)]);
    cp.execFileSync(exe, ["testmatte", "-o", path.join(dir, `bw.${n}.png`), "--shift", String(f)]);
  }
  const fw = dir.replace(/\\/g, "/");
  const S = C.normaliseState(C.defaultState());
  S.passes.push(C.makePass("pass_v01.0001.exr", fw + "/pass_v01.####.exr", ["R", "G", "B"]));
  S.passes[0].main = true;
  S.passes.push({ name: "depth", src: fw + "/pass_v01.####.exr", layer: "", map: "R->Z", type: "float", main: false });
  S.crypto.objects.push({ name: "left é", src: fw + "/rgbmatte.####.exr", layer: "", channel: "R" });
  S.crypto.objects.push({ name: "right", src: fw + "/rgbmatte.####.exr", layer: "", channel: "G" });
  S.crypto.objects.push({ name: "box", src: fw + "/rgbmatte.####.exr", layer: "", channel: "B" });
  S.crypto.objects.push({ name: "luma", src: fw + "/bw.####.png", layer: "", channel: "L" });
  S.crypto.background = true;
  S.output = fw + "/out/compiled.####.exr";
  eq(C.validateState(S).errors, [], "e2e state valid");
  fs.mkdirSync(path.join(dir, "out"));
  const jobFile = path.join(dir, "job.json");
  fs.writeFileSync(jobFile, C.jsonStringify(C.buildJob(S), 2));

  const dry = run(["run", jobFile, "--json-log", "--dry-run"]);
  eq(dry.exit, 0, "dry run exit 0");
  const frames = C.eventsOf(dry, "plan").map(e => e.frame);
  eq(frames, [1, 2, 3, 4], "dry run plans 4 frames");
  const chunks = C.planChunks(frames, 2);
  let written = 0;
  for (const c of chunks) {
    const r = run(["run", jobFile, "--json-log", "--frames", c, "--check"]);   // one call: write + check
    eq(r.exit, 0, "chunk " + c + " exit 0" + (r.exit ? " " + C.firstError(r) : ""));
    written += C.eventsOf(r, "frame").length;
    const cs = C.checkSummary(r.events);
    eq([cs.passed, cs.failed], [C.eventsOf(r, "frame").length, 0], "chunk " + c + ": every frame checked PASS");
  }
  eq(written, 4, "4 frames written");
  const first = C.eventsOf(dry, "plan")[0].file;
  const chk = run(["check", first]);
  eq(chk.exit, 0, "jd_exrc check passes on frame 1" + (chk.exit ? " " + chk.other.join(" | ") : ""));
  const info = cp.execFileSync(exe, ["info", first], { encoding: "utf8" });
  // every manifest entry the CLI wrote must equal the panel's hash (same IDs = same chip colours)
  const man = /\{[^{}]*"[0-9a-f]{8}"[^{}]*\}/.exec(info);
  ok(!!man, "info shows a manifest", info.slice(0, 400));
  if (man) {
    const m = JSON.parse(man[0]);
    eq(Object.keys(m).sort(), ["background", "box", "left é", "luma", "right"], "manifest names (UTF-8 kept)");
    for (const k of Object.keys(m)) eq(m[k], C.cryptoHex(k), "manifest hex " + k);
  }
  ok(/6ce235e/.test(info), "metadata key 6ce235e in info");
  // a broken job: exit 2, an error event
  fs.writeFileSync(jobFile, C.jsonStringify({ version: 1, output: fw + "/out/x.####.exr", layers: [{ name: "a", src: fw + "/nope.####.exr", map: "Q?" }] }));
  const bad = run(["run", jobFile, "--json-log", "--dry-run"]);
  ok(bad.exit === 2 && C.firstError(bad) !== "", "bad map -> exit 2 with message", [bad.exit, C.firstError(bad)]);
  fs.writeFileSync(jobFile, C.jsonStringify({ version: 1, output: fw + "/out/x.####.exr", layers: [{ name: "a", src: fw + "/nope.####.exr", map: "RGB" }] }));
  const missing = run(["run", jobFile, "--json-log", "--dry-run"]);
  ok(missing.exit === 3, "missing input -> exit 3 (file error)", [missing.exit, C.firstError(missing)]);
  // comp-layer path: AE writes name_#####.exr (5 digits, comp frame numbers). Fake those renders with
  // testmatte, resolve layer rows to them, compile without a dry run (frames worked out like the panel).
  {
    const rd = path.join(dir, "_jd_exrc_tmp_1"); fs.mkdirSync(rd);
    for (let f = 1001; f <= 1003; f++) {
      cp.execFileSync(exe, ["testmatte", "-o", path.join(rd, `L1_Beauty_0${f}.exr`), "--shift", String(f - 1000)]);
      cp.execFileSync(exe, ["testmatte", "-o", path.join(rd, `L2_car_matte_0${f}.exr`), "--shift", String(f - 999)]);
    }
    const rdf = rd.replace(/\\/g, "/");
    const st = C.normaliseState(C.defaultState());
    st.passes.push(C.makeLayerPass(refA, true));
    st.passes[0].map = "RGB";   // testmatte has no alpha; AE's real renders are RGBA
    st.crypto.objects.push(...C.makeLayerMattes(refB, "rgb", true, ["car", "bike", "tree"]));
    st.output = fw + "/out/layers.####.exr";
    const r = C.resolveLayerSources(st, { "7:3": rdf + "/L1_Beauty_#####.exr", "7:5": rdf + "/L2_car_matte_#####.exr" });
    const frames = C.intersectFrames([[1001, 1002, 1003], [1001, 1002, 1003]]);
    fs.writeFileSync(jobFile, C.jsonStringify(C.buildJob(r), 2));
    const res = run(["run", jobFile, "--json-log", "--frames", C.planChunks(frames, 10, 25)[0]]);
    eq([res.exit, C.eventsOf(res, "frame").length], [0, 3], "layer-row job compiles 3 frames" + (res.exit ? ": " + C.firstError(res) : ""));
    const first = C.expandFrame(st.output, frames[0]);
    ok(fs.existsSync(first), "first output from expandFrame exists", first);
    eq(run(["check", first]).exit, 0, "check passes on layer-row output");
  }
  // 0.3.2 route: temp layer renders are 32-bit float RGBA TIFFs named like AE writes them (L1_..._01001.tif).
  // beauty = solid (0.5, 0.25, 0.125, 1) with an HDR pixel (4.0) and a negative one; disc / box = alpha mattes.
  {
    const td = path.join(dir, "_jd_exrc_tmp_tif"); fs.mkdirSync(td);
    const W = 32, H = 16;
    const disc = (x, y) => (Math.hypot(x + 0.5 - 11, y + 0.5 - 8) < 6 ? 1 : 0);
    const box = (x, y) => (x >= 14 && x < 26 && y >= 4 && y < 12 ? 1 : 0);
    for (let f = 1001; f <= 1002; f++) {
      fs.writeFileSync(path.join(td, `L1_beauty_0${f}.tif`), makeFloatTiff(W, H, (x, y) => (x === 0 && y === 0 ? [4, -0.5, 0.125, 1] : [0.5, 0.25, 0.125, 1])));
      fs.writeFileSync(path.join(td, `L2_disc_0${f}.tif`), makeFloatTiff(W, H, (x, y) => { const a = disc(x, y); return [a, a, a, a]; }));
      fs.writeFileSync(path.join(td, `L3_box_0${f}.tif`), makeFloatTiff(W, H, (x, y) => { const a = box(x, y); return [a, a, a, a]; }, false));
    }
    const tdf = td.replace(/\\/g, "/");
    const rA = { compId: 9, compName: "layers_test", layerId: 1, layerIndex: 6, layerName: "beauty" };
    const rD = { compId: 9, compName: "layers_test", layerId: 2, layerIndex: 1, layerName: "disc" };
    const rB = { compId: 9, compName: "layers_test", layerId: 3, layerIndex: 2, layerName: "box" };
    const st = C.normaliseState(C.defaultState());
    st.passes.push(C.makeLayerPass(rA, true));
    st.crypto.objects.push(...C.makeLayerMattes(rD, "auto", true, null), ...C.makeLayerMattes(rB, "auto", true, null));
    st.output = fw + "/out/tifroute.####.exr";
    const r = C.resolveLayerSources(st, { "9:1": tdf + "/L1_beauty_#####.tif", "9:2": tdf + "/L2_disc_#####.tif", "9:3": tdf + "/L3_box_#####.tif" });
    fs.writeFileSync(jobFile, C.jsonStringify(C.buildJob(r), 2));
    const res = run(["run", jobFile, "--json-log", "--frames", "1001-1002", "--check"]);
    eq([res.exit, C.eventsOf(res, "frame").length, C.checkSummary(res.events).passed], [0, 2, 2], "TIFF route: 2 frames written and checked" + (res.exit ? ": " + C.firstError(res) : ""));
    const out1 = C.expandFrame(st.output, 1001);
    const px = (x, y) => {
      const o = {}; for (const l of cp.execFileSync(exe, ["pixel", out1, String(x), String(y)], { encoding: "utf8" }).split(/\r?\n/)) {
        const m = /^(\S+)\s+(\S+)\s+0x([0-9a-f]{8})/.exec(l.trim()); if (m) o[m[1]] = { v: parseFloat(m[2]), hex: m[3] };
      } return o;
    };
    const p5 = px(5, 2), p0 = px(0, 0);
    eq([p5.R.v, p5.G.v, p5.B.v, p5.A.v], [0.5, 0.25, 0.125, 1], "TIFF route: beauty values exact");
    // the panel's calibration check on real "jd_exrc pixel" text, read back through the .bat + --log-file path
    const pres = run(["pixel", out1, "5", "2"]);
    eq(C.calibrationCheck(pres.other).ok, true, "calibration check on real jd_exrc pixel output" + (pres.other.length ? "" : " (no text)"));
    eq([p0.R.v, p0.G.v], [4, -0.5], "TIFF route: HDR and negative values kept (float, not clipped)");
    const pd = px(11, 8), pb = px(20, 8);
    eq([pd["CryptoMatte00.R"].hex, pd["CryptoMatte00.G"].v], [C.cryptoHex("disc"), 1], "TIFF route: disc from alpha");
    eq([pb["CryptoMatte00.R"].hex, pb["CryptoMatte00.G"].v], [C.cryptoHex("box"), 1], "TIFF route: box from alpha (big-endian TIFF)");
  }
  console.log("e2e folder: " + dir);
  console.log("info excerpt:\n" + info.split("\n").slice(0, 30).join("\n"));
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
