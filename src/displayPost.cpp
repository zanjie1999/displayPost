#define NOMINMAX

#include <windows.h>
#include <winhttp.h>
#include <gdiplus.h>
#include <objidl.h>

#include <algorithm>
#include <chrono>
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
};

static bool Crack(const std::wstring& input, Url& out) {
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);

    wchar_t host[256]{};
    wchar_t path[4096]{};

    components.lpszHostName = host;
    components.dwHostNameLength = _countof(host);
    components.lpszUrlPath = path;
    components.dwUrlPathLength = _countof(path);

    if (!WinHttpCrackUrl(
            input.c_str(),
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
    return true;
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
    const std::wstring& baseUrl,
    const std::wstring& relativePath) {

    Url url;

    if (!Crack(baseUrl, url)) {
        throw std::runtime_error("bad URL");
    }

    HINTERNET session = WinHttpOpen(
        L"displayPost/0.5",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        nullptr,
        nullptr,
        0);

    if (!session) {
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
            throw std::runtime_error(
                "GET WinHttpSendRequest failed");
        }

        if (!WinHttpReceiveResponse(request, nullptr)) {
            throw std::runtime_error(
                "GET WinHttpReceiveResponse failed");
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
        throw;
    }
}

static Fb ReadFbInfo(const std::wstring& url) {
    const std::string json =
        HttpGet(url, L"/fbinfo");

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

        ULONG quality = 80;

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
    Fb fb_;

    HDC screen_ = nullptr;
    HDC dc_ = nullptr;

    HBITMAP bitmap_ = nullptr;
    void* bits_ = nullptr;

    HGDIOBJ oldBitmap_ = nullptr;

public:
    ~Capture() {
        if (dc_ && oldBitmap_) {
            SelectObject(dc_, oldBitmap_);
        }

        if (bitmap_) {
            DeleteObject(bitmap_);
        }

        if (dc_) {
            DeleteDC(dc_);
        }

        if (screen_) {
            ReleaseDC(nullptr, screen_);
        }
    }

    bool Init(const Fb& fb) {
        fb_ = fb;

        screen_ = GetDC(nullptr);
        if (!screen_) {
            return false;
        }

        dc_ = CreateCompatibleDC(screen_);
        if (!dc_) {
            return false;
        }

        BITMAPINFO info{};
        info.bmiHeader.biSize =
            sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth =
            fb.w;
        info.bmiHeader.biHeight =
            -fb.h; // top-down DIB
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        bitmap_ = CreateDIBSection(
            dc_,
            &info,
            DIB_RGB_COLORS,
            &bits_,
            nullptr,
            0);

        if (!bitmap_) {
            return false;
        }

        oldBitmap_ = SelectObject(
            dc_,
            bitmap_);

        if (!oldBitmap_ ||
            oldBitmap_ == HGDI_ERROR) {
            return false;
        }

        SetStretchBltMode(
            dc_,
            COLORONCOLOR);

        return true;
    }

    bool CaptureFrame(const RECT& source) {
        const int sourceWidth =
            source.right - source.left;
        const int sourceHeight =
            source.bottom - source.top;

        if (sourceWidth <= 0 ||
            sourceHeight <= 0) {
            return false;
        }

        const double sourceAspect =
            static_cast<double>(sourceWidth) /
            static_cast<double>(sourceHeight);

        const double targetAspect =
            static_cast<double>(fb_.w) /
            static_cast<double>(fb_.h);

        RECT destination{0, 0, fb_.w, fb_.h};

        if (sourceAspect > targetAspect) {
            destination.bottom =
                static_cast<LONG>(
                    static_cast<double>(fb_.w) /
                    sourceAspect +
                    0.5);

            destination.top =
                (fb_.h - destination.bottom) / 2;
        } else {
            destination.right =
                static_cast<LONG>(
                    static_cast<double>(fb_.h) *
                    sourceAspect +
                    0.5);

            destination.left =
                (fb_.w - destination.right) / 2;
        }

        const RECT fullTarget{
            0,
            0,
            fb_.w,
            fb_.h
        };

        FillRect(
            dc_,
            &fullTarget,
            static_cast<HBRUSH>(
                GetStockObject(BLACK_BRUSH)));

        return StretchBlt(
                   dc_,
                   destination.left,
                   destination.top,
                   destination.right -
                       destination.left,
                   destination.bottom -
                       destination.top,
                   screen_,
                   source.left,
                   source.top,
                   sourceWidth,
                   sourceHeight,
                   SRCCOPY) != FALSE;
    }

    const BYTE* Pixels() const {
        return static_cast<const BYTE*>(bits_);
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

class Stream {
    HINTERNET session_ = nullptr;
    HINTERNET connect_ = nullptr;
    HINTERNET request_ = nullptr;

public:
    ~Stream() {
        if (request_) {
            WinHttpCloseHandle(request_);
        }

        if (connect_) {
            WinHttpCloseHandle(connect_);
        }

        if (session_) {
            WinHttpCloseHandle(session_);
        }
    }

    bool Open(const std::wstring& baseUrl) {
        Url url;

        if (!Crack(baseUrl, url)) {
            return false;
        }

        session_ = WinHttpOpen(
            L"displayPost/0.5",
            WINHTTP_ACCESS_TYPE_NO_PROXY,
            nullptr,
            nullptr,
            0);

        if (!session_) {
            return false;
        }

        connect_ = WinHttpConnect(
            session_,
            url.host.c_str(),
            url.port,
            0);

        if (!connect_) {
            return false;
        }

        const std::wstring path =
            JoinPath(url.path, L"/fb");

        request_ = WinHttpOpenRequest(
            connect_,
            L"POST",
            path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            url.tls
                ? WINHTTP_FLAG_SECURE
                : 0);

        if (!request_) {
            return false;
        }

        // Disable HTTP/2 and HTTP/3 so this stays HTTP/1.1.
        DWORD enabledProtocols = 0;

        if (!WinHttpSetOption(
                request_,
                WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL,
                &enabledProtocols,
                sizeof(enabledProtocols))) {
            return false;
        }

        /*
            Match the working curl command.

            Important:
            do NOT add Transfer-Encoding: chunked here.
            With WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH,
            WinHTTP handles HTTP/1.1 chunk framing itself.
        */

        LPCWSTR headers =
            L"Content-Type: "
            L"multipart/x-mixed-replace; "
            L"boundary=ffmpeg\r\n"
            L"Expect:\r\n";

        return WinHttpSendRequest(
                   request_,
                   headers,
                   static_cast<DWORD>(-1),
                   nullptr,
                   0,
                   WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH,
                   0) != FALSE;
    }

    bool WriteFrame(
        const std::vector<BYTE>& jpeg) {

        if (jpeg.empty()) {
            return false;
        }

        /*
            Multipart frame format:

                --ffmpeg\r\n
                Content-type: image/jpeg\r\n
                Content-length: N\r\n
                \r\n
                JPEG bytes
                \r\n

            HTTP chunk framing is intentionally NOT
            added here. WinHTTP adds that transport
            framing itself.
        */

        const std::string header =
            "--ffmpeg\r\n"
            "Content-type: image/jpeg\r\n"
            "Content-length: " +
            std::to_string(jpeg.size()) +
            "\r\n"
            "\r\n";

        if (!WinHttpWriteData(
                request_,
                header.data(),
                static_cast<DWORD>(header.size()),
                nullptr)) {
            return false;
        }

        if (jpeg.size() >
            std::numeric_limits<DWORD>::max()) {
            return false;
        }

        if (!WinHttpWriteData(
                request_,
                jpeg.data(),
                static_cast<DWORD>(jpeg.size()),
                nullptr)) {
            return false;
        }

        static const char endOfFrame[] = "\r\n";

        return WinHttpWriteData(
                   request_,
                   endOfFrame,
                   sizeof(endOfFrame) - 1,
                   nullptr) != FALSE;
    }

    bool Finish() {
        /*
            For a finite stream, finish multipart first.
        */

        static const char endBoundary[] =
            "--ffmpeg--\r\n";

        if (!WinHttpWriteData(
                request_,
                endBoundary,
                sizeof(endBoundary) - 1,
                nullptr)) {
            return false;
        }

        /*
            With WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH,
            zero-length WriteData tells WinHTTP there
            is no more request-body data.
        */

        if (!WinHttpWriteData(
                request_,
                nullptr,
                0,
                nullptr)) {
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

    if (argc < 2) {
        std::wcerr
            << L"Usage: displayPost.exe "
            << L"<url> [frames=0] [monitor=1]\n";
        return 2;
    }

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
        return 1;
    }

    try {
        const std::wstring url = argv[1];

        const uint64_t frames =
            argc > 2
                ? ParseUnsigned(argv[2])
                : 0;

        const uint64_t monitorIndex =
            argc > 3
                ? ParseUnsigned(argv[3])
                : 1;

        if (monitorIndex == 0) {
            throw std::runtime_error(
                "monitor must be >= 1");
        }

        const Fb fb = ReadFbInfo(url);

        const std::vector<MonitorInfo> monitors =
            EnumerateMonitors();

        if (monitors.empty()) {
            throw std::runtime_error(
                "no Windows monitors found");
        }

        if (monitorIndex > monitors.size()) {
            throw std::runtime_error(
                "monitor out of range");
        }

        Capture capture;

        if (!capture.Init(fb)) {
            throw std::runtime_error(
                "capture initialization failed");
        }

        JpegEncoder encoder;
        Stream stream;

        if (!stream.Open(url)) {
            throw std::runtime_error(
                "failed to open /fb MJPEG stream");
        }

        std::vector<BYTE> jpeg;

        constexpr int FPS = 30;
        constexpr auto FRAME_INTERVAL =
            std::chrono::microseconds(1000000 / FPS);

        auto nextFrame =
            std::chrono::steady_clock::now();

        uint64_t sentFrames = 0;

        const RECT monitorRect =
            monitors[
                static_cast<size_t>(monitorIndex - 1)
            ].rect;

        while (!frames || sentFrames < frames) {
            if (!capture.CaptureFrame(monitorRect)) {
                throw std::runtime_error(
                    "capture frame failed");
            }

            if (!encoder.Encode(
                    capture.Pixels(),
                    capture.Width(),
                    capture.Height(),
                    capture.Stride(),
                    jpeg)) {
                throw std::runtime_error(
                    "JPEG encode failed");
            }

            if (!stream.WriteFrame(jpeg)) {
                throw std::runtime_error(
                    "MJPEG stream write failed at frame " +
                    std::to_string(sentFrames + 1));
            }

            ++sentFrames;

            nextFrame += FRAME_INTERVAL;
            std::this_thread::sleep_until(nextFrame);
        }

        if (frames != 0) {
            if (!stream.Finish()) {
                throw std::runtime_error(
                    "failed to finish /fb stream");
            }
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

        return 1;
    }
}
