@echo off
REM Uninstall FGLS from Windows Context Menu
REM Run this once as Administrator

echo Removing FGLS from context menu...

reg delete "HKCR\*\shell\FGLS_Compress" /f
reg delete "HKCR\*\shell\FGLS_Decompress" /f
reg delete "HKCR\*\shell\FGLS_Info" /f

echo.
echo Done! FGLS removed from context menu.
echo.
pause
