// Read-back for the panel 0.3 render-queue test: expected vs measured values of a compiled frame.
//   node readback.js <jd_exrc.exe> <compiled frame, e.g. .../layers_test_compiled.1001.exr> <label> <report.txt>
// Appends one block to the report. Exit code 0 = every check within tolerance, 1 = at least one DIFF.
// Expected values come from tests/ae_make_test_project.jsx (comp layers_test).
"use strict";
const cp = require("child_process");
const fs = require("fs");
const [exe, frame, label, report] = process.argv.slice(2);
if (!exe || !frame || !label || !report) {
  console.log("usage: node readback.js <jd_exrc.exe> <compiled frame .exr> <label> <report.txt>");
  process.exit(2);
}
const lines = [`== ${label}  ${new Date().toISOString()}  ${frame}`];
let diffs = 0;

function pixel(x, y) {
  const out = cp.execFileSync(exe, ["pixel", frame, String(x), String(y)], { encoding: "utf8" });
  const px = {};
  for (const l of out.split(/\r?\n/)) {
    const m = /^(\S+)\s+(\S+)\s+0x([0-9a-f]{8})/.exec(l.trim());
    if (m) px[m[1]] = { v: parseFloat(m[2]), hex: m[3] };
  }
  return px;
}
function check(what, got, want, tol) {
  const okv = got !== undefined && Math.abs(got - want) <= tol;
  if (!okv) diffs++;
  lines.push(`${okv ? "OK  " : "DIFF"}  ${what.padEnd(34)} expected ${want}  measured ${got === undefined ? "(missing)" : got}`);
}

// Values: solids written by AE as half or float; 1e-3 covers half precision.
const p1 = pixel(10, 10);
check("beauty R (bare) @10,10", p1.R && p1.R.v, 0.5, 1e-3);
check("beauty G (bare) @10,10", p1.G && p1.G.v, 0.25, 1e-3);
check("beauty B (bare) @10,10", p1.B && p1.B.v, 0.125, 1e-3);
check("spec.R @10,10", p1["spec.R"] && p1["spec.R"].v, 0.1, 1e-3);
check("spec.G @10,10", p1["spec.G"] && p1["spec.G"].v, 0.2, 1e-3);
check("spec.B @10,10", p1["spec.B"] && p1["spec.B"].v, 0.3, 1e-3);

// Cryptomatte: rank 0 at the disc centre is disc, right part of the box is box, both fully covered.
const info = cp.execFileSync(exe, ["info", frame], { encoding: "utf8" });
const mm = /cryptomatte\/([0-9a-f]{7})\/manifest = (\{.*\})/.exec(info);
let manifest = {};
if (mm) { manifest = JSON.parse(mm[2]); lines.push(`      manifest key ${mm[1]}: ${Object.keys(manifest).join(", ")}`); }
else { diffs++; lines.push("DIFF  no Cryptomatte manifest in the file"); }
const mName = /channel (\S+?)00\.R\b/.exec(info);
const name = mName ? mName[1] : "CryptoMatte";
for (const [obj, x, y] of [["disc", 44, 32], ["box", 85, 32]]) {
  const px = pixel(x, y);
  const id = px[name + "00.R"], cov = px[name + "00.G"];
  const okId = id && manifest[obj] === id.hex;
  if (!okId) diffs++;
  lines.push(`${okId ? "OK  " : "DIFF"}  rank 0 @${x},${y} is ${obj.padEnd(19)} expected ${manifest[obj] || "?"}  measured ${id ? id.hex : "(missing)"}`);
  check(`${obj} coverage @${x},${y}`, cov && cov.v, 1, 1e-3);
}
const types = (info.match(/channel (R|spec\.R)\s+(half|float)/g) || []).join("; ");
lines.push(`      pixel types: ${types}`);
lines.push(diffs ? `RESULT ${diffs} DIFF(s)` : "RESULT all OK", "");
fs.appendFileSync(report, lines.join("\n") + "\n");
console.log(lines.join("\n"));
process.exit(diffs ? 1 : 0);
