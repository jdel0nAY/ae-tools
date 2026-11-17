@echo off
rem Compile the PiPL resource like Adobe's sample projects.
rem %1 = cl.exe   %2 = SDK Examples dir   %3 = .r file   %4 = output dir
"%~1" /nologo /I "%~2\Headers" /I "%~dp3." /EP "%~3" > "%~4\FourDWarperPiPL.rr" || exit /b 1
"%~2\Resources\PiPLtool.exe" "%~4\FourDWarperPiPL.rr" "%~4\FourDWarperPiPL.rrc" || exit /b 1
"%~1" /nologo /D MSWindows /EP "%~4\FourDWarperPiPL.rrc" > "%~4\FourDWarperPiPL.rc" || exit /b 1
