# jd_exrc end-to-end test (run by CTest): cmake -DEXE=... -DWORK=... -P cli_test.cmake
# Uses only procedural test mattes made by `jd_exrc testmatte`.
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/seq")
set(FAILS 0)

function(expect code)
  execute_process(COMMAND "${EXE}" ${ARGN} WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  if(NOT rc EQUAL code)
    message("FAIL (exit ${rc}, expected ${code}): ${ARGN}\n${out}")
    math(EXPR n "${FAILS} + 1")
    set(FAILS ${n} PARENT_SCOPE)
  else()
    message("ok   ${ARGN}")
  endif()
  set(LAST_OUT "${out}" PARENT_SCOPE)
endfunction()

function(expect_in text)
  string(REGEX MATCH "${text}" hit "${LAST_OUT}")
  if(NOT hit)
    message("FAIL: '${text}' not in output:\n${LAST_OUT}")
    math(EXPR n "${FAILS} + 1")
    set(FAILS ${n} PARENT_SCOPE)
  endif()
endfunction()

# Inputs
expect(0 testmatte -o t.exr)
expect(0 testmatte -o t.png)
expect(0 testmatte -o seq/s.0001.exr --shift 0)
expect(0 testmatte -o seq/s.0002.exr --shift 1)
expect(0 testmatte -o seq/s.0003.exr --shift 2)

# Cryptomatte from EXR and from PNG, spec check
expect(0 crypto -o c.exr --matte left,right,box=t.exr:RGB --background bg)
expect(0 check c.exr)
expect_in("PASS")
expect(0 crypto -o cp.exr --name CryptoObject --matte left,right,box=t.png:RGB --preview)
expect(0 check cp.exr)
expect(0 info cp.exr)
expect_in("CryptoObject[.]R")
expect_in("cryptomatte/3ae39a5/name = CryptoObject")

# TIFF mattes: 32-bit float strips (.tif) and 16-bit planar tiles (.tiff) must give the same result as EXR
expect(0 testmatte -o t.tif)
expect(0 testmatte -o t16.tiff)
expect(0 crypto -o ct.exr --matte left,right,box=t.tif:RGB --background bg)
expect(0 check ct.exr)
expect(0 pixel ct.exr 32 16)
expect_in("CryptoMatte00[.]G +0[.]5 ")
expect(0 crypto -o ct16.exr --matte left,right,box=t16.tiff:RGB)
expect(0 check ct16.exr)
expect(0 pixel ct16.exr 31 16)
expect_in("CryptoMatte00[.]G +0[.]7499")  # 0.75 in 16-bit = 49151/65535

# Two half edges stay 0.5 / 0.5 in priority mode (pixel 32,16: left and right at 0.5 under the box)
expect(0 pixel c.exr 32 16)
expect_in("CryptoMatte00[.]G +0[.]5 ")
expect_in("CryptoMatte00[.]A +0[.]5 ")

# Merge passes + a Cryptomatte in one file
expect(0 merge -o m.exr --layer beauty=t.exr:RGB --layer depth=t.exr:R->Z --layer mat=t.png:RGB --matte left=t.exr:R)
expect(0 info m.exr)
expect_in("channel R +half")
expect_in("depth[.]Z +float")
expect_in("mat[.]G +half")
expect(0 check m.exr)

# Multi-part
expect(0 merge -o mp.exr --multipart --layer beauty=t.exr:RGB --layer mat=t.png:RGB --matte left,right=t.exr:RG)
expect(0 info mp.exr)
expect_in("3 part[(]s[)]")
expect(0 check mp.exr)

# Sequence, frames auto
expect(0 crypto -o "out/c.####.exr" --matte "left,right,box=seq/s.####.exr:RGB" --threads 2)
foreach(f 0001 0002 0003)
  if(NOT EXISTS "${WORK}/out/c.${f}.exr")
    message("FAIL: out/c.${f}.exr missing")
    math(EXPR FAILS "${FAILS} + 1")
  endif()
endforeach()
expect(0 check out/c.0003.exr)

# Job file, frame range, manifest sidecar, JSON log
file(WRITE "${WORK}/job.json" [=[
{
  "version": 1,
  "output": "job_out/shot.%04d.exr",
  "frames": "2-3",
  "layers": [ {"name": "beauty", "main": true, "src": "seq/s.####.exr", "map": "RGB"} ],
  "cryptomattes": [ {"name": "CryptoMatte", "manifest_sidecar": true,
    "objects": [ {"name": "left", "src": "seq/s.####.exr", "channel": "R"},
                 {"name": "right", "src": "seq/s.####.exr", "channel": "G"},
                 {"name": "box", "src": "seq/s.####.exr", "channel": "B"} ] } ]
}
]=])
expect(0 run job.json --json-log)
expect_in("event.:.done")
if(EXISTS "${WORK}/job_out/shot.0001.exr" OR NOT EXISTS "${WORK}/job_out/shot.0003.exr" OR NOT EXISTS "${WORK}/job_out/shot_CryptoMatte.json")
  message("FAIL: job frames or sidecar wrong")
  math(EXPR FAILS "${FAILS} + 1")
endif()
expect(0 check job_out/shot.0002.exr)
expect(0 run job.json --dry-run)

# --log-file: same lines in the file as on stdout (AE's callSystem drops stdout)
expect(0 run job.json --json-log --log-file job_log.txt)
file(READ "${WORK}/job_log.txt" LOGTXT)
string(REGEX MATCH "event.:.done" hit "${LOGTXT}")
if(NOT hit)
  message("FAIL: --log-file has no done event:\n${LOGTXT}")
  math(EXPR FAILS "${FAILS} + 1")
endif()
expect(0 check c.exr --log-file check_log.txt)
file(READ "${WORK}/check_log.txt" LOGTXT)
string(REGEX MATCH "PASS" hit "${LOGTXT}")
if(NOT hit)
  message("FAIL: check --log-file has no PASS")
  math(EXPR FAILS "${FAILS} + 1")
endif()
expect(2 run job.json --log-file)

# --log-file on the other commands too
foreach(c "--version" "info;c.exr" "pixel;c.exr;1;1" "testmatte;-o;lf.exr")
  expect(0 ${c} --log-file lf_log.txt)
  file(READ "${WORK}/lf_log.txt" LOGTXT)
  if(LOGTXT STREQUAL "")
    message("FAIL: --log-file empty for ${c}")
    math(EXPR FAILS "${FAILS} + 1")
  endif()
endforeach()

# --check: run and spec-check in one call
expect(0 run job.json --json-log --check)
expect_in("event.:.check.,.frame.:3,.*PASS")
expect(0 crypto -o checked.exr --check --matte left,right=t.exr:RG)
expect_in("check.*PASS")

# Errors and exit codes
expect(2 crypto -o bad.exr --compression dwaa --matte left=t.exr:R)
expect(3 crypto -o bad.exr --matte left=missing.exr:R)
expect(2 crypto -o bad.exr --matte a,a=t.exr:RG)
expect(3 crypto -o "bad.####.exr" --frames 1-5 --matte "left=seq/s.####.exr:R")
expect(0 crypto -o "hold.####.exr" --frames 1-5 --hold-missing --matte "left=seq/s.####.exr:R")
expect(2 merge -o bad.exr --layer x=t.exr:R --layer x=t.exr:G)
expect(1 check t.exr)
expect_in("no Cryptomatte metadata")

if(FAILS GREATER 0)
  message(FATAL_ERROR "${FAILS} check(s) failed")
endif()
message("all CLI checks passed")
