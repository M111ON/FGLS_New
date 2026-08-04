@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
nvcc -O2 -std=c++17 -D_GNU_SOURCE -arch=sm_61 -I. -I..\.. -I..\..\collection -I..\..\collection\src -I..\..\collection\core\pogls_engine\twin_core -I..\..\collection\core\pogls_engine -I..\..\collection\core\pogls_engine\core -I..\..\collection\core\core -I..\..\collection\rdh -I..\..\runner -o gpu_jet_puller_local gpu_jet_puller.cu
pause