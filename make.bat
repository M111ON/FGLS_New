@echo off
REM make.bat — Wrapper for mingw32-make
REM Usage: make.bat [target]
REM Example: make.bat test

/c/msys64/mingw64/bin/mingw32-make.exe %*
