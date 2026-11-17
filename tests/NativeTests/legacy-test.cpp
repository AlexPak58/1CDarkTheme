#include <windows.h>
extern "C" __declspec(dllexport) DWORD WINAPI SetThemeProfile(LPVOID)
{
    return ERROR_NOT_SUPPORTED;
}
