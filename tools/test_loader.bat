@echo off
rem Build and run the loader/render test harness on the files in testdata\.
rem res\app.res is linked in for the manifest (Common Controls v6 is needed).
cd /d "%~dp0\.."
windres -I res -O coff -i res\app.rc -o res\app.res
if errorlevel 1 exit /b 1
gcc -O1 -Wall -Wextra -Wno-missing-field-initializers -DIH_TEST -municode -I res ^
    -o tools\test_loader.exe tools\test_loader.c src\loader.c src\render.c src\locate.c src\settings.c src\pdf.c res\app.res ^
    -lole32 -loleaut32 -luuid -lshell32 -lshlwapi -lwindowscodecs -lshcore -lgdi32 -luser32 ^
    -lcomctl32 -ladvapi32 -lruntimeobject
if errorlevel 1 exit /b 1
tools\test_loader.exe testdata\sample.png testdata\photo_exif.jpg testdata\anim.gif testdata\anim.webp testdata\sample.pdf testdata\clip.mp4 testdata\twin.jpg
