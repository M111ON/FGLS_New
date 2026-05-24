@echo off
REM Run TGW→FGLS connector tests
cd /d "%~dp0"
gcc -O2 -I. -Icore/core -Icore/pogls_engine -D__USE_MINGW_ANSI_STDIO -o test_tgw_fgls test_tgw_fgls.c && test_tgw_fgls
