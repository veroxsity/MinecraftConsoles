#pragma once
#include <windows.h>

#define LCE_SHARED_MEMORY_NAME L"Global\\LCE_SURFACE_DATA"

#define LCE_STATE_WAITING      0
#define LCE_STATE_SC_REQUESTED 1
#define LCE_STATE_SC_READY     2
#define LCE_STATE_SC_FAILED    3

#define LCE_PRESENT_IDLE       0
#define LCE_PRESENT_REQUESTED  1
#define LCE_PRESENT_DONE       2

struct LCE_SharedData
{
    void*        coreWindowUnknown;
    UINT         width;
    UINT         height;
    volatile LONG state;
    DWORD        launcherPid;
    void*        pDevice;
    void*        pImmediateContext;
    void*        pSwapChain;
    // Present marshalling — game sets REQUESTED, CoreWindow thread calls Present and sets DONE
    volatile LONG presentState;
    UINT          syncInterval;
    UINT          presentFlags;
    // VSync toggle — written by game DLL, read by CoreWindow present loop.
    // 0 = uncapped (default), 1 = lock to display refresh.
    volatile LONG vsyncEnabled;
};
