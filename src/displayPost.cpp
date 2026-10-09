#define NOMINMAX

#include <windows.h>
#include <winhttp.h>
#include <gdiplus.h>
#include <objidl.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <chrono>
#include <conio.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <limits>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

struct Fb {
    int w = 0;
    int h = 0;
    int bpp = 0;
    int stride = 0;

    int redOffset = 0;
    int redLength = 0;
    int greenOffset = 0;
    int greenLength = 0;
    int blueOffset = 0;
    int blueLength = 0;
    int alphaOffset = 0;
    int alphaLength = 0;

    size_t frameSize = 0;
    std::string format;
};

struct Url {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool tls = false;
    bool portSpecified = false;
};

// Set when the executable was launched without command-line parameters.
static bool gInteractiveLaunch = false;

static bool HasExplicitPort(const std::wstring& url) {
    const size_t schemeEnd = url.find(L"://");
    const size_t authorityBegin =
        schemeEnd == std::wstring::npos ? 0 : schemeEnd + 3;
    const size_t authorityEnd =
        url.find_first_of(L"/?#", authorityBegin);
    std::wstring authority = url.substr(
        authorityBegin,
        authorityEnd == std::wstring::npos
            ? std::wstring::npos
            : authorityEnd - authorityBegin);

    const size_t userInfoEnd = authority.rfind(L'@');
    if (userInfoEnd != std::wstring::npos) {
        authority.erase(0, userInfoEnd + 1);
    }

    size_t portSeparator = std::wstring::npos;
    if (!authority.empty() && authority.front() == L'[') {
        const size_t hostEnd = authority.find(L']');
        if (hostEnd != std::wstring::npos &&
            hostEnd + 1 < authority.size() &&
            authority[hostEnd + 1] == L':') {
            portSeparator = hostEnd + 1;
        }
    } else {
        portSeparator = authority.rfind(L':');
    }

    if (portSeparator == std::wstring::npos ||
        portSeparator + 1 == authority.size()) {
        return false;
    }

    return std::all_of(
        authority.begin() + portSeparator + 1,
        authority.end(),
        [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

static bool IsConnectionFailure(DWORD error) {
    return error == ERROR_WINHTTP_CANNOT_CONNECT ||
           error == ERROR_WINHTTP_CONNECTION_ERROR ||
           error == ERROR_WINHTTP_TIMEOUT;
}

static bool AdvancePortFallback(Url& url) {
    if (url.tls) {
        return false;
    }

    if (!url.portSpecified && url.port == INTERNET_DEFAULT_HTTP_PORT) {
        url.port = 8080;
    } else if (url.port == 8080) {
        url.port = 8880;
    } else {
        return false;
    }

    return true;
}

static std::wstring NormalizeUrl(std::wstring url);

static bool Crack(const std::wstring& input, Url& out) {
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);

    wchar_t host[256]{};
    wchar_t path[4096]{};

    const std::wstring fullUrl = NormalizeUrl(input);
    components.lpszHostName = host;
    components.dwHostNameLength = _countof(host);
    components.lpszUrlPath = path;
    components.dwUrlPathLength = _countof(path);

    if (!WinHttpCrackUrl(
            fullUrl.c_str(),
            0,
            0,
            &components)) {
        return false;
    }

    out.host.assign(host, components.dwHostNameLength);
    out.path.assign(path, components.dwUrlPathLength);

    if (out.path.empty() || out.path == L"/") {
        out.path.clear();
    }

    while (out.path.size() > 1 && out.path.back() == L'/') {
        out.path.pop_back();
    }

    // Keep compatibility with the old accidental /fb base URL.
    if (out.path == L"/fb") {
        out.path.clear();
    }

    out.port = components.nPort;
    out.tls = components.nScheme == INTERNET_SCHEME_HTTPS;
    out.portSpecified = HasExplicitPort(fullUrl);
    return true;
}

static std::wstring NormalizeUrl(std::wstring url) {
    if (url.size() < 7 ||
        (_wcsnicmp(url.c_str(), L"http://", 7) != 0 &&
         (url.size() < 8 ||
          _wcsnicmp(url.c_str(), L"https://", 8) != 0))) {
        url = L"http://" + url;
    }

    return url;
}

static std::wstring JoinPath(
    const std::wstring& base,
    const std::wstring& relative) {

    std::wstring rel = relative;

    if (rel.empty()) {
        return base.empty() ? L"/" : base;
    }

    if (rel.front() != L'/') {
        rel.insert(rel.begin(), L'/');
    }

    if (base.empty()) {
        return rel;
    }

    if (base.back() == L'/') {
        return base.substr(0, base.size() - 1) + rel;
    }

    return base + rel;
}

static long ReadNumber(
    const std::string& json,
    const char* key) {

    std::regex expression(
        std::string("\"") + key +
        "\"\\s*:\\s*([0-9]+)");

    std::smatch match;

    if (!std::regex_search(json, match, expression)) {
        throw std::runtime_error(
            std::string("fbinfo missing ") + key);
    }

    return std::stol(match[1]);
}

static std::string ReadString(
    const std::string& json,
    const char* key) {

    std::regex expression(
        std::string("\"") + key +
        "\"\\s*:\\s*\"([^\"]*)\"");

    std::smatch match;

    if (!std::regex_search(json, match, expression)) {
        throw std::runtime_error(
            std::string("fbinfo missing ") + key);
    }

    return match[1];
}

static std::string HttpGet(
    Url& url,
    const std::wstring& relativePath) {

    for (;;) {
        DWORD failureError = ERROR_SUCCESS;
        HINTERNET session = WinHttpOpen(
            L"displayPost/0.5",
            WINHTTP_ACCESS_TYPE_NO_PROXY,
            nullptr,
            nullptr,
            0);

        if (!session) {
            failureError = GetLastError();
            if (IsConnectionFailure(failureError) &&
                AdvancePortFallback(url)) {
                std::wcerr << L"Cannot connect; trying port "
                           << url.port << L".\n";
                continue;
            }
            throw std::runtime_error("WinHttpOpen failed");
        }

        HINTERNET connect = nullptr;
        HINTERNET request = nullptr;

        try {
            connect = WinHttpConnect(
                session,
                url.host.c_str(),
                url.port,
                0);

            if (!connect) {
                failureError = GetLastError();
                throw std::runtime_error("WinHttpConnect failed");
            }

            const std::wstring requestPath =
                JoinPath(url.path, relativePath);

            request = WinHttpOpenRequest(
                connect,
                L"GET",
                requestPath.c_str(),
                nullptr,
                WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES,
                url.tls ? WINHTTP_FLAG_SECURE : 0);

            if (!request) {
                failureError = GetLastError();
                throw std::runtime_error(
                    "WinHttpOpenRequest failed");
            }

            if (!WinHttpSendRequest(
                    request,
                    nullptr,
                    0,
                    nullptr,
                    0,
                    0,
                    0)) {
                failureError = GetLastError();
                throw std::runtime_error(
                    "GET WinHttpSendRequest failed");
            }

            if (!WinHttpReceiveResponse(request, nullptr)) {
                failureError = GetLastError();
                throw std::runtime_error(
                    "GET WinHttpReceiveResponse failed");
            }

            DWORD statusCode = 0;
            DWORD statusCodeSize = sizeof(statusCode);
            if (!WinHttpQueryHeaders(
                    request,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX,
                    &statusCode,
                    &statusCodeSize,
                    WINHTTP_NO_HEADER_INDEX)) {
                failureError = GetLastError();
                throw std::runtime_error(
                    "GET WinHttpQueryHeaders failed");
            }
            if (statusCode < 200 || statusCode >= 300) {
                throw std::runtime_error(
                    "GET HTTP status " + std::to_string(statusCode));
            }

            std::string result;
            char buffer[8192];

            for (;;) {
                DWORD bytesRead = 0;

                if (!WinHttpReadData(
                        request,
                        buffer,
                        sizeof(buffer),
                        &bytesRead)) {
                    failureError = GetLastError();
                    throw std::runtime_error(
                        "GET WinHttpReadData failed");
                }

                if (bytesRead == 0) {
                    break;
                }

                result.append(buffer, buffer + bytesRead);
            }

            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return result;

        } catch (...) {
            if (request) {
                WinHttpCloseHandle(request);
            }
            if (connect) {
                WinHttpCloseHandle(connect);
            }
            WinHttpCloseHandle(session);

            if (IsConnectionFailure(failureError) &&
                AdvancePortFallback(url)) {
                std::wcerr << L"Cannot connect; trying port "
                           << url.port << L".\n";
                continue;
            }
            throw;
        }
    }
}

static Fb ParseFbInfo(const std::string& json) {
    Fb fb;

    fb.w = static_cast<int>(
        ReadNumber(json, "width"));

    fb.h = static_cast<int>(
        ReadNumber(json, "height"));

    fb.bpp = static_cast<int>(
        ReadNumber(json, "bits_per_pixel"));

    fb.stride = static_cast<int>(
        ReadNumber(json, "stride"));

    fb.frameSize = static_cast<size_t>(
        ReadNumber(json, "frame_size"));

    fb.format =
        ReadString(json, "format");

    fb.redOffset = static_cast<int>(
        ReadNumber(json, "red_offset"));
    fb.redLength = static_cast<int>(
        ReadNumber(json, "red_length"));

    fb.greenOffset = static_cast<int>(
        ReadNumber(json, "green_offset"));
    fb.greenLength = static_cast<int>(
        ReadNumber(json, "green_length"));

    fb.blueOffset = static_cast<int>(
        ReadNumber(json, "blue_offset"));
    fb.blueLength = static_cast<int>(
        ReadNumber(json, "blue_length"));

    fb.alphaOffset = static_cast<int>(
        ReadNumber(json, "alpha_offset"));
    fb.alphaLength = static_cast<int>(
        ReadNumber(json, "alpha_length"));

    if (fb.w <= 0 ||
        fb.h <= 0 ||
        fb.stride <= 0 ||
        fb.frameSize == 0) {
        throw std::runtime_error(
            "invalid framebuffer geometry");
    }

    if (fb.bpp != 8 &&
        fb.bpp != 16 &&
        fb.bpp != 24 &&
        fb.bpp != 32) {
        throw std::runtime_error(
            "unsupported framebuffer bpp: " +
            std::to_string(fb.bpp));
    }

    const size_t bytesPerPixel =
        static_cast<size_t>(fb.bpp / 8);

    const size_t minimumStride =
        static_cast<size_t>(fb.w) * bytesPerPixel;

    const size_t minimumFrameSize =
        static_cast<size_t>(fb.stride) *
        static_cast<size_t>(fb.h);

    if (static_cast<size_t>(fb.stride) < minimumStride) {
        throw std::runtime_error(
            "invalid framebuffer stride");
    }

    if (fb.frameSize < minimumFrameSize) {
        throw std::runtime_error(
            "invalid framebuffer frame_size");
    }

    return fb;
}

static Fb ReadFbInfo(Url& parts) {
    for (;;) {
        try {
            return ParseFbInfo(HttpGet(parts, L"/fbinfo"));
        } catch (const std::exception&) {
            if (!AdvancePortFallback(parts)) {
                throw;
            }
            std::wcerr << L"fbinfo request or response was invalid; "
                       << L"trying port " << parts.port << L".\n";
        }
    }
}

struct MonitorInfo {
    RECT rect{};
    bool primary = false;
};

static BOOL CALLBACK EnumMonitorProc(
    HMONITOR monitor,
    HDC,
    LPRECT,
    LPARAM parameter) {

    auto* monitors =
        reinterpret_cast<std::vector<MonitorInfo>*>(
            parameter);

    MONITORINFO info{};
    info.cbSize = sizeof(info);

    if (GetMonitorInfoW(monitor, &info)) {
        MonitorInfo item;
        item.rect = info.rcMonitor;
        item.primary =
            (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        monitors->push_back(item);
    }

    return TRUE;
}

static std::vector<MonitorInfo> EnumerateMonitors() {
    std::vector<MonitorInfo> monitors;

    EnumDisplayMonitors(
        nullptr,
        nullptr,
        EnumMonitorProc,
        reinterpret_cast<LPARAM>(&monitors));

    // Primary display becomes monitor 1.
    std::stable_sort(
        monitors.begin(),
        monitors.end(),
        [](const MonitorInfo& a,
           const MonitorInfo& b) {
            return a.primary > b.primary;
        });

    return monitors;
}

class JpegEncoder {
    ULONG_PTR gdiplusToken_ = 0;
    CLSID jpegClsid_{};

    static bool FindJpegEncoder(CLSID& clsid) {
        UINT encoderCount = 0;
        UINT encoderBytes = 0;

        if (Gdiplus::GetImageEncodersSize(
                &encoderCount,
                &encoderBytes) != Gdiplus::Ok) {
            return false;
        }

        if (encoderBytes == 0) {
            return false;
        }

        std::vector<BYTE> buffer(encoderBytes);

        auto* encoders =
            reinterpret_cast<Gdiplus::ImageCodecInfo*>(
                buffer.data());

        if (Gdiplus::GetImageEncoders(
                encoderCount,
                encoderBytes,
                encoders) != Gdiplus::Ok) {
            return false;
        }

        for (UINT i = 0; i < encoderCount; ++i) {
            if (encoders[i].MimeType &&
                std::wcscmp(
                    encoders[i].MimeType,
                    L"image/jpeg") == 0) {
                clsid = encoders[i].Clsid;
                return true;
            }
        }

        return false;
    }

public:
    JpegEncoder() {
        Gdiplus::GdiplusStartupInput startupInput;

        const Gdiplus::Status status =
            Gdiplus::GdiplusStartup(
                &gdiplusToken_,
                &startupInput,
                nullptr);

        if (status != Gdiplus::Ok ||
            !FindJpegEncoder(jpegClsid_)) {

            if (gdiplusToken_) {
                Gdiplus::GdiplusShutdown(
                    gdiplusToken_);
                gdiplusToken_ = 0;
            }

            throw std::runtime_error(
                "GDI+ JPEG initialization failed");
        }
    }

    ~JpegEncoder() {
        if (gdiplusToken_) {
            Gdiplus::GdiplusShutdown(
                gdiplusToken_);
        }
    }

    bool Encode(
        const BYTE* pixels,
        int width,
        int height,
        int stride,
        ULONG quality,
        std::vector<BYTE>& jpeg) {

        if (!pixels ||
            width <= 0 ||
            height <= 0 ||
            stride <= 0) {
            return false;
        }

        // CreateDIBSection with BI_RGB stores B,G,R,0.
        Gdiplus::Bitmap bitmap(
            width,
            height,
            stride,
            PixelFormat32bppRGB,
            const_cast<BYTE*>(pixels));

        if (bitmap.GetLastStatus() != Gdiplus::Ok) {
            return false;
        }

        IStream* stream = nullptr;

        if (FAILED(CreateStreamOnHGlobal(
                nullptr,
                TRUE,
                &stream))) {
            return false;
        }

        Gdiplus::EncoderParameters parameters{};
        parameters.Count = 1;
        parameters.Parameter[0].Guid =
            Gdiplus::EncoderQuality;
        parameters.Parameter[0].Type =
            Gdiplus::EncoderParameterValueTypeLong;
        parameters.Parameter[0].NumberOfValues = 1;
        parameters.Parameter[0].Value = &quality;

        if (bitmap.Save(
                stream,
                &jpegClsid_,
                &parameters) != Gdiplus::Ok) {
            stream->Release();
            return false;
        }

        STATSTG stat{};

        if (FAILED(stream->Stat(
                &stat,
                STATFLAG_NONAME))) {
            stream->Release();
            return false;
        }

        if (stat.cbSize.QuadPart < 0 ||
            static_cast<unsigned long long>(
                stat.cbSize.QuadPart) >
                std::numeric_limits<size_t>::max()) {
            stream->Release();
            return false;
        }

        const size_t size =
            static_cast<size_t>(stat.cbSize.QuadPart);

        if (size == 0 ||
            size >
                static_cast<size_t>(
                    std::numeric_limits<ULONG>::max())) {
            stream->Release();
            return false;
        }

        jpeg.resize(size);

        LARGE_INTEGER zero{};

        if (FAILED(stream->Seek(
                zero,
                STREAM_SEEK_SET,
                nullptr))) {
            stream->Release();
            jpeg.clear();
            return false;
        }

        ULONG bytesRead = 0;

        const HRESULT result =
            stream->Read(
                jpeg.data(),
                static_cast<ULONG>(jpeg.size()),
                &bytesRead);

        stream->Release();

        if (FAILED(result) ||
            static_cast<size_t>(bytesRead) != jpeg.size()) {
            jpeg.clear();
            return false;
        }

        return true;
    }
};

class Capture {
    Fb fb_{};
    int rotation_ = 0;
    std::vector<BYTE> pixels_;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGIOutputDuplication* duplication_ = nullptr;
    ID3D11Texture2D* staging_ = nullptr;
    int outputLeft_ = 0, outputTop_ = 0;
    int outputWidth_ = 0, outputHeight_ = 0;
    int frameWidth_ = 0, frameHeight_ = 0;
    DXGI_MODE_ROTATION outputRotation_ = DXGI_MODE_ROTATION_UNSPECIFIED;

    void ReleaseDuplication() {
        if (staging_) { staging_->Release(); staging_ = nullptr; }
        if (duplication_) { duplication_->Release(); duplication_ = nullptr; }
        if (context_) { context_->Release(); context_ = nullptr; }
        if (device_) { device_->Release(); device_ = nullptr; }
        frameWidth_ = 0;
        frameHeight_ = 0;
        outputRotation_ = DXGI_MODE_ROTATION_UNSPECIFIED;
    }

    bool OpenOutput(const RECT& source) {
        ReleaseDuplication();
        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return false;
        IDXGIAdapter1* adapter = nullptr; IDXGIOutput* output = nullptr;
        RECT hit{}; bool found = false;
        POINT p{(source.left + source.right) / 2, (source.top + source.bottom) / 2};
        for (UINT ai = 0; !found && factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
            for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; ++oi) {
                DXGI_OUTPUT_DESC d{}; output->GetDesc(&d);
                if (PtInRect(&d.DesktopCoordinates, p)) { hit = d.DesktopCoordinates; found = true; break; }
                output->Release(); output = nullptr;
            }
            if (!found) { adapter->Release(); adapter = nullptr; }
        }
        if (!found) { factory->Release(); return false; }
        ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
        D3D_FEATURE_LEVEL fl{};
        HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &dev, &fl, &ctx);
        IDXGIOutput1* output1 = nullptr;
        if (SUCCEEDED(hr)) hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
        IDXGIOutputDuplication* dup = nullptr;
        if (SUCCEEDED(hr)) hr = output1->DuplicateOutput(dev, &dup);
        if (output1) output1->Release(); output->Release(); adapter->Release(); factory->Release();
        if (FAILED(hr)) { if (ctx) ctx->Release(); if (dev) dev->Release(); return false; }
        DXGI_OUTDUPL_DESC dd{}; dup->GetDesc(&dd);
        const bool quarterTurn =
            dd.Rotation == DXGI_MODE_ROTATION_ROTATE90 ||
            dd.Rotation == DXGI_MODE_ROTATION_ROTATE270;
        const UINT frameWidth =
            quarterTurn ? dd.ModeDesc.Height : dd.ModeDesc.Width;
        const UINT frameHeight =
            quarterTurn ? dd.ModeDesc.Width : dd.ModeDesc.Height;
        D3D11_TEXTURE2D_DESC td{}; td.Width = frameWidth; td.Height = frameHeight;
        td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* staging = nullptr;
        hr = dev->CreateTexture2D(&td, nullptr, &staging);
        if (FAILED(hr)) { dup->Release(); ctx->Release(); dev->Release(); return false; }
        device_ = dev; context_ = ctx; duplication_ = dup; staging_ = staging;
        outputLeft_ = hit.left;
        outputTop_ = hit.top;
        outputWidth_ = hit.right - hit.left;
        outputHeight_ = hit.bottom - hit.top;
        frameWidth_ = static_cast<int>(frameWidth);
        frameHeight_ = static_cast<int>(frameHeight);
        outputRotation_ = dd.Rotation;
        return true;
    }

    /* DXGI supplies BGRA pixels; scaling and rotation are applied while copying. */
    static void Rotate90CW(
        const BYTE* src,
        int srcWidth,
        int srcHeight,
        int srcStride,
        BYTE* dst,
        int dstWidth,
        int dstHeight,
        int dstStride,
        int dstLeft,
        int dstTop) {

        (void)dstWidth;
        (void)dstHeight;

        for (int y = 0; y < srcHeight; ++y) {
            const BYTE* srcRow =
                src + static_cast<size_t>(y) * srcStride;

            for (int x = 0; x < srcWidth; ++x) {
                const BYTE* pixel = srcRow + x * 4;

                const int dx =
                    dstLeft + (srcHeight - 1 - y);
                const int dy =
                    dstTop + x;

                BYTE* out =
                    dst + static_cast<size_t>(dy) * dstStride +
                    static_cast<size_t>(dx) * 4;

                out[0] = pixel[0];
                out[1] = pixel[1];
                out[2] = pixel[2];
                out[3] = pixel[3];
            }
        }
    }

    static void Rotate90CCW(
        const BYTE* src,
        int srcWidth,
        int srcHeight,
        int srcStride,
        BYTE* dst,
        int dstWidth,
        int dstHeight,
        int dstStride,
        int dstLeft,
        int dstTop) {

        (void)dstWidth;
        (void)dstHeight;

        for (int y = 0; y < srcHeight; ++y) {
            const BYTE* srcRow =
                src + static_cast<size_t>(y) * srcStride;

            for (int x = 0; x < srcWidth; ++x) {
                const BYTE* pixel = srcRow + x * 4;

                const int dx =
                    dstLeft + y;
                const int dy =
                    dstTop + (srcWidth - 1 - x);

                BYTE* out =
                    dst + static_cast<size_t>(dy) * dstStride +
                    static_cast<size_t>(dx) * 4;

                out[0] = pixel[0];
                out[1] = pixel[1];
                out[2] = pixel[2];
                out[3] = pixel[3];
            }
        }
    }

    static void Rotate180(
        const BYTE* src,
        int srcWidth,
        int srcHeight,
        int srcStride,
        BYTE* dst,
        int dstWidth,
        int dstHeight,
        int dstStride,
        int dstLeft,
        int dstTop) {

        (void)dstWidth;
        (void)dstHeight;

        for (int y = 0; y < srcHeight; ++y) {
            const BYTE* srcRow =
                src + static_cast<size_t>(y) * srcStride;

            for (int x = 0; x < srcWidth; ++x) {
                const BYTE* pixel = srcRow + x * 4;

                const int dx =
                    dstLeft + (srcWidth - 1 - x);
                const int dy =
                    dstTop + (srcHeight - 1 - y);

                BYTE* out =
                    dst + static_cast<size_t>(dy) * dstStride +
                    static_cast<size_t>(dx) * 4;

                out[0] = pixel[0];
                out[1] = pixel[1];
                out[2] = pixel[2];
                out[3] = pixel[3];
            }
        }
    }

public:
    ~Capture() { ReleaseDuplication(); }

    void ResetOutput() {
        ReleaseDuplication();
        outputLeft_ = 0;
        outputTop_ = 0;
        outputWidth_ = 0;
        outputHeight_ = 0;
    }

    bool Init(const Fb& fb, int rotation) {
        fb_ = fb; rotation_ = rotation; pixels_.assign((size_t)fb.w * fb.h * 4, 0); return true;
    }

    bool CaptureFrame(const RECT& source) {
        const int sourceWidth = source.right - source.left, sourceHeight = source.bottom - source.top;
        if (sourceWidth <= 0 || sourceHeight <= 0 || fb_.w <= 0 || fb_.h <= 0) return false;
        if (!duplication_ || source.left < outputLeft_ || source.top < outputTop_ ||
            source.right > outputLeft_ + outputWidth_ || source.bottom > outputTop_ + outputHeight_) {
            if (!OpenOutput(source)) return false;
        }
        DXGI_OUTDUPL_FRAME_INFO fi{}; IDXGIResource* resource = nullptr;
        HRESULT hr = duplication_->AcquireNextFrame(100, &fi, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED) { ReleaseDuplication(); return false; }
        if (FAILED(hr)) return false;
        ID3D11Texture2D* frame = nullptr; hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&frame);
        if (SUCCEEDED(hr)) { context_->CopyResource(staging_, frame); frame->Release(); }
        resource->Release(); duplication_->ReleaseFrame();
        if (FAILED(hr)) return false;
        D3D11_MAPPED_SUBRESOURCE map{}; if (FAILED(context_->Map(staging_, 0, D3D11_MAP_READ, 0, &map))) return false;

        const bool quarterTurn = rotation_ == 90 || rotation_ == 270;

        /*
            Work out the aspect ratio of the image AFTER
            rotation. This determines the letterbox size.
        */
        const int rotatedSourceWidth =
            quarterTurn ? sourceHeight : sourceWidth;
        const int rotatedSourceHeight =
            quarterTurn ? sourceWidth : sourceHeight;

        const double sourceAspect =
            static_cast<double>(rotatedSourceWidth) /
            static_cast<double>(rotatedSourceHeight);

        const double targetAspect =
            static_cast<double>(fb_.w) /
            static_cast<double>(fb_.h);

        int contentWidth = fb_.w;
        int contentHeight = fb_.h;

        if (sourceAspect > targetAspect) {
            contentHeight = static_cast<int>(
                static_cast<double>(fb_.w) /
                sourceAspect +
                0.5);
        } else {
            contentWidth = static_cast<int>(
                static_cast<double>(fb_.h) *
                sourceAspect +
                0.5);
        }

        contentWidth = std::max(1, contentWidth);
        contentHeight = std::max(1, contentHeight);

        const int left =
            (fb_.w - contentWidth) / 2;
        const int top =
            (fb_.h - contentHeight) / 2;

        std::fill(pixels_.begin(), pixels_.end(), 0);
        const BYTE* src = (const BYTE*)map.pData;
        const int srcStride = (int)map.RowPitch;
        for (int y=0; y<contentHeight; ++y) for (int x=0; x<contentWidth; ++x) {
            int ux=x, uy=y;
            if (rotation_ == 90) { ux = (int)((long long)y * sourceWidth / contentHeight); uy = sourceHeight - 1 - (int)((long long)x * sourceHeight / contentWidth); }
            else if (rotation_ == 180) { ux = sourceWidth - 1 - (int)((long long)x * sourceWidth / contentWidth); uy = sourceHeight - 1 - (int)((long long)y * sourceHeight / contentHeight); }
            else if (rotation_ == 270) { ux = sourceWidth - 1 - (int)((long long)y * sourceWidth / contentHeight); uy = (int)((long long)x * sourceHeight / contentWidth); }
            else { ux = (int)((long long)x * sourceWidth / contentWidth); uy = (int)((long long)y * sourceHeight / contentHeight); }
            const int desktopX = source.left - outputLeft_ + ux;
            const int desktopY = source.top - outputTop_ + uy;
            int sx = desktopX;
            int sy = desktopY;
            switch (outputRotation_) {
            case DXGI_MODE_ROTATION_ROTATE90:
                sx = desktopY;
                sy = frameHeight_ - 1 - desktopX;
                break;
            case DXGI_MODE_ROTATION_ROTATE180:
                sx = frameWidth_ - 1 - desktopX;
                sy = frameHeight_ - 1 - desktopY;
                break;
            case DXGI_MODE_ROTATION_ROTATE270:
                sx = frameWidth_ - 1 - desktopY;
                sy = desktopX;
                break;
            default:
                break;
            }
            if (sx < 0 || sy < 0 || sx >= frameWidth_ || sy >= frameHeight_) {
                context_->Unmap(staging_, 0);
                return false;
            }
            BYTE* q = pixels_.data() + ((size_t)(top+y)*fb_.w + left+x)*4;
            memcpy(q, src + (size_t)sy*srcStride + sx*4, 4);
        }
        context_->Unmap(staging_, 0); return true;

    }

    const BYTE* Pixels() const {
        return pixels_.data();
    }

    int Width() const {
        return fb_.w;
    }

    int Height() const {
        return fb_.h;
    }

    int Stride() const {
        return fb_.w * 4;
    }
};


enum class ColorMode : uint64_t {
    Auto = 0,
    Color = 1,
    Grayscale = 2,
    FloydSteinberg = 3,
    Bayer4x4 = 4,
    Binary = 5,
};

static BYTE Luminance(const BYTE* pixel) {
    return static_cast<BYTE>((static_cast<unsigned>(pixel[2]) * 299u +
                              static_cast<unsigned>(pixel[1]) * 587u +
                              static_cast<unsigned>(pixel[0]) * 114u + 500u) / 1000u);
}

static void ApplyColorMode(std::vector<BYTE>& pixels, int width, int height, ColorMode mode) {
    if (mode == ColorMode::Auto || mode == ColorMode::Color) {
        return;
    }

    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (mode == ColorMode::Grayscale) {
        for (size_t i = 0; i < count; ++i) {
            BYTE* p = pixels.data() + i * 4;
            const BYTE y = Luminance(p);
            p[0] = p[1] = p[2] = y;
        }
        return;
    }

    if (mode == ColorMode::FloydSteinberg) {
        std::vector<float> values(count);
        for (size_t i = 0; i < count; ++i) {
            values[i] = static_cast<float>(Luminance(pixels.data() + i * 4));
        }
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const size_t i = static_cast<size_t>(y) * width + x;
                const float oldValue = std::clamp(values[i], 0.0f, 255.0f);
                const float newValue = oldValue < 128.0f ? 0.0f : 255.0f;
                const float error = oldValue - newValue;
                values[i] = newValue;
                if (x + 1 < width) values[i + 1] += error * 7.0f / 16.0f;
                if (y + 1 < height) {
                    if (x > 0) values[i + width - 1] += error * 3.0f / 16.0f;
                    values[i + width] += error * 5.0f / 16.0f;
                    if (x + 1 < width) values[i + width + 1] += error * 1.0f / 16.0f;
                }
            }
        }
        for (size_t i = 0; i < count; ++i) {
            BYTE* p = pixels.data() + i * 4;
            const BYTE v = values[i] < 128.0f ? 0 : 255;
            p[0] = p[1] = p[2] = v;
        }
        return;
    }

    if (mode == ColorMode::Bayer4x4 || mode == ColorMode::Binary) {
        static constexpr int bayer[4][4] = {
            {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}
        };
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const size_t i = static_cast<size_t>(y) * width + x;
                const BYTE gray = Luminance(pixels.data() + i * 4);
                const int threshold = mode == ColorMode::Binary
                    ? 128
                    : bayer[y & 3][x & 3] * 16 + 8;
                BYTE* p = pixels.data() + i * 4;
                const BYTE v = gray >= threshold ? 255 : 0;
                p[0] = p[1] = p[2] = v;
            }
        }
    }
}

class Stream {
    HINTERNET session_ = nullptr;
    HINTERNET connect_ = nullptr;
    HINTERNET request_ = nullptr;
    DWORD lastWriteError_ = ERROR_SUCCESS;

    bool WriteRaw(const void* data, size_t size) {
        const BYTE* p = static_cast<const BYTE*>(data);

        while (size > 0) {
            const DWORD chunk = static_cast<DWORD>(
                std::min<size_t>(
                    size,
                    std::numeric_limits<DWORD>::max()));

            if (!WinHttpWriteData(
                    request_,
                    p,
                    chunk,
                    nullptr)) {
                lastWriteError_ = GetLastError();
                return false;
            }

            p += chunk;
            size -= chunk;
        }

        return true;
    }

    /*
        Write one HTTP/1.1 chunk.

        The wire format is:

            HEX_LENGTH\r\n
            DATA\r\n
        WinHTTP accepts these bytes through WinHttpWriteData.
    */
    bool WriteChunk(const void* data, size_t size) {
        char length[32]{};

        const int lengthChars = std::snprintf(
            length,
            sizeof(length),
            "%zX\r\n",
            size);

        if (lengthChars <= 0 ||
            static_cast<size_t>(lengthChars) >= sizeof(length)) {
            return false;
        }

        if (!WriteRaw(
                length,
                static_cast<size_t>(lengthChars))) {
            return false;
        }

        if (size > 0 && !WriteRaw(data, size)) {
            return false;
        }

        static const char crlf[] = "\r\n";

        return WriteRaw(
            crlf,
            sizeof(crlf) - 1);
    }

    bool WriteChunkedEnd() {
        static const char end[] = "0\r\n\r\n";

        return WriteRaw(
            end,
            sizeof(end) - 1);
    }

public:
    DWORD LastWriteError() const {
        return lastWriteError_;
    }

    bool ReadFailureResponse(
        DWORD& statusCode,
        std::wstring& location) {
        if (!request_ || !WinHttpReceiveResponse(request_, nullptr)) {
            return false;
        }

        DWORD statusCodeSize = sizeof(statusCode);
        if (!WinHttpQueryHeaders(
                request_,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &statusCode,
                &statusCodeSize,
                WINHTTP_NO_HEADER_INDEX)) {
            return false;
        }

        DWORD locationSize = 0;
        if (WinHttpQueryHeaders(
                request_,
                WINHTTP_QUERY_LOCATION,
                WINHTTP_HEADER_NAME_BY_INDEX,
                nullptr,
                &locationSize,
                WINHTTP_NO_HEADER_INDEX)) {
            return true;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
            locationSize < sizeof(wchar_t)) {
            return true;
        }

        std::vector<wchar_t> locationBuffer(
            locationSize / sizeof(wchar_t));
        if (!WinHttpQueryHeaders(
                request_,
                WINHTTP_QUERY_LOCATION,
                WINHTTP_HEADER_NAME_BY_INDEX,
                locationBuffer.data(),
                &locationSize,
                WINHTTP_NO_HEADER_INDEX)) {
            return true;
        }

        location.assign(locationBuffer.data());
        return true;
    }

    void Close() {
        if (request_) {
            WinHttpCloseHandle(request_);
            request_ = nullptr;
        }
        lastWriteError_ = ERROR_SUCCESS;

        if (connect_) {
            WinHttpCloseHandle(connect_);
            connect_ = nullptr;
        }

        if (session_) {
            WinHttpCloseHandle(session_);
            session_ = nullptr;
        }
    }

    ~Stream() {
        Close();
    }

    bool Open(Url& url) {
        Close();

        const std::wstring path = JoinPath(
            url.path,
            L"/fb");

        /*
            This is the same HTTP streaming mechanism used by
            curl -T - for an unknown-length request body.

            WinHTTP must see Transfer-Encoding before SendRequest.
        */
        static const wchar_t transferEncoding[] =
            L"Transfer-Encoding: chunked\r\n";

        static const wchar_t contentType[] =
            L"Content-Type: multipart/x-mixed-replace; "
            L"boundary=ffmpeg\r\n"
            L"Expect:\r\n";

        /*
            Unknown total length because the stream is continuous.
            The actual HTTP chunks are written by WriteChunk().
        */
        for (;;) {
            DWORD failureError = ERROR_SUCCESS;
            session_ = WinHttpOpen(
                L"displayPost/0.6",
                WINHTTP_ACCESS_TYPE_NO_PROXY,
                nullptr,
                nullptr,
                0);

            if (!session_) {
                failureError = GetLastError();
            } else {
                WinHttpSetTimeouts(
                    session_,
                    5000,
                    5000,
                    5000,
                    5000);

                connect_ = WinHttpConnect(
                    session_,
                    url.host.c_str(),
                    url.port,
                    0);
                if (!connect_) {
                    failureError = GetLastError();
                } else {
                    request_ = WinHttpOpenRequest(
                        connect_,
                        L"POST",
                        path.c_str(),
                        nullptr,
                        WINHTTP_NO_REFERER,
                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                        url.tls ? WINHTTP_FLAG_SECURE : 0);
                    if (!request_) {
                        failureError = GetLastError();
                    } else if (!WinHttpAddRequestHeaders(
                                   request_,
                                   transferEncoding,
                                   static_cast<DWORD>(-1),
                                   WINHTTP_ADDREQ_FLAG_ADD)) {
                        failureError = GetLastError();
                    } else if (!WinHttpAddRequestHeaders(
                                   request_,
                                   contentType,
                                   static_cast<DWORD>(-1),
                                   WINHTTP_ADDREQ_FLAG_ADD)) {
                        failureError = GetLastError();
                    } else if (WinHttpSendRequest(
                                   request_,
                                   WINHTTP_NO_ADDITIONAL_HEADERS,
                                   0,
                                   WINHTTP_NO_REQUEST_DATA,
                                   0,
                                   WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH,
                                   0)) {
                        return true;
                    } else {
                        failureError = GetLastError();
                    }
                }
            }

            Close();
            if (IsConnectionFailure(failureError) &&
                AdvancePortFallback(url)) {
                std::wcerr << L"Cannot connect; trying port "
                           << url.port << L".\n";
                continue;
            }
            return false;
        }
    }

    bool WriteFrame(const std::vector<BYTE>& jpeg) {
        if (jpeg.empty()) {
            return false;
        }

        /*
            Multipart MJPEG frame.  After HTTP chunk decoding,
            Go's multipart.Reader sees exactly this:

                --ffmpeg\r\n
                Content-type: image/jpeg\r\n
                Content-length: N\r\n
                \r\n
                JPEG
                \r\n
        */
        const std::string partHeader =
            "--ffmpeg\r\n"
            "Content-type: image/jpeg\r\n"
            "Content-length: " +
            std::to_string(jpeg.size()) +
            "\r\n"
            "\r\n";

        const size_t totalSize =
            partHeader.size() +
            jpeg.size() +
            2;

        std::vector<BYTE> part(totalSize);
        BYTE* p = part.data();

        std::memcpy(
            p,
            partHeader.data(),
            partHeader.size());

        p += partHeader.size();

        std::memcpy(
            p,
            jpeg.data(),
            jpeg.size());

        p += jpeg.size();

        p[0] = '\r';
        p[1] = '\n';

        return WriteChunk(
            part.data(),
            part.size());
    }

    bool Finish() {
        /*
            Complete the multipart stream first.
        */
        static const char endBoundary[] =
            "--ffmpeg--\r\n";

        if (!WriteChunk(
                endBoundary,
                sizeof(endBoundary) - 1)) {
            return false;
        }

        /*
            Then terminate HTTP chunked transfer.
        */
        if (!WriteChunkedEnd()) {
            return false;
        }

        if (!WinHttpReceiveResponse(
                request_,
                nullptr)) {
            return false;
        }

        char buffer[4096];

        for (;;) {
            DWORD bytesRead = 0;

            if (!WinHttpReadData(
                    request_,
                    buffer,
                    sizeof(buffer),
                    &bytesRead)) {
                return false;
            }

            if (bytesRead == 0) {
                break;
            }
        }

        return true;
    }
};

static uint64_t ParseUnsigned(
    const wchar_t* text) {

    if (!text || !*text) {
        throw std::runtime_error(
            "invalid numeric argument");
    }

    wchar_t* end = nullptr;

    const uint64_t value =
        _wcstoui64(text, &end, 10);

    if (!end || *end != L'\0') {
        throw std::runtime_error(
            "invalid numeric argument");
    }

    return value;
}

int wmain(
    int argc,
    wchar_t** argv) {

    if (!SetProcessDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        SetProcessDPIAware();
    }

    if (argc >= 2 && argc < 7) {
        std::vector<std::wstring> fixedArgs;
        for (int i = 0; i < argc; ++i) {
            fixedArgs.push_back(argv[i]);
        }

        while (fixedArgs.size() < 7) {
            if (fixedArgs.size() == 2) fixedArgs.push_back(L"30");
            else if (fixedArgs.size() == 3) fixedArgs.push_back(L"1");
            else if (fixedArgs.size() == 4) fixedArgs.push_back(L"0");
            else if (fixedArgs.size() == 5) fixedArgs.push_back(L"90");
            else if (fixedArgs.size() == 6) fixedArgs.push_back(L"0");
        }

        std::vector<wchar_t*> fixedArgv;
        for (auto& a : fixedArgs) {
            fixedArgv.push_back(const_cast<wchar_t*>(a.c_str()));
        }

        return wmain(static_cast<int>(fixedArgv.size()), fixedArgv.data());
    }

    if (argc < 5) {
        gInteractiveLaunch = true;

        std::vector<std::wstring> args;
        args.reserve(7);
        args.push_back(argv[0]);

        std::wstring urlInput;
        std::wstring fpsInput;
        std::wstring monitorInput;
        std::wstring rotationInput;
        std::wstring qualityInput;
        std::wstring colorModeInput;

        if (argc >= 2) {
            urlInput = argv[1];
        } else {
            std::wcout << L"workdayAlarmClockGo URL: ";
            std::getline(std::wcin, urlInput);
            if (urlInput.empty()) {
                std::wcerr << L"URL is empty\n";
                return 2;
            }
        }

        if (argc >= 3) {
            fpsInput = argv[2];
        } else {
            std::wcout << L"fps (default 30): ";
            std::getline(std::wcin, fpsInput);
        }

        if (argc >= 4) {
            monitorInput = argv[3];
        } else {
            std::wcout << L"monitor (default 1): ";
            std::getline(std::wcin, monitorInput);
        }

        if (argc >= 5) {
            rotationInput = argv[4];
        } else {
            std::wcout << L"rotation (0/90/180/270 default 0): ";
            std::getline(std::wcin, rotationInput);
        }

        if (fpsInput.empty()) {
            fpsInput = L"30";
        }
        if (monitorInput.empty()) {
            monitorInput = L"1";
        }
        if (rotationInput.empty()) {
            rotationInput = L"0";
        }
        std::wcout << L"quality (0-100, default 75): ";
        std::getline(std::wcin, qualityInput);
        if (qualityInput.empty()) {
            qualityInput = L"75";
        }
        // std::wcout << L"color mode (0-5, default 0): ";
        // std::getline(std::wcin, colorModeInput);
        if (colorModeInput.empty()) {
            colorModeInput = L"0";
        }

        const std::wstring normalizedUrl = NormalizeUrl(urlInput);

        args.push_back(normalizedUrl);
        args.push_back(fpsInput);
        args.push_back(monitorInput);
        args.push_back(rotationInput);
        args.push_back(qualityInput);
        args.push_back(colorModeInput);

        std::vector<wchar_t*> interactiveArgs;
        for (auto& arg : args) {
            interactiveArgs.push_back(const_cast<wchar_t*>(arg.c_str()));
        }

        return wmain(
            static_cast<int>(interactiveArgs.size()),
            interactiveArgs.data());
    }

    for (;;) {
        const HRESULT comResult =
            CoInitializeEx(
                nullptr,
                COINIT_MULTITHREADED);

        const bool comInitialized =
            SUCCEEDED(comResult);

        if (FAILED(comResult) &&
            comResult != RPC_E_CHANGED_MODE) {
            std::cerr
                << "error: CoInitializeEx failed\n";
            if (!gInteractiveLaunch) {
                return 1;
            }
            std::wcerr << L"Press Enter to retry.\n";
            std::wstring line;
            if (!std::getline(std::wcin, line)) {
                return 1;
            }
            continue;
        }

        try {
        const std::wstring url = NormalizeUrl(argv[1]);
        Url activeUrl;
        if (!Crack(url, activeUrl)) {
            throw std::runtime_error("bad URL");
        }

        const uint64_t fps =
            argc > 2
                ? ParseUnsigned(argv[2])
                : 30;

        uint64_t monitorIndex =
            argc > 3
                ? ParseUnsigned(argv[3])
                : 1;

        // Rotation angle, clockwise: 0, 90, 180, or 270 degrees.
        const uint64_t rotation =
            argc > 4
                ? ParseUnsigned(argv[4])
                : 0;

        const uint64_t jpegQuality =
            argc > 5
                ? ParseUnsigned(argv[5])
                : 90;

        const uint64_t colorModeValue =
            argc > 6
                ? ParseUnsigned(argv[6])
                : 0;

        if (fps == 0 || fps > 1000) {
            throw std::runtime_error(
                "fps must be between 1 and 1000");
        }

        if (monitorIndex < 1) {
            monitorIndex = 1;
        }

        const Fb fb = ReadFbInfo(activeUrl);

        const std::vector<MonitorInfo> monitors =
            EnumerateMonitors();

        if (monitors.empty()) {
            throw std::runtime_error(
                "no Windows monitors found");
        }

        if (monitorIndex > monitors.size()) {
            monitorIndex = monitors.size();
        }

        if (rotation != 0 &&
            rotation != 90 &&
            rotation != 180 &&
            rotation != 270) {
            throw std::runtime_error(
                "rotation must be 0, 90, 180, or 270 degrees");
        }

        if (jpegQuality > 100) {
            throw std::runtime_error(
                "JPEG quality must be between 0 and 100");
        }

        if (colorModeValue > static_cast<uint64_t>(ColorMode::Binary)) {
            throw std::runtime_error(
                "color mode must be between 0 and 5");
        }
        const ColorMode colorMode = static_cast<ColorMode>(colorModeValue);

        std::wcout << L"Configuration:\n"
                   << L"  URL: " << url << L"\n"
                   << L"  Framebuffer: " << fb.w << L"x" << fb.h << L"\n"
                   << L"  FPS: " << fps << L"\n"
                   << L"  Monitor: " << monitorIndex << L"\n"
                   << L"  Rotation: " << rotation << L"\n"
                   << L"  JPEG quality: " << jpegQuality << L"\n"
                   << L"  Color mode: " << colorModeValue << L"\n"
                   << L"Starting transmission..." << std::endl;

        Capture capture;

        if (!capture.Init(fb, static_cast<int>(rotation))) {
            throw std::runtime_error(
                "capture initialization failed");
        }

        JpegEncoder encoder;
        Stream stream;

        std::vector<BYTE> jpeg;

        const auto frameInterval =
            std::chrono::microseconds(
                static_cast<long long>(1000000 / fps));

        auto nextFrame =
            std::chrono::steady_clock::now();

        uint64_t sentFrames = 0;

        const bool interactiveMode = gInteractiveLaunch;
        bool connected = false;
        bool everConnected = false;
        auto nextReconnect = std::chrono::steady_clock::now();
        int interactiveRetries = 0;
        int autoReconnectRetries = 0;
        bool forceReconnect = false;

        RECT monitorRect =
            monitors[
                static_cast<size_t>(monitorIndex - 1)
            ].rect;

        auto refreshMonitor = [&]() {
            const std::vector<MonitorInfo> currentMonitors =
                EnumerateMonitors();
            if (currentMonitors.empty()) {
                capture.ResetOutput();
                std::cerr << "Unable to refresh monitor list: no Windows monitors found."
                          << std::endl;
                return false;
            }

            if (monitorIndex > currentMonitors.size()) {
                monitorIndex = currentMonitors.size();
            }
            monitorRect =
                currentMonitors[
                    static_cast<size_t>(monitorIndex - 1)
                ].rect;
            capture.ResetOutput();
            std::cout << "Refreshed monitor " << monitorIndex << ": "
                      << monitorRect.right - monitorRect.left << "x"
                      << monitorRect.bottom - monitorRect.top << " at ("
                      << monitorRect.left << ", " << monitorRect.top << ")."
                      << std::endl;
            return true;
        };

        for (;;) {
            if (_kbhit()) {
                int ch = _getch();
                if (ch == 13) {
                    std::cout << "Manual reconnect requested." << std::endl;
                    refreshMonitor();
                    connected = false;
                    stream.Close();
                    interactiveRetries = 0;
                    forceReconnect = true;
                    continue;
                }
            }

            if (!connected) {
                if (!interactiveMode &&
                    std::chrono::steady_clock::now() < nextReconnect) {
                    std::this_thread::sleep_until(nextReconnect);
                }
                if (interactiveMode && everConnected && interactiveRetries < 3 && !forceReconnect) {
                    std::cout << "Disconnected. Retrying in 1 seconds (" << interactiveRetries + 1 << "/3)..."
                              << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                if (!interactiveMode && everConnected) {
                    if (autoReconnectRetries < 3) {
                        std::cout << "Disconnected. Retrying in 1 second (" << autoReconnectRetries + 1 << "/3)..."
                                  << std::endl;
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    } else {
                        std::cout << "Disconnected. Retrying in 10 seconds (" << autoReconnectRetries + 1 << "/3)..."
                                  << std::endl;
                        std::this_thread::sleep_for(std::chrono::seconds(10));
                    }
                }

                connected = stream.Open(activeUrl);
                if (!connected) {
                    if (interactiveMode) {
                        ++interactiveRetries;
                        if (interactiveRetries >= 3) {
                            std::cout << "Connection failed 3 times. Press Enter to retry."
                                      << std::endl;
                            std::wstring line;
                            std::getline(std::wcin, line);
                            refreshMonitor();
                            interactiveRetries = 0;
                            forceReconnect = true;
                        } else {
                            std::cout << "Connection failed. Retrying in 1 seconds (" << interactiveRetries + 1 << "/3)..."
                                      << std::endl;
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                        }
                    } else {
                        ++autoReconnectRetries;
                        if (autoReconnectRetries > 3) {
                            autoReconnectRetries = 3;
                        }
                    }
                    continue;
                }

                everConnected = true;
                interactiveRetries = 0;
                autoReconnectRetries = 0;
                forceReconnect = false;
                std::cout << "Connected. Transmission started." << std::endl;
                nextFrame = std::chrono::steady_clock::now();
            }

            if (!capture.CaptureFrame(monitorRect)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            const BYTE* pixels = capture.Pixels();
            std::vector<BYTE> processedPixels;
            if (colorMode != ColorMode::Auto && colorMode != ColorMode::Color) {
                const size_t pixelBytes =
                    static_cast<size_t>(capture.Width()) * capture.Height() * 4;
                processedPixels.assign(pixels, pixels + pixelBytes);
                ApplyColorMode(
                    processedPixels,
                    capture.Width(),
                    capture.Height(),
                    colorMode);
                pixels = processedPixels.data();
            }

            if (!encoder.Encode(
                    pixels,
                    capture.Width(),
                    capture.Height(),
                    capture.Stride(),
                    static_cast<ULONG>(jpegQuality),
                    jpeg)) {
                throw std::runtime_error(
                    "JPEG encode failed");
            }

            if (!stream.WriteFrame(jpeg)) {
                connected = false;
                DWORD httpStatus = 0;
                std::wstring location;
                bool portFallbackApplied = false;

                if (stream.ReadFailureResponse(httpStatus, location) &&
                    httpStatus >= 300) {
                    Url redirected;
                    if (!location.empty() && Crack(location, redirected)) {
                        activeUrl = redirected;
                        portFallbackApplied = true;
                        std::wcout << L"Following HTTP redirect to "
                                   << activeUrl.host << L":"
                                   << activeUrl.port << activeUrl.path << L"\n";
                    } else if (AdvancePortFallback(activeUrl)) {
                        portFallbackApplied = true;
                        std::wcout << L"HTTP status " << httpStatus
                                   << L"; trying port "
                                   << activeUrl.port << L".\n";
                    }
                }

                if (!portFallbackApplied &&
                    IsConnectionFailure(stream.LastWriteError()) &&
                    AdvancePortFallback(activeUrl)) {
                    std::wcout << L"WinHTTP write connection error; trying port "
                               << activeUrl.port << L".\n";
                }

                stream.Close();
                if (!interactiveMode) {
                    ++autoReconnectRetries;
                    if (autoReconnectRetries > 3) {
                        autoReconnectRetries = 3;
                    }
                }
                continue;
            }

            ++sentFrames;

            nextFrame += frameInterval;
            std::this_thread::sleep_until(nextFrame);
        }

        if (comInitialized) {
            CoUninitialize();
        }

        return 0;

        } catch (const std::exception& e) {
            std::cerr
                << "error: "
                << e.what()
                << "\n";

            if (comInitialized) {
                CoUninitialize();
            }

            if (!gInteractiveLaunch) {
                return 1;
            }
            std::wcerr << L"Press Enter to retry.\n";
            std::wstring line;
            if (!std::getline(std::wcin, line)) {
                return 1;
            }
        } catch (...) {
            std::cerr << "error: unknown failure\n";

            if (comInitialized) {
                CoUninitialize();
            }

            if (!gInteractiveLaunch) {
                return 1;
            }
            std::wcerr << L"Press Enter to retry.\n";
            std::wstring line;
            if (!std::getline(std::wcin, line)) {
                return 1;
            }
        }
    }
}
