// AudioShim.cpp
// Implements a minimal XACT3-compatible interface backed by XAudio2.
// Built as xactengine3_7.dll placed alongside Minecraft.Client.exe.
// The game loads XACT dynamically via CoCreateInstance / LoadLibrary.

#include "AudioShim.h"
#include "XWBParser.h"
#include "XSBParser.h"
#include <xaudio2.h>
#include <wrl/client.h>
#include <unordered_map>
#include <string>
#include <vector>
#include <mutex>

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Global XAudio2 state
// ---------------------------------------------------------------------------

static ComPtr<IXAudio2>        g_xaudio;
static IXAudio2MasteringVoice* g_masterVoice = nullptr;
static std::mutex              g_mutex;

// Loaded banks: filename stem → parser
static std::unordered_map<std::string, XWBParser> g_waveBanks;
static std::unordered_map<std::string, XSBParser> g_soundBanks;

// Active source voices keyed by cue name (one voice per cue, simplification)
static std::unordered_map<std::string, IXAudio2SourceVoice*> g_activeVoices;

static bool g_initialised = false;

static bool EnsureXAudio2()
{
    if (g_initialised) return true;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialised) return true;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;

    hr = XAudio2Create(g_xaudio.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) return false;

    hr = g_xaudio->CreateMasteringVoice(&g_masterVoice);
    if (FAILED(hr)) return false;

    g_initialised = true;
    return true;
}

// ---------------------------------------------------------------------------
// Play a sound by cue name — called from the public API below
// ---------------------------------------------------------------------------

static void PlayCue(const std::string& bankName, const std::string& cueName, bool loop)
{
    if (!EnsureXAudio2()) return;

    auto sbIt = g_soundBanks.find(bankName);
    if (sbIt == g_soundBanks.end()) return;

    const XSBCue* cue = sbIt->second.FindCue(cueName);
    if (!cue) return;

    // Find the wave bank — LCE uses matching stem names (minecraft.xsb → resident.xwb)
    const XWBSound* sound = nullptr;
    for (auto& [wbName, wb] : g_waveBanks)
    {
        sound = wb.GetSound(cue->waveIndex);
        if (sound) break;
    }
    if (!sound || sound->data.empty()) return;

    // Stop existing voice for this cue if any
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_activeVoices.find(cueName);
        if (it != g_activeVoices.end())
        {
            it->second->Stop();
            it->second->DestroyVoice();
            g_activeVoices.erase(it);
        }
    }

    WAVEFORMATEX fmt = {};
    fmt.wFormatTag      = sound->isADPCM ? WAVE_FORMAT_ADPCM : WAVE_FORMAT_PCM;
    fmt.nChannels       = sound->channels;
    fmt.nSamplesPerSec  = sound->sampleRate;
    fmt.wBitsPerSample  = sound->bitsPerSample;
    fmt.nBlockAlign     = sound->blockAlign;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    fmt.cbSize          = 0;

    IXAudio2SourceVoice* voice = nullptr;
    if (FAILED(g_xaudio->CreateSourceVoice(&voice, &fmt))) return;

    XAUDIO2_BUFFER buf = {};
    buf.pAudioData = sound->data.data();
    buf.AudioBytes = static_cast<UINT32>(sound->data.size());
    buf.Flags      = XAUDIO2_END_OF_STREAM;
    buf.LoopCount  = loop ? XAUDIO2_LOOP_INFINITE : 0;

    if (FAILED(voice->SubmitSourceBuffer(&buf))) { voice->DestroyVoice(); return; }
    voice->SetVolume(cue->volume);
    voice->Start(0);

    std::lock_guard<std::mutex> lock(g_mutex);
    g_activeVoices[cueName] = voice;
}

static void StopCue(const std::string& cueName)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_activeVoices.find(cueName);
    if (it != g_activeVoices.end())
    {
        it->second->Stop();
        it->second->DestroyVoice();
        g_activeVoices.erase(it);
    }
}

// ---------------------------------------------------------------------------
// Public C API — exported from xactengine3_7.dll
// The game calls these via the XACT3 COM interface; we expose a flat C API
// that the game can call after we've intercepted the XACT3 factory.
// ---------------------------------------------------------------------------

extern "C"
{

// Called by the game to initialise the audio engine
__declspec(dllexport) HRESULT LCEAudio_Init()
{
    return EnsureXAudio2() ? S_OK : E_FAIL;
}

// Load a wave bank file (.xwb)
__declspec(dllexport) HRESULT LCEAudio_LoadWaveBank(const wchar_t* path, const char* name)
{
    EnsureXAudio2();
    XWBParser parser;
    if (!parser.Load(path)) return E_FAIL;
    g_waveBanks[name] = std::move(parser);
    return S_OK;
}

// Load a sound bank file (.xsb)
__declspec(dllexport) HRESULT LCEAudio_LoadSoundBank(const wchar_t* path, const char* name)
{
    EnsureXAudio2();
    XSBParser parser;
    if (!parser.Load(path)) return E_FAIL;
    g_soundBanks[name] = std::move(parser);
    return S_OK;
}

// Play a cue by name
__declspec(dllexport) void LCEAudio_PlayCue(const char* bankName, const char* cueName, BOOL loop)
{
    PlayCue(bankName, cueName, loop != 0);
}

// Stop a cue
__declspec(dllexport) void LCEAudio_StopCue(const char* cueName)
{
    StopCue(cueName);
}

// Set master volume (0.0 - 1.0)
__declspec(dllexport) void LCEAudio_SetMasterVolume(float vol)
{
    if (g_masterVoice) g_masterVoice->SetVolume(vol);
}

// Shutdown
__declspec(dllexport) void LCEAudio_Shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& [name, voice] : g_activeVoices)
    {
        voice->Stop();
        voice->DestroyVoice();
    }
    g_activeVoices.clear();

    if (g_masterVoice) { g_masterVoice->DestroyVoice(); g_masterVoice = nullptr; }
    g_xaudio.Reset();
    g_initialised = false;
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        EnsureXAudio2();
    else if (reason == DLL_PROCESS_DETACH)
        LCEAudio_Shutdown();
    return TRUE;
}
