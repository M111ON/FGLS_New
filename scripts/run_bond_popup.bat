@echo off
title Bond Layer Popup
setlocal

set PYTHONPATH=I:\FGLS_new\collection\python_src;I:\ZGLS
set BOND_API_URL=http://127.0.0.1:8000
set PYTHON=C:\Users\Administrator.AVENTADOR\AppData\Local\Programs\Python\Python310\python.exe

"%PYTHON%" "%~dp0python_src\bond_popup.pyw" %*
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Press any key to close...
    pause >nul
)
