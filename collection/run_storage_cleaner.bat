@echo off
title Storage Cleaner
setlocal
set PYTHON=C:\Users\Administrator.AVENTADOR\AppData\Local\Programs\Python\Python310\python.exe
set PYTHONPATH=I:\storage-cleaner\src
echo.
echo === Storage Cleaner ===
echo Scans directories for duplicates, trash, temp files
echo.
echo Commands:
echo   storage-cleaner scan ^<dir^>        Scan a directory
echo   storage-cleaner gui                 Launch Tkinter GUI
echo   storage-cleaner api                 Start API server on port 9077
echo.
"%PYTHON%" -m storage_cleaner.cli %*
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Press any key to close...
    pause >nul
)
