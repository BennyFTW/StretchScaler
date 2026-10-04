// Scaler: Windows.Graphics.Capture (window) -> crop client area on GPU -> stretch -> flip-model overlay.
#include <Unknwn.h>
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <atomic>
#include <thread>
#include <vector>
#include <algorithm>

#include "Scaler.h"
#include "Enum.h"
#include "Log.h"
#include "FpsCounter.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using winrt::com_ptr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {

const char* kShaderSrc = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 PSOpaque(VSOut i) : SV_Target { return float4(tex.Sample(smp, i.uv).rgb, 1); }
float4 PSAlpha(VSOut i) : SV_Target { return tex.Sample(smp, i.uv); }
)";

const wchar_t* kOverlayClass = L"StretchScalerOverlay";

LRESULT CALLBACK OverlayProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

com_ptr<ID3DBlob> Compile(const char* entry, const char* target) {
    com_ptr<ID3DBlob> code, err;
    HRESULT hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "StretchScaler", nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), err.put());
    if (FAILED(hr)) {
        Log(L"Shader compile failed (%S): 0x%08X %S", entry, hr, err ? (const char*)err->GetBufferPointer() : "");
        return nullptr;
    }
    return code;
}

int64_t Qpc100ns() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    return (int64_t)((double)now.QuadPart * 10000000.0 / (double)freq.QuadPart);
}

struct CursorTex {
    HCURSOR handle = nullptr;
    com_ptr<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0, hotX = 0, hotY = 0;
};

// Converts an HCURSOR into a straight-alpha BGRA texture.
bool BuildCursorTexture(ID3D11Device* dev, HCURSOR hc, CursorTex& out) {
    ICONINFO ii{};
    if (!GetIconInfo(hc, &ii)) return false;
    struct Cleanup { ICONINFO& i; ~Cleanup() { if (i.hbmColor) DeleteObject(i.hbmColor); if (i.hbmMask) DeleteObject(i.hbmMask); } } cleanup{ii};

    auto readBits = [](HBITMAP bmp, int w, int h, std::vector<uint32_t>& px) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        px.assign((size_t)w * h, 0);
        HDC dc = GetDC(nullptr);
        int r = GetDIBits(dc, bmp, 0, h, px.data(), &bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, dc);
        return r == h;
    };

    BITMAP bm{};
    std::vector<uint32_t> pixels;
    int w = 0, h = 0;
    if (ii.hbmColor) {
        GetObjectW(ii.hbmColor, sizeof(bm), &bm);
        w = bm.bmWidth; h = bm.bmHeight;
        if (!readBits(ii.hbmColor, w, h, pixels)) return false;
        bool hasAlpha = std::any_of(pixels.begin(), pixels.end(), [](uint32_t p) { return (p >> 24) != 0; });
        if (!hasAlpha) {
            std::vector<uint32_t> mask;
            if (ii.hbmMask && readBits(ii.hbmMask, w, h, mask)) {
                for (size_t i = 0; i < pixels.size(); ++i)
                    pixels[i] = (pixels[i] & 0x00FFFFFF) | ((mask[i] & 0x00FFFFFF) ? 0u : 0xFF000000u);
            } else {
                for (auto& p : pixels) p |= 0xFF000000u;
            }
        }
    } else {
        // Monochrome cursor: mask is AND (top half) + XOR (bottom half).
        GetObjectW(ii.hbmMask, sizeof(bm), &bm);
        w = bm.bmWidth; h = bm.bmHeight / 2;
        std::vector<uint32_t> mask;
        if (!readBits(ii.hbmMask, w, h * 2, mask)) return false;
        pixels.assign((size_t)w * h, 0);
        for (int i = 0; i < w * h; ++i) {
            bool a = (mask[i] & 0xFFFFFF) != 0;
            bool x = (mask[(size_t)i + (size_t)w * h] & 0xFFFFFF) != 0;
            if (!a) pixels[i] = x ? 0xFFFFFFFFu : 0xFF000000u;
            else    pixels[i] = x ? 0xFFFFFFFFu : 0u; // "invert" pixels approximated as white
        }
    }
    if (w <= 0 || h <= 0) return false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{pixels.data(), (UINT)w * 4, 0};
    com_ptr<ID3D11Texture2D> tex;
    if (FAILED(dev->CreateTexture2D(&td, &sd, tex.put()))) return false;
    out.srv = nullptr;
    if (FAILED(dev->CreateShaderResourceView(tex.get(), nullptr, out.srv.put()))) return false;
    out.handle = hc; out.w = w; out.h = h; out.hotX = (int)ii.xHotspot; out.hotY = (int)ii.yHotspot;
    return true;
}

} // namespace

struct Scaler::Impl {
    HWND src = nullptr, overlay = nullptr, notify = nullptr;
    UINT stoppedMsg = 0;
    RECT mon{};
    int outW = 0, outH = 0;
    ScalerOptions opt;

    com_ptr<ID3D11Device> dev;
    com_ptr<ID3D11DeviceContext> ctx;
    com_ptr<IDXGISwapChain2> sc;
    HANDLE waitable = nullptr;
    bool tearing = false;
    com_ptr<ID3D11RenderTargetView> rtv;
    com_ptr<ID3D11VertexShader> vs;
    com_ptr<ID3D11PixelShader> psOpaque, psAlpha;
    com_ptr<ID3D11SamplerState> sampler;
    com_ptr<ID3D11BlendState> blend;

    com_ptr<ID3D11Texture2D> srcTex;
    com_ptr<ID3D11ShaderResourceView> srcSrv;
    UINT srcW = 0, srcH = 0;
    RECT lastClient{};          // source client rect on screen at last copy
    CursorTex cursor;
    POINT lastCursorPos{-1, -1};
    HCURSOR lastCursorDrawn = nullptr;
    bool lastCursorVisible = false;

    wd3d::IDirect3DDevice rtDevice{nullptr};
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::Windows::Graphics::SizeInt32 poolSize{};
    winrt::event_token frameToken{}, closedToken{};

    HANDLE frameEvent = nullptr, stopEvent = nullptr;
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> visible{false};
    std::atomic<bool> stopPosted{false};
    std::atomic<uint32_t> nCaptured{0}, nPresented{0}, nLatency{0};
    std::atomic<int64_t> latencySum{0};
    std::atomic<int> presentMode{-1};
    uint32_t totalPresents = 0;
    com_ptr<IDXGISwapChainMedia> media;
    bool loggedFirstFrame = false;

    // FPS overlay (Direct2D/DirectWrite drawn straight into the swap chain back buffer)
    com_ptr<ID2D1Factory1> d2dFactory;
    com_ptr<ID2D1DeviceContext> d2dCtx;
    com_ptr<ID2D1Bitmap1> d2dTarget;
    com_ptr<IDWriteTextFormat> textFormat;
    com_ptr<ID2D1SolidColorBrush> textBrush, bgBrush;
    GameFpsCounter gameFps;
    uint32_t shownFrames = 0;
    int64_t fpsWindowStart = 0;
    std::wstring fpsText = L"-- FPS";

    bool CreateTextOverlay();
    void UpdateFpsText();
    void DrawFps();

    bool CreateDevice(HMONITOR hmon);
    bool CreatePipeline();
    bool CreateOverlay();
    bool CreateSwapChain();
    bool StartCapture();
    void RenderThread();
    bool CopyFrame(const wgc::Direct3D11CaptureFrame& frame);
    void Render(int64_t frameTime100ns);
    void PostStopped(const wchar_t* why);
    void Release();
};

bool EnablePrivilege(const wchar_t* name) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
    TOKEN_PRIVILEGES tp{1};
    bool ok = LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid);
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    ok = ok && AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

bool Scaler::Impl::CreateDevice(HMONITOR hmon) {
    // Prefer the adapter driving the output monitor so presentation avoids a cross-adapter copy.
    com_ptr<IDXGIFactory1> factory;
    com_ptr<IDXGIAdapter1> chosen;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) {
        com_ptr<IDXGIAdapter1> a;
        for (UINT i = 0; !chosen && factory->EnumAdapters1(i, a.put()) != DXGI_ERROR_NOT_FOUND; ++i) {
            com_ptr<IDXGIOutput> o;
            for (UINT j = 0; a->EnumOutputs(j, o.put()) != DXGI_ERROR_NOT_FOUND; ++j) {
                DXGI_OUTPUT_DESC od{};
                o->GetDesc(&od);
                o = nullptr;
                if (od.Monitor == hmon) { chosen = a; break; }
            }
            a = nullptr;
        }
    }
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    HRESULT hr = D3D11CreateDevice(chosen.get(), chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   dev.put(), nullptr, ctx.put());
    if (FAILED(hr)) { Log(L"D3D11CreateDevice failed: 0x%08X", hr); return false; }

    com_ptr<IDXGIDevice> dxgiDev = dev.as<IDXGIDevice>();
    com_ptr<IDXGIAdapter> adapter;
    dxgiDev->GetAdapter(adapter.put());
    DXGI_ADAPTER_DESC ad{};
    adapter->GetDesc(&ad);
    Log(L"D3D11 device on adapter: %s%s", ad.Description, chosen ? L"" : L" (default adapter; output monitor's adapter not found)");

    // WGC uses the device from its own threads.
    if (auto mt = dev.try_as<ID3D11Multithread>()) mt->SetMultithreadProtected(TRUE);
    // Let our tiny copy+stretch jump ahead of the game's GPU work so it isn't stuck behind a full game frame.
    EnablePrivilege(SE_INC_BASE_PRIORITY_NAME);
    HRESULT gp = dxgiDev->SetGPUThreadPriority(7);
    if (FAILED(gp)) Log(L"GPU priority boost not permitted (0x%08X) - run as admin for it.", gp);
    else Log(L"GPU priority boost enabled.");
    if (auto d1 = dxgiDev.try_as<IDXGIDevice1>()) d1->SetMaximumFrameLatency(1);

    com_ptr<::IInspectable> insp;
    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDev.get(), insp.put());
    if (FAILED(hr)) { Log(L"CreateDirect3D11DeviceFromDXGIDevice failed: 0x%08X", hr); return false; }
    rtDevice = insp.as<wd3d::IDirect3DDevice>();
    return true;
}

bool Scaler::Impl::CreatePipeline() {
    auto vsb = Compile("VSMain", "vs_5_0");
    auto ps1 = Compile("PSOpaque", "ps_5_0");
    auto ps2 = Compile("PSAlpha", "ps_5_0");
    if (!vsb || !ps1 || !ps2) return false;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, vs.put());
    dev->CreatePixelShader(ps1->GetBufferPointer(), ps1->GetBufferSize(), nullptr, psOpaque.put());
    dev->CreatePixelShader(ps2->GetBufferPointer(), ps2->GetBufferSize(), nullptr, psAlpha.put());

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = opt.pointFilter ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    dev->CreateSamplerState(&sd, sampler.put());

    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_ZERO; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bd, blend.put());
    return vs && psOpaque && psAlpha && sampler && blend;
}

bool Scaler::Impl::CreateOverlay() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = OverlayProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kOverlayClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    // Topmost, never activated, click-through (LAYERED+TRANSPARENT), not in Alt+Tab (TOOLWINDOW),
    // no GDI redirection surface (content comes only from the flip-model swap chain).
    // Measured: with these styles the output still lands on a hardware overlay plane (no DWM composition).
    overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                                  WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
                              kOverlayClass, L"StretchScaler Output", WS_POPUP,
                              mon.left, mon.top, outW, outH, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!overlay) { Log(L"CreateWindowEx(overlay) failed: %lu", GetLastError()); return false; }
    SetLayeredWindowAttributes(overlay, 0, 255, LWA_ALPHA); // fully opaque, so it stays eligible for direct flip
    return true;
}

bool Scaler::Impl::CreateSwapChain() {
    com_ptr<IDXGIDevice> dxgiDev = dev.as<IDXGIDevice>();
    com_ptr<IDXGIAdapter> adapter;
    dxgiDev->GetAdapter(adapter.put());
    com_ptr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(factory.put()));

    BOOL allowTearing = FALSE;
    if (auto f5 = factory.try_as<IDXGIFactory5>())
        f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
    tearing = allowTearing != FALSE;

    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = outW; d.Height = outH;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_NONE;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);

    com_ptr<IDXGISwapChain1> sc1;
    HRESULT hr = factory->CreateSwapChainForHwnd(dev.get(), overlay, &d, nullptr, nullptr, sc1.put());
    if (FAILED(hr)) { Log(L"CreateSwapChainForHwnd failed: 0x%08X", hr); return false; }
    factory->MakeWindowAssociation(overlay, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    sc = sc1.as<IDXGISwapChain2>();
    sc->SetMaximumFrameLatency(1);
    waitable = sc->GetFrameLatencyWaitableObject();
    media = sc.try_as<IDXGISwapChainMedia>();

    com_ptr<ID3D11Texture2D> bb;
    sc->GetBuffer(0, IID_PPV_ARGS(bb.put()));
    hr = dev->CreateRenderTargetView(bb.get(), nullptr, rtv.put());
    if (FAILED(hr)) { Log(L"CreateRenderTargetView failed: 0x%08X", hr); return false; }
    Log(L"Swap chain: %dx%d FLIP_DISCARD, 2 buffers, max frame latency 1, waitable, tearing %s, present mode %s",
        outW, outH, tearing ? L"supported" : L"not supported",
        opt.vsync ? L"VSync (Present 1)" : L"no VSync (Present 0)");
    return true;
}

bool Scaler::Impl::CreateTextOverlay() {
    D2D1_FACTORY_OPTIONS fo{};
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &fo, d2dFactory.put_void());
    com_ptr<ID2D1Device> d2dDev;
    if (SUCCEEDED(hr)) hr = d2dFactory->CreateDevice(dev.as<IDXGIDevice>().get(), d2dDev.put());
    if (SUCCEEDED(hr)) hr = d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2dCtx.put());
    com_ptr<IDXGISurface> surf;
    if (SUCCEEDED(hr)) hr = sc->GetBuffer(0, IID_PPV_ARGS(surf.put()));
    if (SUCCEEDED(hr)) {
        D2D1_BITMAP_PROPERTIES1 bp{};
        bp.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE};
        bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        hr = d2dCtx->CreateBitmapFromDxgiSurface(surf.get(), &bp, d2dTarget.put());
    }
    com_ptr<IDWriteFactory> dw;
    if (SUCCEEDED(hr)) hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.put()));
    if (SUCCEEDED(hr)) hr = dw->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                                 DWRITE_FONT_STRETCH_NORMAL, 22.0f * outH / 1440.0f, L"en-us", textFormat.put());
    if (SUCCEEDED(hr)) {
        textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        d2dCtx->SetTarget(d2dTarget.get());
        d2dCtx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        d2dCtx->CreateSolidColorBrush(D2D1::ColorF(0.30f, 1.0f, 0.30f), textBrush.put());
        d2dCtx->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.55f), bgBrush.put());
    }
    if (FAILED(hr) || !textBrush || !bgBrush) {
        Log(L"FPS overlay: Direct2D setup failed (0x%08X) - overlay disabled.", hr);
        d2dCtx = nullptr; d2dTarget = nullptr; textFormat = nullptr;
        return false;
    }
    if (opt.gamePid) gameFps.Start(opt.gamePid);
    return true;
}

void Scaler::Impl::UpdateFpsText() {
    // Refresh the numbers twice a second so they're readable.
    int64_t now = Qpc100ns();
    if (fpsWindowStart == 0) { fpsWindowStart = now; return; }
    int64_t dt = now - fpsWindowStart;
    if (dt < 5000000) return;
    double shown = shownFrames * 1e7 / (double)dt;
    shownFrames = 0;
    fpsWindowStart = now;
    wchar_t buf[128];
    if (gameFps.Active()) {
        double ms = 0;
        double fps = gameFps.Fps(&ms);
        swprintf_s(buf, L"%.0f FPS  %.1f ms  | shown %.0f", fps, ms, shown);
    } else {
        swprintf_s(buf, L"%.0f FPS (on-screen)", shown);
    }
    fpsText = buf;
}

void Scaler::Impl::DrawFps() {
    if (!d2dCtx) return;
    UpdateFpsText();
    float pad = 10.0f * outH / 1440.0f;
    float h = 32.0f * outH / 1440.0f;
    float w = (float)fpsText.size() * 12.5f * outH / 1440.0f + 2 * pad;
    float x = (opt.fpsCorner & 1) ? outW - w - pad : pad;
    float y = (opt.fpsCorner & 2) ? outH - h - pad : pad;
    d2dCtx->BeginDraw();
    d2dCtx->FillRectangle(D2D1::RectF(x, y, x + w, y + h), bgBrush.get());
    d2dCtx->DrawTextW(fpsText.c_str(), (UINT32)fpsText.size(), textFormat.get(),
                      D2D1::RectF(x + pad, y + (h - 22.0f * outH / 1440.0f) * 0.5f - 2, x + w, y + h), textBrush.get());
    HRESULT hr = d2dCtx->EndDraw();
    if (FAILED(hr)) { Log(L"FPS overlay draw failed (0x%08X) - disabled.", hr); d2dCtx = nullptr; }
}

bool Scaler::Impl::StartCapture() {
    try {
        if (!wgc::GraphicsCaptureSession::IsSupported()) {
            Log(L"Windows.Graphics.Capture is not supported on this system.");
            return false;
        }
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        HRESULT hr = interop->CreateForWindow(src, winrt::guid_of<wgc::IGraphicsCaptureItem>(), winrt::put_abi(item));
        if (FAILED(hr)) { Log(L"IGraphicsCaptureItemInterop::CreateForWindow failed: 0x%08X", hr); return false; }

        poolSize = item.Size();
        Log(L"Capture item: \"%s\", size %dx%d", item.DisplayName().c_str(), poolSize.Width, poolSize.Height);
        pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            rtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, poolSize);
        session = pool.CreateCaptureSession(item);

        // The real cursor is either drawn by us (scaled) or left as the hardware cursor; never baked into the capture.
        try { session.IsCursorCaptureEnabled(false); } catch (...) { Log(L"IsCursorCaptureEnabled not available."); }
        if (opt.hideBorder) {
            try { session.IsBorderRequired(false); }
            catch (winrt::hresult_error const& e) { Log(L"Could not disable capture border: 0x%08X %s", (unsigned)e.code(), e.message().c_str()); }
        }

        HANDLE ev = frameEvent;
        Impl* self = this;
        frameToken = pool.FrameArrived([ev, self](auto&&, auto&&) {
            self->nCaptured++;
            SetEvent(ev);
        });
        closedToken = item.Closed([self](auto&&, auto&&) { self->PostStopped(L"Source window closed (capture item closed)."); });

        session.StartCapture();
        Log(L"Capture started.");
        return true;
    } catch (winrt::hresult_error const& e) {
        Log(L"Capture setup failed: 0x%08X %s", (unsigned)e.code(), e.message().c_str());
        return false;
    }
}

void Scaler::Impl::PostStopped(const wchar_t* why) {
    Log(L"%s", why);
    if (!stopPosted.exchange(true) && notify) PostMessageW(notify, stoppedMsg, 0, 0);
}

bool Scaler::Impl::CopyFrame(const wgc::Direct3D11CaptureFrame& frame) {
    auto cs = frame.ContentSize();
    if (cs.Width != poolSize.Width || cs.Height != poolSize.Height) {
        Log(L"Source window size changed: %dx%d -> %dx%d (recreating frame pool)", poolSize.Width, poolSize.Height, cs.Width, cs.Height);
        poolSize = cs;
        pool.Recreate(rtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, cs);
    }

    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> tex;
    if (FAILED(access->GetInterface(IID_PPV_ARGS(tex.put())))) return false;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);

    // WGC captures the window's visible frame (DWMWA_EXTENDED_FRAME_BOUNDS). Crop to the client area.
    RECT fb{};
    if (FAILED(DwmGetWindowAttribute(src, DWMWA_EXTENDED_FRAME_BOUNDS, &fb, sizeof(fb)))) GetWindowRect(src, &fb);
    RECT cl = ClientRectOnScreen(src);
    lastClient = cl;
    int ox = (std::max)(0L, cl.left - fb.left);
    int oy = (std::max)(0L, cl.top - fb.top);
    int maxW = (std::min)((int)td.Width, cs.Width), maxH = (std::min)((int)td.Height, cs.Height);
    int cw = (std::min)((int)(cl.right - cl.left), maxW - ox);
    int ch = (std::min)((int)(cl.bottom - cl.top), maxH - oy);
    if (cw <= 0 || ch <= 0) return false;

    if (!srcTex || srcW != (UINT)cw || srcH != (UINT)ch) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = cw; d.Height = ch; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        srcTex = nullptr; srcSrv = nullptr;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, srcTex.put())) ||
            FAILED(dev->CreateShaderResourceView(srcTex.get(), nullptr, srcSrv.put()))) {
            Log(L"Failed to create source texture %dx%d", cw, ch);
            srcTex = nullptr; return false;
        }
        srcW = cw; srcH = ch;
        Log(L"Source client area: %dx%d (crop offset %d,%d in %dx%d capture) -> output %dx%d (scale x%.4f, y%.4f)",
            cw, ch, ox, oy, cs.Width, cs.Height, outW, outH, (double)outW / cw, (double)outH / ch);
        if (cw != 1920 || ch != 1440)
            Log(L"Note: source client is %dx%d, not 1920x1440. Check H1Z1 WindowedWidth/Height and Windows DPI scaling.", cw, ch);
    }

    D3D11_BOX box{(UINT)ox, (UINT)oy, 0, (UINT)(ox + cw), (UINT)(oy + ch), 1};
    ctx->CopySubresourceRegion(srcTex.get(), 0, 0, 0, 0, tex.get(), 0, &box);
    shownFrames++;
    if (!loggedFirstFrame) { loggedFirstFrame = true; Log(L"First frame captured."); }
    return true;
}

void Scaler::Impl::Render(int64_t frameTime100ns) {
    if (!srcSrv) return;

    // Destination rectangle of the game image inside the overlay.
    float vx = 0, vy = 0, vw = (float)outW, vh = (float)outH;
    if (!opt.stretch) {
        float s = (std::min)((float)outW / srcW, (float)outH / srcH);
        vw = srcW * s; vh = srcH * s;
        vx = (outW - vw) * 0.5f; vy = (outH - vh) * 0.5f;
    }

    if (d2dCtx) ctx->ClearState(); // Direct2D changes pipeline state on the shared context
    ID3D11RenderTargetView* rt = rtv.get();
    ctx->OMSetRenderTargets(1, &rt, nullptr);
    if (!opt.stretch) {
        const float black[4] = {0, 0, 0, 1};
        ctx->ClearRenderTargetView(rt, black);
    }
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs.get(), nullptr, 0);
    ID3D11SamplerState* s = sampler.get();
    ctx->PSSetSamplers(0, 1, &s);

    D3D11_VIEWPORT vp{vx, vy, vw, vh, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->PSSetShader(psOpaque.get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = srcSrv.get();
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->Draw(3, 0);

    // Cursor: the real one is hidden by the UI while the game is focused; draw it at the stretched position.
    lastCursorVisible = false;
    if (opt.drawCursor) {
        CURSORINFO ci{sizeof(ci)};
        if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor) {
            RECT cl = lastClient;
            POINT p = ci.ptScreenPos;
            lastCursorPos = p;
            lastCursorDrawn = ci.hCursor;
            if (p.x >= cl.left && p.x < cl.right && p.y >= cl.top && p.y < cl.bottom) {
                if (cursor.handle != ci.hCursor && !BuildCursorTexture(dev.get(), ci.hCursor, cursor)) cursor = {};
                if (cursor.srv) {
                    float sx = vw / srcW, sy = vh / srcH;
                    D3D11_VIEWPORT cvp{vx + (p.x - cl.left - cursor.hotX) * sx, vy + (p.y - cl.top - cursor.hotY) * sy,
                                       cursor.w * sx, cursor.h * sy, 0, 1};
                    ctx->RSSetViewports(1, &cvp);
                    ctx->OMSetBlendState(blend.get(), nullptr, 0xFFFFFFFF);
                    ctx->PSSetShader(psAlpha.get(), nullptr, 0);
                    ID3D11ShaderResourceView* csrv = cursor.srv.get();
                    ctx->PSSetShaderResources(0, 1, &csrv);
                    ctx->Draw(3, 0);
                    lastCursorVisible = true;
                }
            }
        }
    }

    DrawFps();

    UINT interval = opt.vsync ? 1 : 0;
    UINT flags = (!opt.vsync && tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr = sc->Present(interval, flags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        PostStopped(L"GPU device removed/reset - scaling stopped.");
        return;
    }
    if (FAILED(hr)) Log(L"Present failed: 0x%08X", hr);
    // About once a second, ask DXGI how our output reached the screen: composed by DWM (one extra refresh)
    // or on a hardware overlay plane / direct flip (no extra composition).
    if (media && (++totalPresents % 60) == 0) {
        DXGI_FRAME_STATISTICS_MEDIA fs{};
        if (SUCCEEDED(media->GetFrameStatisticsMedia(&fs))) presentMode = (int)fs.CompositionMode;
    }
    nPresented++;
    if (frameTime100ns > 0) {
        // SystemRelativeTime is the DWM display time scheduled for the source frame; positive lead means we
        // presented the stretched copy before that slot.
        int64_t lead = frameTime100ns - Qpc100ns();
        if (lead > -10000000 && lead < 10000000) { latencySum += lead; nLatency++; }
    }
}

void Scaler::Impl::RenderThread() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SetThreadDescription(GetCurrentThread(), L"StretchScaler render");

    bool mustWaitSwapChain = true; // waitable starts signaled once
    while (true) {
        // Don't queue more than one frame on the swap chain: wait until the previous present was picked up.
        if (mustWaitSwapChain && waitable) {
            HANDLE hs[2] = {stopEvent, waitable};
            if (WaitForMultipleObjects(2, hs, FALSE, 1000) == WAIT_OBJECT_0) break;
            mustWaitSwapChain = false;
        }

        // Wake on a new capture frame; poll the cursor at ~2 ms when idle so menu cursor movement stays smooth
        // even if the game isn't redrawing.
        HANDLE hs[2] = {stopEvent, frameEvent};
        DWORD timeout = (opt.drawCursor && visible) ? 2 : 100;
        DWORD r = WaitForMultipleObjects(2, hs, FALSE, timeout);
        if (r == WAIT_OBJECT_0) break;

        try {
            if (r == WAIT_OBJECT_0 + 1) {
                // Drain to the newest frame; older ones are dropped (never queued).
                wgc::Direct3D11CaptureFrame frame{nullptr};
                while (auto f = pool.TryGetNextFrame()) {
                    if (frame) frame.Close();
                    frame = f;
                }
                if (!frame) continue;
                int64_t t = frame.SystemRelativeTime().count();
                bool ok = CopyFrame(frame);
                frame.Close();
                if (ok && visible) { Render(t); mustWaitSwapChain = true; }
            } else if (visible && opt.drawCursor && srcSrv) {
                CURSORINFO ci{sizeof(ci)};
                if (GetCursorInfo(&ci)) {
                    bool vis = (ci.flags & CURSOR_SHOWING) != 0;
                    bool changed = ci.ptScreenPos.x != lastCursorPos.x || ci.ptScreenPos.y != lastCursorPos.y ||
                                   ci.hCursor != lastCursorDrawn || (vis != lastCursorVisible && vis);
                    if (changed || (!vis && lastCursorVisible)) { Render(0); mustWaitSwapChain = true; }
                }
            }
        } catch (winrt::hresult_error const& e) {
            Log(L"Render thread error: 0x%08X %s", (unsigned)e.code(), e.message().c_str());
            Sleep(10);
        }
    }
    winrt::uninit_apartment();
}

void Scaler::Impl::Release() {
    if (thread.joinable()) {
        SetEvent(stopEvent);
        thread.join();
    }
    try {
        if (pool) { pool.FrameArrived(frameToken); }
        if (item) { item.Closed(closedToken); }
        if (session) session.Close();
        if (pool) pool.Close();
    } catch (...) {}
    session = nullptr; pool = nullptr; item = nullptr; rtDevice = nullptr;
    gameFps.Stop();
    if (d2dCtx) d2dCtx->SetTarget(nullptr);
    bgBrush = nullptr; textBrush = nullptr; textFormat = nullptr; d2dTarget = nullptr; d2dCtx = nullptr; d2dFactory = nullptr;
    cursor = {};
    srcSrv = nullptr; srcTex = nullptr; srcW = srcH = 0;
    rtv = nullptr;
    media = nullptr;
    if (waitable) { CloseHandle(waitable); waitable = nullptr; }
    sc = nullptr;
    if (ctx) { ctx->ClearState(); ctx->Flush(); }
    vs = nullptr; psOpaque = nullptr; psAlpha = nullptr; sampler = nullptr; blend = nullptr;
    ctx = nullptr; dev = nullptr;
    if (overlay) { DestroyWindow(overlay); overlay = nullptr; }
    if (frameEvent) { CloseHandle(frameEvent); frameEvent = nullptr; }
    if (stopEvent) { CloseHandle(stopEvent); stopEvent = nullptr; }
    running = false;
    visible = false;
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
}

Scaler::Scaler() : m(std::make_unique<Impl>()) {}
Scaler::~Scaler() { Stop(); }

bool Scaler::Start(HWND source, HMONITOR monitor, const RECT& monitorRect, const ScalerOptions& opt,
                   HWND notifyWnd, UINT stoppedMsg) {
    Stop();
    m = std::make_unique<Impl>();
    auto& s = *m;
    s.src = source; s.mon = monitorRect; s.opt = opt; s.notify = notifyWnd; s.stoppedMsg = stoppedMsg;
    s.outW = monitorRect.right - monitorRect.left;
    s.outH = monitorRect.bottom - monitorRect.top;

    if (!IsWindow(source)) { Log(L"Source window no longer exists."); return false; }
    if (IsIconic(source)) { Log(L"Source window is minimized - restore it first (minimized windows cannot be captured)."); return false; }

    s.frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Log(L"Starting: output %dx%d at (%d,%d), filter %s, %s", s.outW, s.outH, monitorRect.left, monitorRect.top,
        opt.pointFilter ? L"point" : L"bilinear", opt.stretch ? L"stretch to fill" : L"fit");

    if (!s.CreateDevice(monitor) || !s.CreatePipeline() || !s.CreateOverlay() || !s.CreateSwapChain() || !s.StartCapture()) {
        s.Release();
        Log(L"Start failed.");
        return false;
    }
    if (opt.fpsOverlay) s.CreateTextOverlay();
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    s.running = true;
    s.thread = std::thread([&s] { s.RenderThread(); });
    return true;
}

void Scaler::Stop() {
    if (!m) return;
    bool was = m->running || m->overlay;
    m->Release();
    if (was) Log(L"Scaling stopped.");
}

bool Scaler::Running() const { return m && m->running; }
HWND Scaler::OverlayHwnd() const { return m ? m->overlay : nullptr; }

void Scaler::SetVisible(bool v) {
    if (!m || !m->overlay) return;
    if (v == m->visible) return;
    m->visible = v;
    if (v) {
        SetWindowPos(m->overlay, HWND_TOPMOST, m->mon.left, m->mon.top, m->outW, m->outH,
                     SWP_SHOWWINDOW | SWP_NOACTIVATE);
        if (m->frameEvent) SetEvent(m->frameEvent); // render immediately with whatever is pending
    } else {
        ShowWindow(m->overlay, SW_HIDE);
    }
}

ScalerStats Scaler::TakeStats() {
    ScalerStats st;
    if (!m) return st;
    st.captured = m->nCaptured.exchange(0);
    st.presented = m->nPresented.exchange(0);
    uint32_t n = m->nLatency.exchange(0);
    int64_t sum = m->latencySum.exchange(0);
    st.avgLeadMs = n ? (double)sum / n / 10000.0 : 0;
    st.presentMode = m->presentMode.load();
    return st;
}
