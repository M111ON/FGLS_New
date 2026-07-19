@echo off
REM fgls — User-Friendly Wrapper (Windows)
REM
REM Usage:
REM   fgls compress <file>           — auto-compress
REM   fgls decompress <file.fgls>    — auto-decompress
REM   fgls info <file>               — show file info
REM   fgls benchmark <file>          — benchmark all routes
REM   fgls sid <file>                — SID capture (Pro tier)
REM   fgls version                   — show version
REM   fgls help                      — show help

setlocal

set FGLS_EXE=%~dp0fgls.exe
set PRO_EXE=%~dp0pro_cli.exe

if "%1"=="" goto help
if "%1"=="help" goto help
if "%1"=="--help" goto help
if "%1"=="-h" goto help
if "%1"=="version" goto version
if "%1"=="--version" goto version
if "%1"=="-v" goto version
if "%1"=="compress" goto compress
if "%1"=="c" goto compress
if "%1"=="decompress" goto decompress
if "%1"=="d" goto decompress
if "%1"=="info" goto info
if "%1"=="i" goto info
if "%1"=="benchmark" goto bench
if "%1"=="bench" goto bench
if "%1"=="b" goto bench
if "%1"=="sid" goto sid
if "%1"=="s" goto sid

echo Unknown command: %1
echo.
goto help

:help
echo FGLS Universal Codec v2.0.0
echo.
echo Usage:
echo   fgls compress ^<file^> [output.fgls]   Compress a file
echo   fgls decompress ^<file.fgls^> [output]  Decompress a file
echo   fgls info ^<file^>                       Show file details
echo   fgls benchmark ^<file^>                  Benchmark all routes
echo   fgls sid ^<file^> [output.twidx]         SID capture (Pro tier)
echo   fgls version                           Show version
echo   fgls help                              Show this help
echo.
echo Examples:
echo   fgls compress model.bin                -^> model.bin.fgls
echo   fgls decompress model.bin.fgls         -^> model.bin
echo   fgls info model.bin                    -^> file details
echo   fgls benchmark model.bin               -^> speed comparison
echo   fgls sid model.bin                     -^> SID coordinates
goto end

:version
echo FGLS Universal Codec v2.0.0
"%FGLS_EXE%" version
goto end

:compress
if "%2"=="" (
    echo Usage: fgls compress ^<input^> [output.fgls]
    goto end
)
if not exist "%FGLS_EXE%" (
    echo Error: fgls.exe not found. Run 'make' first.
    goto end
)
echo Compressing: %2
"%FGLS_EXE%" encode %2 %3
goto end

:decompress
if "%2"=="" (
    echo Usage: fgls decompress ^<input.fgls^> [output]
    goto end
)
if not exist "%FGLS_EXE%" (
    echo Error: fgls.exe not found. Run 'make' first.
    goto end
)
echo Decompressing: %2
"%FGLS_EXE%" decode %2 %3
goto end

:info
if "%2"=="" (
    echo Usage: fgls info ^<file^>
    goto end
)
echo File: %2
"%FGLS_EXE%" info %2
goto end

:bench
if "%2"=="" (
    echo Usage: fgls benchmark ^<file^>
    goto end
)
echo Benchmarking: %2
"%FGLS_EXE%" bench %2
goto end

:sid
if "%2"=="" (
    echo Usage: fgls sid ^<input^> [output.twidx]
    goto end
)
if not exist "%PRO_EXE%" (
    echo Error: pro_cli.exe not found. Run 'make' first.
    goto end
)
echo SID capturing: %2
"%PRO_EXE%" capture %2 %3
goto end

:end
endlocal
