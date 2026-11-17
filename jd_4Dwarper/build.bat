@echo off
rem MoCo Align build (CPU, Release). Output: build\Release\moco_align_test.exe
set OUT=%~1
if "%AE_SDK%"=="" if exist "%~dp0local_sdk.txt" set /p AE_SDK=<"%~dp0local_sdk.txt"
if "%OUT%"=="" set OUT=build
set CMAKE="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
%CMAKE% -S "%~dp0." -B "%~dp0%OUT%" -G "Visual Studio 17 2022" -A x64 || exit /b 1
%CMAKE% --build "%~dp0%OUT%" --config Release -- /m /v:minimal || exit /b 1
