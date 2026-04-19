#pragma once
#include <windows.h>

// When building the DLL itself, LCEAU_API = dllexport.
// When including from a consumer, LCEAU_API = dllimport.
#ifdef LCEAU_BUILDING_DLL
#  define LCEAU_API __declspec(dllexport)
#else
#  define LCEAU_API __declspec(dllimport)
#endif

extern "C"
{
    LCEAU_API HRESULT LCEAudio_Init();
    LCEAU_API HRESULT LCEAudio_LoadWaveBank(const wchar_t* path, const char* name);
    LCEAU_API HRESULT LCEAudio_LoadSoundBank(const wchar_t* path, const char* name);
    LCEAU_API void    LCEAudio_PlayCue(const char* bankName, const char* cueName, BOOL loop);
    LCEAU_API void    LCEAudio_StopCue(const char* cueName);
    LCEAU_API void    LCEAudio_SetMasterVolume(float vol);
    LCEAU_API void    LCEAudio_Shutdown();
}
