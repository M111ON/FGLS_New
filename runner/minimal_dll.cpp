// minimal_dll.cpp - test if loading any MSVC-built DLL causes crash
#include <windows.h>
BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
extern "C" __declspec(dllexport) int do_nothing() { return 0; }
