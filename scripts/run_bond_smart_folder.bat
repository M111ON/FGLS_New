@echo off
title Smart Folder + Bond Layer
setlocal

set POGLS_SO_PATH=I:\FGLS_new\collection\pogls_bond.dll
set BOND_EXT_PATH=I:\FGLS_new\collection\python_src
set PYTHONPATH=I:\FGLS_new\collection\python_src;I:\ZGLS

set PYTHON=C:\Users\Administrator.AVENTADOR\AppData\Local\Programs\Python\Python310\python.exe

"%PYTHON%" -m pip install uvicorn fastapi python-multipart -q 2>nul

echo.
echo === Smart Folder + Bond Layer v1.1 ===
echo API:  http://127.0.0.1:8000
echo Docs: http://127.0.0.1:8000/docs
echo UI:   http://127.0.0.1:8000/bond-ui
echo.
echo Tkinter GUI: run_bond_popup.bat
echo Context menu: run_bond_popup.bat install
echo.

"%PYTHON%" -m smart_folder_mvp serve
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo ERROR: Failed to start.
    pause
)
