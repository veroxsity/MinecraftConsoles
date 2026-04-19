// DXGIShim.cpp
// Proxy dxgi.dll — intercepts IDXGIFactory::CreateSwapChain and redirects
// it to a swap chain created on the UWP CoreWindow thread by LCE.Xbox.exe.

#include <windows.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <wrl/client.h>
#include <stdio.h>
#include "../../LCE.Xbox/SharedSurface.h"

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Real dxgi.dll function pointers
// ---------------------------------------------------------------------------
static HMODULE g_realDxgi = nullptr;

typedef HRESULT(WINAPI* PFN_CreateDXGIFactory )(REFIID, void**);
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory1)(REFIID, void**);
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory2)(UINT, REFIID, void**);

static PFN_CreateDXGIFactory  g_realFactory  = nullptr;
static PFN_CreateDXGIFactory1 g_realFactory1 = nullptr;
static PFN_CreateDXGIFactory2 g_realFactory2 = nullptr;

static IUnknown* g_coreWindow = nullptr;

static void LoadRealDxgi()
{
    if (g_realDxgi) return;
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\dxgi.dll");

    // Log what we're trying to load
    wchar_t dllDir[MAX_PATH] = {};
    GetModuleFileNameW((HMODULE)g_realDxgi, dllDir, MAX_PATH); // won't work yet, use alternate
    // Get our own DLL's directory instead
    {
        HMODULE hSelf = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&LoadRealDxgi, &hSelf);
        GetModuleFileNameW(hSelf, dllDir, MAX_PATH);
        wchar_t* s = wcsrchr(dllDir, L'\\');
        if (s) *(s+1) = L'\0';
    }
    wchar_t logPath[MAX_PATH];
    swprintf_s(logPath, L"%slce_dxgi.log", dllDir);

    FILE* f = nullptr;
    _wfopen_s(&f, logPath, L"a");

    if (f) fwprintf(f, L"LoadRealDxgi: trying %s\n", path);

    g_realDxgi = LoadLibraryW(path);

    if (f)
    {
        fwprintf(f, L"LoadRealDxgi: result %p  err=0x%X\n", (void*)g_realDxgi, g_realDxgi ? 0 : GetLastError());
        fclose(f);
    }

    if (!g_realDxgi) return;
    g_realFactory  = (PFN_CreateDXGIFactory) GetProcAddress(g_realDxgi, "CreateDXGIFactory");
    g_realFactory1 = (PFN_CreateDXGIFactory1)GetProcAddress(g_realDxgi, "CreateDXGIFactory1");
    g_realFactory2 = (PFN_CreateDXGIFactory2)GetProcAddress(g_realDxgi, "CreateDXGIFactory2");
}

static void ReadCoreWindow()
{
    if (g_coreWindow) return;
    HANDLE hMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, L"Local\\LCE_SURFACE_DATA");
    if (!hMap) return;
    LCE_SharedData* p = (LCE_SharedData*)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LCE_SharedData));
    if (p) { g_coreWindow = static_cast<IUnknown*>(p->coreWindowUnknown); UnmapViewOfFile(p); }
    CloseHandle(hMap);
}



// ---------------------------------------------------------------------------
// WrappedDXGIFactory — wraps a real IDXGIFactory2 and overrides CreateSwapChain
// We inherit from IDXGIFactory1 only (avoids unimplemented IDXGIFactory2 purities)
// and forward everything else to the real factory via QI.
// ---------------------------------------------------------------------------
class WrappedDXGIFactory : public IDXGIFactory1
{
public:
    WrappedDXGIFactory(IDXGIFactory2* real) : m_real(real), m_ref(1) {}

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef()  override { InterlockedIncrement(&m_ref); return m_ref; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = InterlockedDecrement(&m_ref);
        if (r == 0) { m_real->Release(); delete this; }
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown)       ||
            riid == __uuidof(IDXGIObject)     ||
            riid == __uuidof(IDXGIFactory)    ||
            riid == __uuidof(IDXGIFactory1))
        {
            *ppv = static_cast<IDXGIFactory1*>(this);
            AddRef(); return S_OK;
        }
        // For IDXGIFactory2+ callers just give them the real factory
        return m_real->QueryInterface(riid, ppv);
    }

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void* d) override
        { return m_real->SetPrivateData(g, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* u) override
        { return m_real->SetPrivateDataInterface(g, u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* s, void* d) override
        { return m_real->GetPrivateData(g, s, d); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID r, void** ppv) override
        { return m_real->GetParent(r, ppv); }

    // IDXGIFactory
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT i, IDXGIAdapter** a) override
        { return m_real->EnumAdapters(i, a); }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND w, UINT f) override
        { return m_real->MakeWindowAssociation(w, f); }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* w) override
        { return m_real->GetWindowAssociation(w); }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE m, IDXGIAdapter** a) override
        { return m_real->CreateSoftwareAdapter(m, a); }

    // CreateSwapChain — on Xbox the game bypasses this via InitDevice directly.
    // On PC this falls through to the real factory.
    HRESULT STDMETHODCALLTYPE CreateSwapChain(
        IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSC) override
    {
        return m_real->CreateSwapChain(pDevice, pDesc, ppSC);
    }

    // IDXGIFactory1
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT i, IDXGIAdapter1** a) override
        { return m_real->EnumAdapters1(i, a); }
    BOOL STDMETHODCALLTYPE IsCurrent() override
        { return m_real->IsCurrent(); }

private:
    IDXGIFactory2* m_real;
    volatile ULONG m_ref;
};

// ---------------------------------------------------------------------------
// Exported entry points
// ---------------------------------------------------------------------------
static WrappedDXGIFactory* MakeWrapped(IDXGIFactory2* real)
{
    return new WrappedDXGIFactory(real);
}

extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppv)
{
    LoadRealDxgi(); ReadCoreWindow();
    if (!g_realFactory) return DXGI_ERROR_UNSUPPORTED;
    ComPtr<IDXGIFactory2> real;
    HRESULT hr = g_realFactory(__uuidof(IDXGIFactory2), (void**)real.GetAddressOf());
    if (FAILED(hr)) return hr;
    auto* w = MakeWrapped(real.Detach());
    hr = w->QueryInterface(riid, ppv);
    w->Release();
    return hr;
}

extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppv)
{
    return CreateDXGIFactory(riid, ppv);
}

extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** ppv)
{
    LoadRealDxgi(); ReadCoreWindow();
    if (!g_realFactory2) return DXGI_ERROR_UNSUPPORTED;
    ComPtr<IDXGIFactory2> real;
    HRESULT hr = g_realFactory2(flags, __uuidof(IDXGIFactory2), (void**)real.GetAddressOf());
    if (FAILED(hr)) return hr;
    auto* w = MakeWrapped(real.Detach());
    hr = w->QueryInterface(riid, ppv);
    w->Release();
    return hr;
}

extern "C" HRESULT WINAPI DXGIGetDebugInterface(REFIID r, void** p)
{
    LoadRealDxgi();
    auto fn = (PFN_CreateDXGIFactory)GetProcAddress(g_realDxgi, "DXGIGetDebugInterface");
    return fn ? fn(r, p) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI DXGIGetDebugInterface1(UINT f, REFIID r, void** p)
{
    LoadRealDxgi();
    auto fn = (PFN_CreateDXGIFactory2)GetProcAddress(g_realDxgi, "DXGIGetDebugInterface1");
    return fn ? fn(f, r, p) : E_NOTIMPL;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) { LoadRealDxgi(); ReadCoreWindow(); }
    return TRUE;
}
