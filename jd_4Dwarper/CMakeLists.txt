cmake_minimum_required(VERSION 3.24)
# Product name: jd_4Dwarper (0.3.4; was 4Dwarper). A leading digit is not a valid identifier, so targets and files use FourDWarper / warper_*.
project(FourDWarper LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_library(warper_core STATIC src/align_core.cpp)
target_compile_definitions(warper_core PUBLIC _CRT_SECURE_NO_WARNINGS)
add_executable(warper_test tests/moco_align_test.cpp)
target_link_libraries(warper_test PRIVATE warper_core)
if(MSVC)
  target_compile_options(warper_core PRIVATE /O2 /W3)
  target_compile_options(warper_test PRIVATE /O2 /W3)
  # warper_cli: the solve and render without After Effects (PNG folders in, PNG folder + records.json out).
  # PNG through Windows GDI+, so no third-party code.
  add_executable(warper_cli tools/warper_cli.cpp)
  target_link_libraries(warper_cli PRIVATE warper_core gdiplus)
  target_compile_options(warper_cli PRIVATE /O2 /W3)
  # w4d_verify: the effect's Analyze path (shared settings + record) against the CLI reference run, offline
  add_executable(w4d_verify tests/w4d_verify.cpp)
  target_link_libraries(w4d_verify PRIVATE warper_core gdiplus)
  target_compile_options(w4d_verify PRIVATE /O2 /W3)
endif()

# ---------------------------------------------------------------- After Effects plug-in -> build/dev/jd_4Dwarper.aex
# SDK referenced by path (Adobe licence: never copy it into git). Not installed by the build.
# The effect is written to build/dev. Never build straight into After Effects' live Plug-ins folder: AE locks
# the .aex while it runs, and a plug-in should be installed only after it has been tested.
# The SDK's Examples folder: -DAE_SDK=<path> or the AE_SDK environment variable (not included: Adobe licence).
# Without it the tools and tests still build; the effect is skipped with a warning.
set(AE_SDK "$ENV{AE_SDK}" CACHE PATH "After Effects SDK Examples folder")
if(NOT EXISTS "${AE_SDK}/Headers/AE_Effect.h")
  message(WARNING "After Effects SDK not found (AE_SDK = '${AE_SDK}'). The jd_4Dwarper effect is NOT built. "
                  "Download the SDK from Adobe and set AE_SDK to its Examples folder (-DAE_SDK=... or the environment variable).")
endif()
if(MSVC AND EXISTS "${AE_SDK}/Headers/AE_Effect.h")
  set(PIPL_RC "${CMAKE_BINARY_DIR}/FourDWarperPiPL.rc")
  add_custom_command(OUTPUT "${PIPL_RC}"
    COMMAND cmd /c "${CMAKE_SOURCE_DIR}/ae/pipl.bat" "${CMAKE_CXX_COMPILER}" "${AE_SDK}" "${CMAKE_SOURCE_DIR}/ae/FourDWarperPiPL.r" "${CMAKE_BINARY_DIR}"
    DEPENDS "${CMAKE_SOURCE_DIR}/ae/FourDWarperPiPL.r" "${CMAKE_SOURCE_DIR}/ae/FourDWarperVersion.h" "${CMAKE_SOURCE_DIR}/ae/pipl.bat")
  add_library(warper_ae SHARED ae/FourDWarper.cpp
    "${AE_SDK}/Util/AEGP_SuiteHandler.cpp" "${AE_SDK}/Util/MissingSuiteError.cpp" "${AE_SDK}/Util/AEFX_SuiteHelper.c" "${PIPL_RC}")
  set_target_properties(warper_ae PROPERTIES OUTPUT_NAME "jd_4Dwarper" SUFFIX ".aex"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dev" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/dev")
  target_include_directories(warper_ae PRIVATE "${AE_SDK}/Headers" "${AE_SDK}/Headers/SP" "${AE_SDK}/Headers/Win" "${AE_SDK}/Resources" "${AE_SDK}/Util")
  target_compile_definitions(warper_ae PRIVATE MSWindows WIN32 _WINDOWS _CRT_SECURE_NO_WARNINGS)
  target_compile_options(warper_ae PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/O2 /W3>)
  target_link_libraries(warper_ae PRIVATE warper_core)
endif()
