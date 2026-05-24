@echo off
REM Run Bond ↔ GeoPixel bridge tests
cd /d "%~dp0"
gcc -O2 -I. -Igeopixel/geopixel -o test_bond_gp test_bond_geopixel.c && test_bond_gp
