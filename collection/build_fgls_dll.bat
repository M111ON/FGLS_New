@echo off
REM build_fgls_dll.bat — Build pogls_fgls.dll on Windows (MinGW)
REM Run from collection root (where tgw_fgls_connector.h is accessible)
REM
REM Prerequisites:
REM   MinGW-w64 in PATH (gcc, ar)
REM   pogls_bond.dll already built (dependency for bond layer)
REM
REM Output:
REM   build\pogls_fgls.dll
REM   build\libpogls_fgls.a

setlocal
set OUT_DIR=build
set SRC=pogls_fgls_export.c

if not exist %OUT_DIR% mkdir %OUT_DIR%

echo [1/2] Compiling pogls_fgls.dll ...
gcc -O2 -shared ^
    -DPOGLS_FGLS_EXPORT_DLL ^
    -D__USE_MINGW_ANSI_STDIO ^
    -I. ^
    -Icore ^
    -Icore/core ^
    -Icore/pogls_engine ^
    -o %OUT_DIR%\pogls_fgls.dll %SRC% ^
    -Wl,--out-implib,%OUT_DIR%\libpogls_fgls.a

if errorlevel 1 (
    echo FAILED: check include paths above
    exit /b 1
)

echo [2/2] Verifying exports ...
dumpbin /exports %OUT_DIR%\pogls_fgls.dll 2>nul | findstr pogls_fgls
if errorlevel 1 (
    objdump -p %OUT_DIR%\pogls_fgls.dll | grep pogls_fgls
)

echo.
echo Done: %OUT_DIR%\pogls_fgls.dll
