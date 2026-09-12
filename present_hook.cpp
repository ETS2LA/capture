#include "present_hook.h"
#include "shared_memory.h"
#include <MinHook.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <atomic>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

typedef HRESULT(WINAPI* Present_t)(IDXGISwapChain*, UINT, UINT);
static Present_t oPresent = nullptr;

static SharedFrameWriter g_writer;
static bool g_writerReady = false;

static CRITICAL_SECTION g_stateLock;
static bool g_stateLockInitialized = false;

constexpr int kStagingCount = 3;
static ID3D11Texture2D* g_staging[kStagingCount] = {};
static bool g_stagingValid[kStagingCount] = {};
static int g_stagingWriteIdx = 0;
static UINT g_lastWidth = 0, g_lastHeight = 0;

static std::atomic<bool> g_hookInstalled{false};

static void Log(const char* msg) {
    OutputDebugStringA("[ets2la_capture] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}

static void LogThrottled(const char* msg, int everyN = 300) {
    static int counter = 0;
    if ((counter++ % everyN) == 0) Log(msg);
}

static void trackReadback(bool skipped) {
    static int skipCount = 0, total = 0;
    total++;
    if (skipped) skipCount++;
    if (total >= 120) {
        char buf[128];
        sprintf_s(buf, "[ets2la_capture] skipped %d/%d readbacks (GPU not ready in time)", skipCount, total);
        Log(buf);
        skipCount = 0;
        total = 0;
    }
}

static void ReleaseStagingTextures() {
    for (int i = 0; i < kStagingCount; ++i) {
        if (g_staging[i]) { g_staging[i]->Release(); g_staging[i] = nullptr; }
        g_stagingValid[i] = false;
    }
    g_lastWidth = g_lastHeight = 0;
}

static bool EnsureStagingTextures(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format) {
    if (width == g_lastWidth && height == g_lastHeight && g_staging[0]) return true;

    ReleaseStagingTextures();

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.BindFlags = 0;

    for (int i = 0; i < kStagingCount; ++i) {
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &g_staging[i]))) {
            LogThrottled("failed to create staging texture");
            ReleaseStagingTextures();
            return false;
        }
    }
    g_lastWidth = width;
    g_lastHeight = height;
    g_stagingWriteIdx = 0;
    return true;
}

static HRESULT WINAPI hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    ID3D11Device* device = nullptr;
    if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&device))) {
        ID3D11Texture2D* backBuffer = nullptr;
        if (SUCCEEDED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer))) {
            D3D11_TEXTURE2D_DESC desc;
            backBuffer->GetDesc(&desc);

            if (desc.Width > MAX_WIDTH || desc.Height > MAX_HEIGHT) {
                LogThrottled("game resolution exceeds MAX_WIDTH/MAX_HEIGHT - raise these in shared_frame.h and rebuild");
            } else {
                EnterCriticalSection(&g_stateLock);

                if (!g_writerReady) {
                    g_writerReady = g_writer.init();
                    if (!g_writerReady) Log("shared memory init failed");
                }

                if (g_writerReady && EnsureStagingTextures(device, desc.Width, desc.Height, desc.Format)) {
                    ID3D11DeviceContext* ctx = nullptr;
                    device->GetImmediateContext(&ctx);

                    bool published = false;
                    for (int back = 1; back < kStagingCount && !published; ++back) {
                        int idx = (g_stagingWriteIdx - back + kStagingCount) % kStagingCount;
                        if (!g_stagingValid[idx]) continue;

                        D3D11_MAPPED_SUBRESOURCE mapped;
                        HRESULT hr = ctx->Map(g_staging[idx], 0, D3D11_MAP_READ,
                                              D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                        if (hr == S_OK) {
                            g_writer.publish((const uint8_t*)mapped.pData, desc.Width, desc.Height, mapped.RowPitch);
                            ctx->Unmap(g_staging[idx], 0);
                            trackReadback(false);
                            published = true;
                        } else if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
                            break;
                        }
                    }
                    if (!published) trackReadback(true);

                    ctx->CopyResource(g_staging[g_stagingWriteIdx], backBuffer);
                    g_stagingValid[g_stagingWriteIdx] = true;
                    g_stagingWriteIdx = (g_stagingWriteIdx + 1) % kStagingCount;

                    ctx->Release();
                }

                LeaveCriticalSection(&g_stateLock);
            }
            backBuffer->Release();
        }
        device->Release();
    }
    return oPresent(pSwapChain, SyncInterval, Flags);
}

static void* GetPresentAddressViaDummyDevice() {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ets2la_capture_dummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"dummy",
        WS_OVERLAPPEDWINDOW,
        0, 0, 100, 100,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr
    );

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = 100;
    scd.BufferDesc.Height = 100;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;

    void* presentAddr = nullptr;

    for (int attempt = 0; attempt < 3 && !presentAddr; ++attempt) {
        if (attempt > 0) Sleep(500);

        D3D_FEATURE_LEVEL fl;
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        IDXGISwapChain* swapChain = nullptr;

        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &scd, &swapChain, &device, &fl, &ctx);

        if (SUCCEEDED(hr)) {
            void** vtable = *reinterpret_cast<void***>(swapChain);
            presentAddr = vtable[8];
        } else {
            Log("dummy D3D11CreateDeviceAndSwapChain failed, retrying");
        }

        if (swapChain) swapChain->Release();
        if (ctx) ctx->Release();
        if (device) device->Release();
    }

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return presentAddr;
}

bool StartPresentHook() {
    if (g_hookInstalled.exchange(true)) {
        return true;
    }

    if (!g_stateLockInitialized) {
        InitializeCriticalSection(&g_stateLock);
        g_stateLockInitialized = true;
    }

    void* presentAddr = GetPresentAddressViaDummyDevice();
    if (!presentAddr) {
        Log("could not obtain Present address - giving up");
        g_hookInstalled = false;
        return false;
    }

    if (MH_Initialize() != MH_OK) {
        Log("MH_Initialize failed");
        g_hookInstalled = false;
        return false;
    }
    if (MH_CreateHook(presentAddr, reinterpret_cast<void*>(&hkPresent), reinterpret_cast<void**>(&oPresent)) != MH_OK) {
        Log("MH_CreateHook failed");
        g_hookInstalled = false;
        return false;
    }
    if (MH_EnableHook(presentAddr) != MH_OK) {
        Log("MH_EnableHook failed");
        g_hookInstalled = false;
        return false;
    }
    Log("Present hook installed");
    return true;
}

void StopPresentHook() {
    if (!g_hookInstalled.exchange(false)) {
        return;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    if (g_stateLockInitialized) {
        EnterCriticalSection(&g_stateLock);
        ReleaseStagingTextures();
        LeaveCriticalSection(&g_stateLock);
    }
}