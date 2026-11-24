@echo off
title ASHA RO WATER - Web Server
color 0A
echo.
echo  ==========================================
echo    ASHA RO WATER - Starting Web Server
echo  ==========================================
echo.

SET PATH=C:\Program Files\nodejs;%PATH%

cd /d "%~dp0"

echo  Starting server... Please wait...
echo.
node server.js

echo.
echo  Server stopped. Press any key to restart.
pause
goto :eof
