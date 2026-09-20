#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <MinHook.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "present_hook.h"
#include "scs_logging.h"
#include "shared_memory.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;
using namespace ets2la_capture;

static_assert(
    DXGI_FORMAT_B8G8R8A8_UNORM == 87 &&
    DXGI_FORMAT_R8G8B8A8_UNORM == 28 &&
    DXGI_FORMAT_R10G10B10A2_UNORM == 24 &&
    DXGI_FORMAT_R16G16B16A16_FLOAT == 10,
    "protocol.h format table must match DXGI_FORMAT"
);

typedef HRESULT(WINAPI* Present_t)(IDXGISwapChain*, UINT, UINT);
static Present_t oPresent = nullptr;
static void* g_presentTarget = nullptr;

static SharedFrameWriter g_writer;
static std::atomic<bool> g_writerReady{false};
static std::atomic<bool> g_hookInstalled{false};
static std::atomic<bool> g_stopping{false};
static std::atomic<int> g_inHook{0};
static std::atomic<bool> g_useShared{true};

static uint64_t Qpc() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return uint64_t(q.QuadPart);
}

static void LogOnce(std::atomic<bool>& flag, scs_log_type_t type, const char* msg) {
    if (!flag.exchange(true)) {
        scs_logging::write(type, msg);
    }
}

constexpr int kRing = 3;

struct SharedTex {
    ComPtr<ID3D11Texture2D> gameTex;
    ComPtr<IDXGIKeyedMutex> gameKm;
    HANDLE handle = nullptr;
    ComPtr<ID3D11Texture2D> workerTex;
    ComPtr<IDXGIKeyedMutex> workerKm;
};

struct PipelineC {
    ComPtr<ID3D11Device> gameDevice;
    ComPtr<IDXGIAdapter> adapter;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t bpp = 0;
    SharedTex tex[kRing];
    int next = 0;
    bool workerReady = false;
    ComPtr<ID3D11Texture2D> staging;
};

struct Job {
    std::shared_ptr<PipelineC> p;
    int idx = 0;
    uint64_t qpc = 0;
};

static std::shared_ptr<PipelineC> g_pipeC;
static std::vector<std::shared_ptr<PipelineC>> g_retired;

static std::mutex g_jobMutex;
static std::condition_variable g_jobCv;
static std::deque<Job> g_jobs;
static HANDLE g_workerThread = nullptr;

static ComPtr<ID3D11Device> g_wdev;
static ComPtr<ID3D11DeviceContext> g_wctx;
static LUID g_wluid = {};

static std::shared_ptr<PipelineC> CreatePipelineC(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, uint32_t bpp) {
    auto p = std::make_shared<PipelineC>();
    p->gameDevice = dev;
    p->width = w; p->height = h; p->format = fmt; p->bpp = bpp;

    ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) {
        return nullptr;
    }
    if (FAILED(dxgiDev->GetAdapter(&p->adapter))) {
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    for (int i = 0; i < kRing; ++i) {
        SharedTex& t = p->tex[i];
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &t.gameTex))) {
            return nullptr;
        }
        if (FAILED(t.gameTex.As(&t.gameKm))) {
            return nullptr;
        }
        ComPtr<IDXGIResource> res;
        if (FAILED(t.gameTex.As(&res))) {
            return nullptr;
        }
        if (FAILED(res->GetSharedHandle(&t.handle)) || !t.handle) {
            return nullptr;
        }
    }
    return p;
}

static bool EnsureWorkerSide(PipelineC& p) {
    if (p.workerReady) {
        return true;
    }

    DXGI_ADAPTER_DESC ad = {};
    if (FAILED(p.adapter->GetDesc(&ad))) {
        return false;
    }
    if (!g_wdev || ad.AdapterLuid.LowPart != g_wluid.LowPart || ad.AdapterLuid.HighPart != g_wluid.HighPart) {
        g_wctx.Reset();
        g_wdev.Reset();
        HRESULT hr = D3D11CreateDevice(
            p.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_wdev, nullptr, &g_wctx
        );
        if (FAILED(hr)) {
            scs_logging::writef(SCS_LOG_TYPE_warning, "worker D3D11CreateDevice failed (0x%08X)", unsigned(hr));
            return false;
        }
        g_wluid = ad.AdapterLuid;
    }

    for (int i = 0; i < kRing; ++i) {
        SharedTex& t = p.tex[i];
        HRESULT hr = g_wdev->OpenSharedResource(t.handle, IID_PPV_ARGS(&t.workerTex));
        if (FAILED(hr) || FAILED(t.workerTex.As(&t.workerKm))) {
            scs_logging::writef(SCS_LOG_TYPE_warning, "OpenSharedResource failed (0x%08X)", unsigned(hr));
            return false;
        }
    }

    D3D11_TEXTURE2D_DESC d = {};
    d.Width = p.width;
    d.Height = p.height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = p.format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    HRESULT hr = g_wdev->CreateTexture2D(&d, nullptr, &p.staging);
    if (FAILED(hr)) {
        scs_logging::writef(SCS_LOG_TYPE_warning, "worker staging texture failed (0x%08X)", unsigned(hr));
        return false;
    }
    p.workerReady = true;
    return true;
}

static bool WorkerAcquire(SharedTex& t) {
    while (true) {
        HRESULT hr = t.workerKm->AcquireSync(1, 250);
        if (hr == S_OK || hr == HRESULT(WAIT_ABANDONED)) {
            return true;
        }
        if (FAILED(hr) || g_stopping.load()) {
            return false;
        }
    }
}

static void WorkerFail(const char* why) {
    scs_logging::writef(SCS_LOG_TYPE_warning, "shared pipeline failed (%s), switching to staging fallback", why);
    g_useShared.store(false);
}

static void RecycleJob(const Job& job) {
    PipelineC& p = *job.p;
    if (!EnsureWorkerSide(p)) {
        WorkerFail("worker setup");
        return;
    }
    SharedTex& t = p.tex[job.idx];
    if (!WorkerAcquire(t)) return;
    t.workerKm->ReleaseSync(0);
    g_writer.note_drop();
}

static void ProcessJob(const Job& job) {
    PipelineC& p = *job.p;
    if (!EnsureWorkerSide(p)) {
        WorkerFail("worker setup");
        return;
    }
    SharedTex& t = p.tex[job.idx];
    if (!WorkerAcquire(t) && !g_stopping.load()) {
        WorkerFail("AcquireSync");
        return;
    }

    g_wctx->CopyResource(p.staging.Get(), t.workerTex.Get());
    t.workerKm->ReleaseSync(0);

    D3D11_MAPPED_SUBRESOURCE m = {};
    HRESULT hr = g_wctx->Map(p.staging.Get(), 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) {
        WorkerFail("Map");
        return;
    }
    if (g_writer.ensure_geometry(p.width, p.height, uint32_t(p.format), p.bpp)) {
        g_writer.publish(static_cast<const uint8_t*>(m.pData), m.RowPitch, job.qpc);
    }
    g_wctx->Unmap(p.staging.Get(), 0);
}

static DWORD WINAPI WorkerMain(LPVOID) {
    std::vector<Job> batch;
    while (!g_stopping.load()) {
        batch.clear();
        {
            std::unique_lock<std::mutex> lk(g_jobMutex);
            g_jobCv.wait_for(lk, std::chrono::milliseconds(100), [] {
                return !g_jobs.empty() || g_stopping.load();
            });
            while (!g_jobs.empty()) {
                batch.push_back(std::move(g_jobs.front()));
                g_jobs.pop_front();
            }
        }
        if (batch.empty()) continue;
        try {
            for (size_t i = 0; i + 1 < batch.size(); ++i) RecycleJob(batch[i]);
            ProcessJob(batch.back());
        } catch (...) {}
    }
    batch.clear();
    g_wctx.Reset();
    g_wdev.Reset();
    return 0;
}

static void CaptureShared(
    ID3D11Device* dev,
    ID3D11DeviceContext* ctx,
    ID3D11Texture2D* bb,
    const D3D11_TEXTURE2D_DESC& d,
    uint32_t bpp,
    uint64_t presentQpc,
    bool& handled
) {
    handled = true;
    if (!g_pipeC || g_pipeC->gameDevice.Get() != dev || g_pipeC->width != d.Width ||
        g_pipeC->height != d.Height || g_pipeC->format != d.Format) {
        if (g_pipeC) g_retired.push_back(std::move(g_pipeC));
        g_pipeC = CreatePipelineC(dev, d.Width, d.Height, d.Format, bpp);
        if (!g_pipeC) {
            scs_logging::write(SCS_LOG_TYPE_warning, "could not create shared textures, using staging fallback");
            g_useShared.store(false);
            handled = false;
            return;
        }
    }

    PipelineC& p = *g_pipeC;
    for (int k = 0; k < kRing; ++k) {
        const int idx = (p.next + k) % kRing;
        SharedTex& t = p.tex[idx];
        if (t.gameKm->AcquireSync(0, 0) != S_OK) continue;
        ctx->CopyResource(t.gameTex.Get(), bb);
        t.gameKm->ReleaseSync(1);
        p.next = (idx + 1) % kRing;
        {
            std::lock_guard<std::mutex> lk(g_jobMutex);
            g_jobs.push_back(Job{g_pipeC, idx, presentQpc});
        }
        g_jobCv.notify_one();
        return;
    }
    g_writer.note_drop();
}

constexpr int kStageCount = 3;

struct PipelineB {
    ComPtr<ID3D11Device> dev;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t bpp = 0;
    ComPtr<ID3D11Texture2D> staging[kStageCount];
    bool valid[kStageCount] = {};
    uint64_t qpc[kStageCount] = {};
    int writeIdx = 0;
};
static std::unique_ptr<PipelineB> g_pipeB;

static void CaptureStaging(
    ID3D11Device* dev,
    ID3D11DeviceContext* ctx,
    ID3D11Texture2D* bb,
    const D3D11_TEXTURE2D_DESC& d,
    uint32_t bpp,
    uint64_t presentQpc
) {
    if (!g_pipeB || g_pipeB->dev.Get() != dev || g_pipeB->width != d.Width ||
        g_pipeB->height != d.Height || g_pipeB->format != d.Format) {
        g_pipeB = std::make_unique<PipelineB>();
        g_pipeB->dev = dev;
        g_pipeB->width = d.Width; g_pipeB->height = d.Height;
        g_pipeB->format = d.Format; g_pipeB->bpp = bpp;
        D3D11_TEXTURE2D_DESC sd = {};
        sd.Width = d.Width; sd.Height = d.Height;
        sd.MipLevels = 1; sd.ArraySize = 1;
        sd.Format = d.Format;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (int i = 0; i < kStageCount; ++i) {
            if (FAILED(dev->CreateTexture2D(&sd, nullptr, &g_pipeB->staging[i]))) {
                static std::atomic<bool> once{false};
                LogOnce(once, SCS_LOG_TYPE_error, "failed to create staging textures");
                g_pipeB.reset();
                return;
            }
        }
    }
    PipelineB& p = *g_pipeB;

    for (int back = 1; back < kStageCount; ++back) {
        const int idx = (p.writeIdx - back + kStageCount) % kStageCount;
        if (!p.valid[idx]) continue;
        D3D11_MAPPED_SUBRESOURCE m;
        HRESULT hr = ctx->Map(p.staging[idx].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == S_OK) {
            if (g_writer.ensure_geometry(p.width, p.height, uint32_t(p.format), p.bpp))
                g_writer.publish(static_cast<const uint8_t*>(m.pData), m.RowPitch, p.qpc[idx]);
            ctx->Unmap(p.staging[idx].Get(), 0);
            for (int j = 0; j < kStageCount; ++j) p.valid[j] = false;
            break;
        }
        if (hr != DXGI_ERROR_WAS_STILL_DRAWING) break;
    }

    ctx->CopyResource(p.staging[p.writeIdx].Get(), bb);
    p.valid[p.writeIdx] = true;
    p.qpc[p.writeIdx] = presentQpc;
    p.writeIdx = (p.writeIdx + 1) % kStageCount;
}

static IDXGISwapChain* g_primarySC = nullptr;
static UINT g_primaryArea = 0;
static ULONGLONG g_primaryLastSeen = 0;

static bool IsCaptureTarget(IDXGISwapChain* sc, ULONGLONG nowMs) {
    DXGI_SWAP_CHAIN_DESC d;
    if (FAILED(sc->GetDesc(&d))) {
        return false;
    }
    if (d.BufferDesc.Width < 320 || d.BufferDesc.Height < 200) {
        return false;
    }
    if (d.OutputWindow) {
        DWORD pid = 0;
        GetWindowThreadProcessId(d.OutputWindow, &pid);
        if (pid != GetCurrentProcessId()) {
            return false;
        }
    }
    const UINT area = d.BufferDesc.Width * d.BufferDesc.Height;
    if (sc == g_primarySC) {
        g_primaryArea = area;
        g_primaryLastSeen = nowMs;
        return true;
    }
    if (!g_primarySC || nowMs - g_primaryLastSeen > 1000 || area > g_primaryArea) {
        g_primarySC = sc; g_primaryArea = area; g_primaryLastSeen = nowMs;
        return true;
    }
    return false;
}

static void CaptureFrame(IDXGISwapChain* sc) {
    if (!g_writerReady.load(std::memory_order_acquire)) return;
    const uint64_t presentQpc = Qpc();
    const ULONGLONG nowMs = GetTickCount64();

    if (!g_retired.empty()) {
        for (size_t i = g_retired.size(); i-- > 0;)
            if (g_retired[i].use_count() == 1) g_retired.erase(g_retired.begin() + i);
    }

    if (!IsCaptureTarget(sc, nowMs)) return;

    const bool active = g_writer.has_active_readers();
    g_writer.set_capturing(active);
    if (!active) return;

    ComPtr<ID3D11Device> dev;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev)))) return;
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return;
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);

    static std::atomic<bool> warnedFormat{false}, warnedMsaa{false}, warnedSize{false};
    const uint32_t bpp = bytes_per_pixel_for(uint32_t(d.Format));
    if (bpp == 0) {
        LogOnce(warnedFormat, SCS_LOG_TYPE_warning, "back buffer format not supported by ets2la_capture");
        return;
    }
    if (d.SampleDesc.Count > 1) {
        LogOnce(warnedMsaa, SCS_LOG_TYPE_warning, "multisampled back buffer not supported");
        return;
    }
    if (d.Width > 16384 || d.Height > 16384) {
        LogOnce(warnedSize, SCS_LOG_TYPE_warning, "back buffer too large");
        return;
    }

    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);

    if (g_useShared.load()) {
        bool handled = true;
        CaptureShared(dev.Get(), ctx.Get(), bb.Get(), d, bpp, presentQpc, handled);
        if (handled) return;
    }
    if (g_pipeC) {
        g_retired.push_back(std::move(g_pipeC));
        g_pipeC = nullptr;
    }
    CaptureStaging(dev.Get(), ctx.Get(), bb.Get(), d, bpp, presentQpc);
}

static HRESULT WINAPI hkPresent(IDXGISwapChain* sc, UINT syncInterval, UINT flags) {
    if (!g_stopping.load(std::memory_order_relaxed) && !(flags & DXGI_PRESENT_TEST)) {
        g_inHook.fetch_add(1);
        try {
            CaptureFrame(sc);
        } catch (...) {
        }
        g_inHook.fetch_sub(1);
    }
    return oPresent(sc, syncInterval, flags);
}

static void* GetPresentAddressViaDummyDevice() {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ets2la_capture_dummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"dummy", WS_OVERLAPPEDWINDOW,
        0, 0, 100, 100,
        nullptr, nullptr, wc.hInstance, nullptr
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
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &scd, &swapChain, &device, &fl, &ctx
        );
        if (SUCCEEDED(hr)) {
            void** vtable = *reinterpret_cast<void***>(swapChain);
            presentAddr = vtable[8];
        } else {
            scs_logging::write(SCS_LOG_TYPE_warning, "dummy D3D11CreateDeviceAndSwapChain failed, retrying");
        }
        if (swapChain) {
            swapChain->Release();
        }
        if (ctx) {
            ctx->Release();
        }
        if (device) {
            device->Release();
        }
    }

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return presentAddr;
}

static void StopWorker(bool join) {
    g_jobCv.notify_all();
    if (g_workerThread) {
        if (join) {
            WaitForSingleObject(g_workerThread, 3000);
            CloseHandle(g_workerThread);
        }
        g_workerThread = nullptr;
    }
}

bool StartPresentHook() {
    if (g_hookInstalled.exchange(true)) {
        return true;
    }
    g_stopping.store(false);

    auto giveUp = [](const char* msg) {
        scs_logging::write(SCS_LOG_TYPE_error, msg);
        g_stopping.store(true);
        StopWorker(true);
        g_hookInstalled.store(false);
        return false;
    };

    if (!g_writerReady.load()) {
        if (!g_writer.init()) {
            scs_logging::writef(SCS_LOG_TYPE_error, "shared memory init failed: %s", g_writer.last_error().c_str());
            g_hookInstalled.store(false);
            return false;
        }
        g_writerReady.store(true, std::memory_order_release);
    }

    g_workerThread = CreateThread(nullptr, 0, WorkerMain, nullptr, 0, nullptr);
    if (!g_workerThread) return giveUp("could not start capture worker thread");

    g_presentTarget = GetPresentAddressViaDummyDevice();
    if (!g_presentTarget) return giveUp("could not obtain Present address, giving up");

    if (MH_Initialize() != MH_OK) return giveUp("MH_Initialize failed");
    if (MH_CreateHook(g_presentTarget, reinterpret_cast<void*>(&hkPresent), reinterpret_cast<void**>(&oPresent)) != MH_OK)
        return giveUp("MH_CreateHook failed");
    if (MH_EnableHook(g_presentTarget) != MH_OK) return giveUp("MH_EnableHook failed");

    scs_logging::write(SCS_LOG_TYPE_message, "Present hook installed");
    return true;
}

void StopPresentHook(bool process_detach) {
    if (!g_hookInstalled.exchange(false)) return;
    g_stopping.store(true);
    MH_DisableHook(MH_ALL_HOOKS);

    if (process_detach) return;

    for (int i = 0; i < 500 && g_inHook.load() > 0; ++i) {
        Sleep(1);
    }
    MH_Uninitialize();
    StopWorker(true);
    g_pipeC.reset();
    g_retired.clear();
    g_pipeB.reset();
    g_writer.shutdown();
    g_writerReady.store(false);
}