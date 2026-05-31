@echo off
title Engine Dashboard
setlocal
set PYTHON=C:\Users\Administrator.AVENTADOR\AppData\Local\Programs\Python\Python310\python.exe
set PYTHONPATH=I:\FGLS_new\collection\python_src;I:\ZGLS;I:\storage-cleaner\src
echo.
echo === Engine Dashboard ===
echo Port 8766 - Storage Cleaner, Geometry Store, Bermuda Router, and more
echo.
echo Access:   http://127.0.0.1:8766/dashboard
echo API docs: http://127.0.0.1:8766/docs
echo.
"%PYTHON%" "%~dp0python_src\engine_dashboard.py" %*
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Press any key to close...
    pause >nul
)
