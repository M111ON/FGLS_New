@echo off
REM FGLS GUI Launcher
REM Double-click to start the FGLS Codec Dashboard in your browser
REM Server runs in background, browser opens automatically

cd /d "%~dp0"

echo FGLS Codec Dashboard
echo ====================
echo.

REM Check Python
python --version >nul 2>&1
if errorlevel 1 (
    echo ERROR: Python not found. Install Python 3 first.
    pause
    exit /b 1
)

REM Check server exists
if not exist fgls_gui_server.py (
    echo ERROR: fgls_gui_server.py not found in current directory.
    pause
    exit /b 1
)

REM Kill any existing FGLS server on port 8080
for /f "tokens=5" %%a in ('netstat -ano ^| findstr :8080 ^| findstr LISTENING') do (
    echo Stopping existing server on port 8080...
    taskkill /F /PID %%a >nul 2>&1
)
timeout /t 1 /nobreak >nul

echo Starting FGLS Server...
start /B python fgls_gui_server.py

REM Wait for server and open browser
timeout /t 2 /nobreak >nul
echo Opening FGLS Dashboard in your browser...
start http://localhost:8080

echo.
echo FGLS Dashboard is now running at http://localhost:8080
echo Close this window to stop the server.
echo.
echo Tip: Pin FGLS_GUI.bat to your taskbar for quick access
echo.
pause
taskkill /F /IM python.exe /FI "WINDOWTITLE eq FGLS*" >nul 2>&1
