@echo off
title ASHA RO WATER - Card Scan Bridge
color 0B
echo.
echo  ==========================================
echo    ASHA RO WATER - Card Scan Bridge
echo  ==========================================
echo.
echo  Yeh window KHULI RAKHO jab customer add karo.
echo  Card scan karte hi UID web form mein jayega!
echo.
echo  Band karne ke liye: Ctrl+C ya window band karo
echo.

powershell.exe -ExecutionPolicy Bypass -File "%~dp0CARD_SCAN_BRIDGE.ps1"

echo.
pause
