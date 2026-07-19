@echo off
REM Install FGLS to Windows Context Menu
REM Run this once as Administrator

echo Adding FGLS to context menu...

REM Compress: Right-click → FGLS Compress
reg add "HKCR\*\shell\FGLS_Compress" /ve /t REG_SZ /d "FGLS Compress" /f
reg add "HKCR\*\shell\FGLS_Compress" /v "Icon" /t REG_SZ /d "\"I:\FGLS_new\fgls.exe\"" /f
reg add "HKCR\*\shell\FGLS_Compress\command" /ve /t REG_SZ /d "\"I:\FGLS_new\fgls.exe\" compress \"%%1\"" /f

REM Decompress: Right-click → FGLS Decompress
reg add "HKCR\*\shell\FGLS_Decompress" /ve /t REG_SZ /d "FGLS Decompress" /f
reg add "HKCR\*\shell\FGLS_Decompress" /v "Icon" /t REG_SZ /d "\"I:\FGLS_new\fgls.exe\"" /f
reg add "HKCR\*\shell\FGLS_Decompress\command" /ve /t REG_SZ /d "\"I:\FGLS_new\fgls.exe\" decompress \"%%1\"" /f

REM Info: Right-click → FGLS Info
reg add "HKCR\*\shell\FGLS_Info" /ve /t REG_SZ /d "FGLS Info" /f
reg add "HKCR\*\shell\FGLS_Info" /v "Icon" /t REG_SZ /d "\"I:\FGLS_new\fgls.exe\"" /f
reg add "HKCR\*\shell\FGLS_Info\command" /ve /t REG_SZ /d "\"I:\FGLS_new\fgls.exe\" info \"%%1\"" /f

echo.
echo Done! Right-click any file to see FGLS options.
echo.
pause
