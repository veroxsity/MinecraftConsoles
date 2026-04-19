#include <windows.h>
#include <wchar.h>
#include <stdio.h>
#include <roapi.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.applicationmodel.core.h>
#include <windows.ui.core.h>
#include <windows.ui.viewmanagement.h>
#include <windows.foundation.h>
#include <windows.system.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include "SharedSurface.h"

using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
using namespace ABI::Windows::ApplicationModel::Core;
using namespace ABI::Windows::UI::Core;
using namespace ABI::Windows::Foundation;

static HANDLE          g_hMap    = nullptr;
static LCE_SharedData* g_pShared = nullptr;

// D3D objects created on the CoreWindow thread, passed to the game DLL
static ComPtr<ID3D11Device>        g_device;
static ComPtr<ID3D11DeviceContext> g_ctx;
static ComPtr<IDXGISwapChain1>     g_swapChain;
static D3D_FEATURE_LEVEL           g_fl = D3D_FEATURE_LEVEL_11_0;

static void WriteLog(const wchar_t* dir, const wchar_t* msg) {
    wchar_t path[MAX_PATH]; swprintf_s(path, L"%s\\lce_launch.log", dir);
    FILE* f = nullptr; if (_wfopen_s(&f, path, L"a") == 0 && f) { fwprintf(f, L"%s\n", msg); fclose(f); }
}

static bool OpenSharedMem() {
    g_hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        sizeof(LCE_SharedData), LCE_SHARED_MEMORY_NAME);
    if (!g_hMap) return false;
    g_pShared = (LCE_SharedData*)MapViewOfFile(g_hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LCE_SharedData));
    if (!g_pShared) { CloseHandle(g_hMap); g_hMap = nullptr; return false; }
    memset(g_pShared, 0, sizeof(LCE_SharedData));
    return true;
}

typedef int  (WINAPI* GameMainFn)(ID3D11Device*, ID3D11DeviceContext*, IDXGISwapChain*, D3D_FEATURE_LEVEL);
typedef void (*PfnXboxOnChar)(wchar_t);
typedef void (*PfnXboxOnKeyDown)(int);
typedef int  (*PfnXboxIsChatOpen)();

// Set by GameThread after DLL is loaded; read from CoreWindow thread (lambdas check for null).
static PfnXboxOnChar    g_pfnXboxOnChar    = nullptr;
static PfnXboxOnKeyDown g_pfnXboxOnKeyDown = nullptr;
static PfnXboxIsChatOpen g_pfnXboxIsChatOpen = nullptr;

// Game thread entry — loads MinecraftClient.dll and calls GameMain
struct GameThreadArgs { wchar_t dllPath[MAX_PATH]; wchar_t logDir[MAX_PATH]; };

static DWORD WINAPI GameThread(LPVOID param) {
    GameThreadArgs* args = (GameThreadArgs*)param;
    wchar_t logDir[MAX_PATH]; wcscpy_s(logDir, args->logDir);
    wchar_t dllPath[MAX_PATH]; wcscpy_s(dllPath, args->dllPath);
    delete args;

    WriteLog(logDir, L"GameThread: loading MinecraftClient.dll...");
    HMODULE hGame = LoadLibraryW(dllPath);
    if (!hGame) {
        wchar_t msg[64]; swprintf_s(msg, L"LoadLibrary FAILED err=0x%X", GetLastError());
        WriteLog(logDir, msg); return 1;
    }
    WriteLog(logDir, L"GameThread: DLL loaded, finding GameMain...");

    GameMainFn gameMain = (GameMainFn)GetProcAddress(hGame, "GameMain");
    if (!gameMain) {
        WriteLog(logDir, L"GameMain export not found");
        FreeLibrary(hGame); return 1;
    }
    // Keyboard bridge — used by CoreWindow event handlers to feed into g_KBMInput
    g_pfnXboxOnChar     = (PfnXboxOnChar)    GetProcAddress(hGame, "XboxOnChar");
    g_pfnXboxOnKeyDown  = (PfnXboxOnKeyDown) GetProcAddress(hGame, "XboxOnKeyDown");
    g_pfnXboxIsChatOpen = (PfnXboxIsChatOpen)GetProcAddress(hGame, "XboxIsChatOpen");
    WriteLog(logDir, L"GameThread: calling GameMain...");
    gameMain(g_device.Get(), g_ctx.Get(), g_swapChain.Get(), g_fl);
    WriteLog(logDir, L"GameThread: GameMain returned");
    FreeLibrary(hGame);
    return 0;
}

class App : public RuntimeClass<RuntimeClassFlags<WinRtClassicComMix>, IFrameworkView>
{
public:
    HRESULT STDMETHODCALLTYPE Initialize(ICoreApplicationView* view) override
    {
        m_view = view;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetWindow(ICoreWindow* window) override {
        m_window = window;

        // On Xbox, pressing B fires BackRequested on the SystemNavigationManager
        // for the CURRENT VIEW. Must use GetForCurrentView() (a static factory
        // method on the class, not GetActivationFactory on the interface) — this
        // is the documented fix per Microsoft's UWP-on-Xbox guidance.
        // Without this, the UWP shell suspends the app on every B press regardless
        // of what the game thread does.
        ComPtr<ABI::Windows::UI::Core::ISystemNavigationManagerStatics> navStatics;
        if (SUCCEEDED(GetActivationFactory(
                HStringReference(RuntimeClass_Windows_UI_Core_SystemNavigationManager).Get(),
                &navStatics)))
        {
            ComPtr<ABI::Windows::UI::Core::ISystemNavigationManager> navManager;
            if (SUCCEEDED(navStatics->GetForCurrentView(&navManager)))
            {
                EventRegistrationToken token;
                navManager->add_BackRequested(
                    Callback<ABI::Windows::Foundation::IEventHandler<
                        ABI::Windows::UI::Core::BackRequestedEventArgs*>>(
                        [](IInspectable*,
                           ABI::Windows::UI::Core::IBackRequestedEventArgs* args) -> HRESULT {
                            args->put_Handled(TRUE);
                            return S_OK;
                        }).Get(),
                    &token);
            }
        }

        // Route CoreWindow keyboard events into the game's g_KBMInput so
        // Screen::tick()'s ConsumeChar drain feeds ChatScreen::keyPressed.
        // CharacterReceived fires for every printable character plus backspace (0x08)
        // and enter (0x0D) from the InputPane.  KeyDown handles Escape separately
        // because it never produces a character event.
        {
            EventRegistrationToken charToken = {}, keyToken = {};

            window->add_CharacterReceived(
                Callback<ABI::Windows::Foundation::ITypedEventHandler<
                    ABI::Windows::UI::Core::CoreWindow*,
                    ABI::Windows::UI::Core::CharacterReceivedEventArgs*>>(
                    [](ABI::Windows::UI::Core::ICoreWindow*,
                       ABI::Windows::UI::Core::ICharacterReceivedEventArgs* args) -> HRESULT
                    {
                        if (!g_pfnXboxOnChar) return S_OK;
                        UINT32 code = 0;
                        args->get_KeyCode(&code);
                        // printable + backspace (0x08) + enter (0x0D)
                        if (code >= 0x20 || code == 0x08 || code == 0x0D)
                            g_pfnXboxOnChar(static_cast<wchar_t>(code));
                        return S_OK;
                    }).Get(),
                &charToken);

            window->add_KeyDown(
                Callback<ABI::Windows::Foundation::ITypedEventHandler<
                    ABI::Windows::UI::Core::CoreWindow*,
                    ABI::Windows::UI::Core::KeyEventArgs*>>(
                    [](ABI::Windows::UI::Core::ICoreWindow*,
                       ABI::Windows::UI::Core::IKeyEventArgs* args) -> HRESULT
                    {
                        ABI::Windows::System::VirtualKey vk =
                            ABI::Windows::System::VirtualKey_None;
                        args->get_VirtualKey(&vk);
                        // Escape doesn't produce a CharacterReceived event
                        if (vk == ABI::Windows::System::VirtualKey_Escape && g_pfnXboxOnKeyDown)
                            g_pfnXboxOnKeyDown(VK_ESCAPE);
                        return S_OK;
                    }).Get(),
                &keyToken);
        }

        window->Activate();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Load(HSTRING) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE Run() override
    {
        wchar_t selfPath[MAX_PATH], workDir[MAX_PATH];
        GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
        wchar_t* slash = wcsrchr(selfPath, L'\\');
        if (slash) { wcsncpy_s(workDir, selfPath, slash - selfPath); workDir[slash-selfPath] = L'\0'; }

        { wchar_t lp[MAX_PATH]; swprintf_s(lp, L"%s\\lce_launch.log", workDir);
          FILE* f=nullptr; _wfopen_s(&f,lp,L"w"); if(f) fclose(f); }
        WriteLog(workDir, L"LCE.Xbox Run() started");
        SetCurrentDirectoryW(workDir);

        // Create D3D device + CoreWindow swap chain on this thread
        WriteLog(workDir, L"Creating D3D...");
        HRESULT hr = CreateD3D();
        if (FAILED(hr)) {
            wchar_t msg[64]; swprintf_s(msg, L"CreateD3D FAILED 0x%08X", (unsigned)hr);
            WriteLog(workDir, msg); return E_FAIL;
        }
        WriteLog(workDir, L"D3D ready");

        // Launch game on a background thread (same process — no cross-process issues)
        wchar_t dllPath[MAX_PATH];
        swprintf_s(dllPath, L"%s\\MinecraftClient.dll", workDir);

        GameThreadArgs* args = new GameThreadArgs;
        wcscpy_s(args->dllPath, dllPath);
        wcscpy_s(args->logDir, workDir);

        HANDLE hThread = CreateThread(nullptr, 0, GameThread, args, 0, nullptr);
        if (!hThread) { WriteLog(workDir, L"CreateThread FAILED"); return E_FAIL; }
        WriteLog(workDir, L"Game thread started");

        // Main loop: pump CoreWindow events + service Present requests
        ComPtr<ICoreDispatcher> dispatcher;
        m_window->get_Dispatcher(&dispatcher);

        // InputPane: shows the Xbox soft keyboard when ChatScreen is active.
        // TryShow/TryHide are on IInputPane2 — QI from the base IInputPane.
        // Must be obtained on the CoreWindow thread.
        ComPtr<ABI::Windows::UI::ViewManagement::IInputPane2> inputPane;
        {
            ComPtr<ABI::Windows::UI::ViewManagement::IInputPaneStatics> statics;
            ComPtr<ABI::Windows::UI::ViewManagement::IInputPane> pane1;
            if (SUCCEEDED(GetActivationFactory(
                    HStringReference(RuntimeClass_Windows_UI_ViewManagement_InputPane).Get(),
                    &statics)) && SUCCEEDED(statics->GetForCurrentView(&pane1)))
                pane1.As(&inputPane); // QI to IInputPane2
        }
        bool lastChatOpen = false;

        while (WaitForSingleObject(hThread, 0) == WAIT_TIMEOUT) {
            dispatcher->ProcessEvents(CoreProcessEventsOption_ProcessAllIfPresent);

            // Show/hide Xbox soft keyboard in sync with ChatScreen open state
            if (inputPane && g_pfnXboxIsChatOpen) {
                const bool chatOpen = g_pfnXboxIsChatOpen() != 0;
                boolean bResult = FALSE;
                if (chatOpen && !lastChatOpen)  inputPane->TryShow(&bResult);
                if (!chatOpen && lastChatOpen)  inputPane->TryHide(&bResult);
                lastChatOpen = chatOpen;
            }

            // Service Present requests from game thread (must happen on CoreWindow thread)
            if (g_pShared && g_pShared->presentState == LCE_PRESENT_REQUESTED) {
                IDXGISwapChain* sc = static_cast<IDXGISwapChain*>(g_pShared->pSwapChain);
                if (sc) {
                    // Use our vsyncEnabled flag rather than the value the 4J lib writes
                    // into syncInterval (which is always 1). Default (zero-init) = uncapped.
                    const UINT si = g_pShared->vsyncEnabled ? 1u : 0u;
                    sc->Present(si, g_pShared->presentFlags);
                }
                InterlockedExchange(&g_pShared->presentState, LCE_PRESENT_DONE);
                // Present just happened — skip sleep so the game thread can
                // immediately start the next frame without waiting ~1ms.
            } else {
                // No present pending: yield briefly to avoid burning a whole core
                // on the CoreWindow thread while the game is rendering.
                Sleep(1);
            }
        }
        DWORD exitCode = 0; GetExitCodeThread(hThread, &exitCode);
        wchar_t msg[64]; swprintf_s(msg, L"Game thread exited: 0x%08X", exitCode);
        WriteLog(workDir, msg);
        CloseHandle(hThread);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Uninitialize() override { return S_OK; }

private:
    ComPtr<ICoreWindow> m_window;
    ComPtr<ICoreApplicationView> m_view;

    HRESULT CreateD3D() {
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,  // required for Iggy Flash interop
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            g_device.GetAddressOf(), &g_fl, g_ctx.GetAddressOf());
        if (FAILED(hr)) return hr;

        ComPtr<IDXGIDevice> dxgiDev; g_device.As(&dxgiDev);
        ComPtr<IDXGIAdapter> adapter; dxgiDev->GetAdapter(&adapter);
        ComPtr<IDXGIFactory2> factory;
        hr = adapter->GetParent(__uuidof(IDXGIFactory2), (void**)factory.GetAddressOf());
        if (FAILED(hr)) return hr;

        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width=1920; sd.Height=1080; sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc={1,0};
        sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
        // FLIP_DISCARD + 3 buffers: Present(0,0) returns immediately without stalling
        // on a vblank boundary.  FLIP_SEQUENTIAL with 2 buffers would block when both
        // buffers are consumed, effectively locking to the display refresh rate.
        sd.BufferCount=3; sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Scaling=DXGI_SCALING_STRETCH; sd.AlphaMode=DXGI_ALPHA_MODE_IGNORE;

        hr = factory->CreateSwapChainForCoreWindow(g_device.Get(),
            static_cast<IUnknown*>(m_window.Get()), &sd, nullptr, g_swapChain.GetAddressOf());
        {
            wchar_t msg[64]; swprintf_s(msg, L"CreateSwapChainForCoreWindow hr=0x%08X", (unsigned)hr);
            wchar_t dir[MAX_PATH]; GetModuleFileNameW(nullptr, dir, MAX_PATH);
            wchar_t* s = wcsrchr(dir, L'\\'); if(s) *(s+1)=L'\0';
            wchar_t lp[MAX_PATH]; swprintf_s(lp, L"%slce_launch.log", dir);
            FILE* f=nullptr; _wfopen_s(&f,lp,L"a"); if(f){fwprintf(f,L"%s\n",msg);fclose(f);}
        }
        if (FAILED(hr)) return hr;

        // Do an initial Present so the swap chain is fully initialized before game uses it
        g_swapChain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
        return S_OK;
    }
};

class AppSource : public RuntimeClass<RuntimeClassFlags<WinRtClassicComMix>, IFrameworkViewSource>
{
public:
    HRESULT STDMETHODCALLTYPE CreateView(IFrameworkView** view) override
    { return Make<App>().CopyTo(view); }
};

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    OpenSharedMem();
    RoInitialize(RO_INIT_MULTITHREADED);
    ComPtr<ICoreApplication> coreApp;
    GetActivationFactory(
        HStringReference(RuntimeClass_Windows_ApplicationModel_Core_CoreApplication).Get(),
        &coreApp);
    auto source = Make<AppSource>();
    coreApp->Run(source.Get());
    RoUninitialize();
    if (g_pShared) UnmapViewOfFile(g_pShared);
    if (g_hMap)    CloseHandle(g_hMap);
    return 0;
}
