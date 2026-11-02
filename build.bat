@echo off
rem Build ImageHover.exe with MinGW-w64 gcc.
rem   build.bat         -> release build (no logging)
rem   build.bat debug   -> developer build, writes ImageHover.log next to the exe
rem The icon (res\imagehover.ico) is made by: python tools\make_icon.py
cd /d "%~dp0"
set EXTRA=-s
if /i "%~1"=="debug" set EXTRA=-g -DIH_DEBUG

windres -I res -O coff -i res\app.rc -o res\app.res
if errorlevel 1 (
    echo Resource build FAILED.
    exit /b 1
)

gcc -O2 -Wall -Wextra -Wno-missing-field-initializers %EXTRA% -municode -mwindows -I res ^
    -o ImageHover.exe src\main.c src\locate.c src\loader.c src\render.c src\settings.c src\pdf.c res\app.res ^
    -lole32 -loleaut32 -luuid -lshell32 -lshlwapi -lwindowscodecs -lshcore -lgdi32 -luser32 ^
    -lcomctl32 -ladvapi32 -lruntimeobject
if errorlevel 1 (
    echo Build FAILED.
    exit /b 1
)
if /i "%~1"=="debug" (echo Built ImageHover.exe [debug, logging on]) else (echo Built ImageHover.exe)
