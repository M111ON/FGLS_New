@echo off
title Storage Cleaner
setlocal

set PYTHONPATH=I:\FGLS_new\collection\python_src;I:\ZGLS
set PYTHON=I:\python3.14.4\python.exe

echo.
echo === Storage Cleaner ===
echo Scans directories for duplicates, trash, temp files
echo Uses POGLS topology fingerprint for smart duplicate detection
echo.

"%PYTHON%" "%~dp0python_src\storage_cleaner.pyw" %*
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Press any key to close...
    pause >nul
)
