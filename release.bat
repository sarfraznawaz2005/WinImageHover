@echo off
rem Build ImageHover and publish a GitHub release. See release.ps1 for the steps.
rem Always start it as .\release.bat from this folder. A plain "release.bat" may run
rem another release.bat that is on your PATH.
rem   .\release.bat -SetVersion 1.1.0   set a new version and add a CHANGELOG section
rem   .\release.bat -DryRun             build, test and pack, publish nothing
rem   .\release.bat                     build, test, pack and publish (asks first)
rem   .\release.bat -Draft              same, as a draft release
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0release.ps1" %*
exit /b %errorlevel%
