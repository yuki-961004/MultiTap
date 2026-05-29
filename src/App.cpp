#include "AudioEngine.h"
#include "ComPtr.h"
#include "Config.h"
#include "DeviceEnumerator.h"

#include <Windows.h>
#include <Commctrl.h>
#include <Dwmapi.h>
#include <Endpointvolume.h>
#include <Mmdeviceapi.h>
#include <Objbase.h>
#include <Uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cwctype>
#include <memory>
#include <string>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' "\
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "\
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' "\
    "language='*'\"")

namespace {

// ############################
// Control IDs
// ############################

constexpr int kDeviceList = 1001;
constexpr int kStartButton = 1002;
constexpr int kStopButton = 1003;
constexpr int kRefreshButton = 1004;
constexpr int kTitleLabel = 1005;
constexpr int kSubtitleLabel = 1006;
constexpr int kThemeButton = 1007;
constexpr int kDeviceHeading = 1008;
constexpr int kStatusPanel = 1009;

constexpr UINT kStatusMessage = WM_APP + 1;
constexpr UINT kVolumeChangedMessage = WM_APP + 2;

constexpr int kDefaultWindowWidth = 640;
constexpr int kDefaultWindowHeight = 520;
constexpr int kMinimumWindowWidth = 560;
constexpr int kMinimumWindowHeight = 420;
constexpr int kDeviceRatioMaxPercent = 150;

constexpr int kDeviceRowHeight = 56;
constexpr int kDeviceRowGap = 8;
constexpr int kCardRadius = 8;

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif

enum DwmWindowCornerPreference {
    kDwmCornerDefault = 0,
    kDwmCornerDoNotRound = 1,
    kDwmCornerRound = 2,
    kDwmCornerRoundSmall = 3
};

enum DwmSystemBackdropType {
    kDwmBackdropAuto = 0,
    kDwmBackdropNone = 1,
    kDwmBackdropMainWindow = 2,
    kDwmBackdropTransientWindow = 3,
    kDwmBackdropTabbedWindow = 4
};

enum class StatusKind {
    Idle,
    Running,
    Error
};

class EndpointVolumeCallback final : public IAudioEndpointVolumeCallback {
public:
    explicit EndpointVolumeCallback(HWND hwnd) : hwnd_(hwnd) {}

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        LONG count = InterlockedDecrement(&refCount_);

        if (count == 0) {
            delete this;
        }

        return static_cast<ULONG>(count);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object)
        override {
        if (!object) {
            return E_POINTER;
        }

        if (iid == __uuidof(IUnknown) ||
            iid == __uuidof(IAudioEndpointVolumeCallback)) {
            *object = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef();
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnNotify(
        PAUDIO_VOLUME_NOTIFICATION_DATA data) override {
        if (!data || !IsWindow(hwnd_)) {
            return S_OK;
        }

        int percent = static_cast<int>(
            std::clamp(data->fMasterVolume, 0.0f, 1.0f) * 100.0f + 0.5f);
        PostMessageW(hwnd_, kVolumeChangedMessage,
            static_cast<WPARAM>(percent), 0);
        return S_OK;
    }

private:
    LONG refCount_ = 1;
    HWND hwnd_ = nullptr;
};

class SystemVolumeController {
public:
    ~SystemVolumeController() {
        Close();
    }

    bool Open(HWND hwnd) {
        Close();
        hwnd_ = hwnd;

        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(enumerator.put()));

        if (FAILED(hr)) {
            return false;
        }

        ComPtr<IMMDevice> endpoint;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
            endpoint.put());

        if (FAILED(hr)) {
            return false;
        }

        hr = endpoint->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
            nullptr, reinterpret_cast<void**>(endpointVolume_.put()));

        if (FAILED(hr)) {
            return false;
        }

        callback_ = new EndpointVolumeCallback(hwnd_);
        hr = endpointVolume_->RegisterControlChangeNotify(callback_);

        if (FAILED(hr)) {
            callback_->Release();
            callback_ = nullptr;
        }

        return true;
    }

    void Close() {
        if (endpointVolume_ && callback_) {
            endpointVolume_->UnregisterControlChangeNotify(callback_);
        }

        if (callback_) {
            callback_->Release();
            callback_ = nullptr;
        }

        endpointVolume_.reset();
        hwnd_ = nullptr;
    }

    bool GetVolumePercent(int* percent) {
        if (!endpointVolume_ || !percent) {
            return false;
        }

        float scalar = 1.0f;
        HRESULT hr = endpointVolume_->GetMasterVolumeLevelScalar(&scalar);

        if (FAILED(hr)) {
            return false;
        }

        *percent = static_cast<int>(
            std::clamp(scalar, 0.0f, 1.0f) * 100.0f + 0.5f);
        return true;
    }

    bool SetVolumePercent(int percent) {
        if (!endpointVolume_) {
            return false;
        }

        float scalar = static_cast<float>(
            std::clamp(percent, 0, 100)) / 100.0f;
        return SUCCEEDED(
            endpointVolume_->SetMasterVolumeLevelScalar(scalar, nullptr));
    }

private:
    HWND hwnd_ = nullptr;
    EndpointVolumeCallback* callback_ = nullptr;
    ComPtr<IAudioEndpointVolume> endpointVolume_;
};

struct Theme {
    COLORREF background;
    COLORREF surface;
    COLORREF surfaceHover;
    COLORREF text;
    COLORREF textMuted;
    COLORREF textDisabled;
    COLORREF border;
    COLORREF borderStrong;
    COLORREF accent;
    COLORREF accentPressed;
    COLORREF accentSoft;
    COLORREF button;
    COLORREF buttonPressed;
    COLORREF buttonDisabled;
    COLORREF warning;
    COLORREF error;
    COLORREF ok;
};

constexpr Theme kLightTheme = {
    RGB(246, 247, 249),
    RGB(255, 255, 255),
    RGB(249, 251, 253),
    RGB(32, 33, 36),
    RGB(94, 99, 104),
    RGB(145, 149, 155),
    RGB(222, 226, 232),
    RGB(197, 203, 211),
    RGB(0, 95, 184),
    RGB(0, 78, 154),
    RGB(232, 242, 253),
    RGB(255, 255, 255),
    RGB(242, 246, 251),
    RGB(237, 240, 244),
    RGB(143, 84, 0),
    RGB(196, 43, 28),
    RGB(16, 124, 16)
};

constexpr Theme kDarkTheme = {
    RGB(31, 32, 35),
    RGB(43, 44, 48),
    RGB(51, 53, 58),
    RGB(243, 243, 243),
    RGB(190, 193, 198),
    RGB(126, 130, 136),
    RGB(67, 70, 76),
    RGB(88, 93, 101),
    RGB(96, 174, 255),
    RGB(72, 151, 235),
    RGB(35, 62, 92),
    RGB(50, 52, 57),
    RGB(62, 65, 71),
    RGB(42, 44, 48),
    RGB(255, 196, 87),
    RGB(255, 107, 107),
    RGB(99, 203, 105)
};

struct AppState {
    HWND hwnd = nullptr;
    HWND deviceList = nullptr;
    HWND start = nullptr;
    HWND stop = nullptr;
    HWND refresh = nullptr;
    HWND themeButton = nullptr;
    HWND title = nullptr;
    HWND subtitle = nullptr;
    HWND deviceHeading = nullptr;
    HWND statusPanel = nullptr;

    std::unique_ptr<SystemVolumeController> volumeController;
    std::vector<PlaybackDeviceInfo> devices;
    std::vector<bool> selected;
    std::vector<int> deviceRatios;
    AudioEngine engine;

    UINT dpi = 96;
    bool dark = false;
    StatusKind statusKind = StatusKind::Idle;
    std::wstring statusDetail;
    int deviceScroll = 0;
    int hotDeviceIndex = -1;
    int focusDeviceIndex = -1;
    bool deviceThumbDragging = false;
    int deviceThumbDragOffset = 0;
    int activeDeviceSliderIndex = -1;
    bool deviceSliderDragging = false;
    bool volumeAvailable = false;
    bool volumeDragging = false;
    int systemVolumePercent = 100;

    HFONT titleFont = nullptr;
    HFONT headingFont = nullptr;
    HFONT bodyFont = nullptr;
    HFONT smallFont = nullptr;
    HBRUSH windowBrush = nullptr;
    HBRUSH surfaceBrush = nullptr;
};

void UpdateButtons(AppState* app);
void SaveCurrentDeviceConfigs(AppState* app);
void RefreshSystemVolume(AppState* app);

// ############################
// Theme and drawing helpers
// ############################

const Theme& CurrentTheme(AppState* app) {
    return app->dark ? kDarkTheme : kLightTheme;
}

void ResetBrush(HBRUSH& brush, COLORREF color) {
    // 如果旧画刷存在, 先释放它, 避免主题切换时泄漏 GDI 对象.
    if (brush) {
        DeleteObject(brush);
    }

    brush = CreateSolidBrush(color);
}

int ScaleForDpi(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

int Scale(AppState* app, int value) {
    return ScaleForDpi(value, app ? app->dpi : 96);
}

HFONT MakeFont(int pointSize, int weight, UINT dpi) {
    int height = -MulDiv(pointSize, static_cast<int>(dpi), 72);
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
}

void ReleaseFontObject(HFONT& font) {
    if (font) {
        DeleteObject(font);
        font = nullptr;
    }
}

void ApplyControlFonts(AppState* app) {
    SendMessageW(app->title, WM_SETFONT,
        reinterpret_cast<WPARAM>(app->titleFont), TRUE);
    SendMessageW(app->subtitle, WM_SETFONT,
        reinterpret_cast<WPARAM>(app->bodyFont), TRUE);
    SendMessageW(app->themeButton, WM_SETFONT,
        reinterpret_cast<WPARAM>(app->smallFont), TRUE);
    SendMessageW(app->deviceHeading, WM_SETFONT,
        reinterpret_cast<WPARAM>(app->headingFont), TRUE);
    SendMessageW(app->start, WM_SETFONT,
        reinterpret_cast<WPARAM>(app->bodyFont), TRUE);
    SendMessageW(app->stop, WM_SETFONT,
        reinterpret_cast<WPARAM>(app->bodyFont), TRUE);
    SendMessageW(app->refresh, WM_SETFONT,
        reinterpret_cast<WPARAM>(app->bodyFont), TRUE);
}

void RebuildFonts(AppState* app) {
    ReleaseFontObject(app->titleFont);
    ReleaseFontObject(app->headingFont);
    ReleaseFontObject(app->bodyFont);
    ReleaseFontObject(app->smallFont);

    app->titleFont = MakeFont(20, FW_SEMIBOLD, app->dpi);
    app->headingFont = MakeFont(10, FW_SEMIBOLD, app->dpi);
    app->bodyFont = MakeFont(9, FW_NORMAL, app->dpi);
    app->smallFont = MakeFont(8, FW_NORMAL, app->dpi);

    ApplyControlFonts(app);
}

int FontLineHeight(HWND hwnd, HFONT font) {
    HDC dc = GetDC(hwnd);
    HGDIOBJ oldFont = SelectObject(dc, font);
    TEXTMETRICW metrics = {};
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, oldFont);
    ReleaseDC(hwnd, dc);

    return metrics.tmHeight + metrics.tmExternalLeading;
}

int RectWidth(const RECT& rect) {
    return rect.right - rect.left;
}

int RectHeight(const RECT& rect) {
    return rect.bottom - rect.top;
}

void FillRoundedRect(HDC dc, const RECT& rect, int radius, COLORREF fill,
    COLORREF border, int borderWidth = 1) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, borderWidth, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);

    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius,
        radius);

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void DrawTextLine(HDC dc, HFONT font, COLORREF color,
    const std::wstring& text, RECT rect, UINT flags) {
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, &rect, flags);
    SelectObject(dc, oldFont);
}

template <typename PaintFunc>
void PaintBuffered(HWND hwnd, HDC target, PaintFunc paintFunc) {
    RECT client = {};
    GetClientRect(hwnd, &client);

    int width = RectWidth(client);
    int height = RectHeight(client);

    if (width <= 0 || height <= 0) {
        return;
    }

    HDC memoryDc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);

    paintFunc(memoryDc);

    BitBlt(target, 0, 0, width, height, memoryDc, 0, 0, SRCCOPY);

    SelectObject(memoryDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
}

void RedrawWholeWindow(AppState* app) {
    if (!app || !app->hwnd) {
        return;
    }

    RedrawWindow(app->hwnd, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

bool StartsWith(const std::wstring& text, const std::wstring& prefix) {
    return text.rfind(prefix, 0) == 0;
}

std::wstring ToLower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
        [](wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });
    return text;
}

bool ContainsText(const std::wstring& text, const std::wstring& needle) {
    return ToLower(text).find(ToLower(needle)) != std::wstring::npos;
}

// ############################
// Device metadata helpers
// ############################

bool DeviceUsesBluetooth(const PlaybackDeviceInfo& device) {
    // 蓝牙设备通常在友好名称中包含 bluetooth, 这里用于状态区延迟提示.
    return ContainsText(device.name, L"bluetooth");
}

std::wstring DeviceDetailLabel(const PlaybackDeviceInfo& device) {
    if (device.isDefault) {
        return L"System playback source";
    }

    if (DeviceUsesBluetooth(device)) {
        return L"Wireless audio device";
    }

    if (ContainsText(device.name, L"hdmi") ||
        ContainsText(device.name, L"display")) {
        return L"Display audio endpoint";
    }

    return L"Windows audio device";
}

std::wstring DeviceSecondaryLabel(const PlaybackDeviceInfo& device) {
    // 默认输出设备需要明确标记, 但不把说明塞进主设备名称.
    if (device.isDefault) {
        return L"Default output";
    }

    // 这些标签是轻量推断, 只用于让列表比原始设备名更容易扫读.
    if (DeviceUsesBluetooth(device)) {
        return L"Bluetooth";
    }

    if (ContainsText(device.name, L"headphones") ||
        ContainsText(device.name, L"headset")) {
        return L"Headphones";
    }

    if (ContainsText(device.name, L"speaker") ||
        ContainsText(device.name, L"speakers") ||
        ContainsText(device.name, L"realtek")) {
        return L"Speakers";
    }

    if (ContainsText(device.name, L"hdmi") ||
        ContainsText(device.name, L"display") ||
        ContainsText(device.name, L"nvidia") ||
        ContainsText(device.name, L"amd")) {
        return L"Display audio";
    }

    return L"Windows audio device";
}

int SelectedDeviceCount(AppState* app) {
    int count = 0;

    // 遍历选择状态, 让按钮状态和状态区始终与列表保持一致.
    for (bool selected : app->selected) {
        if (selected) {
            ++count;
        }
    }

    return count;
}

int DeviceRatioPercent(AppState* app, size_t index) {
    if (!app || index >= app->deviceRatios.size()) {
        return 100;
    }

    return std::clamp(app->deviceRatios[index], 0, kDeviceRatioMaxPercent);
}

void SetDeviceRatioPercent(AppState* app, int index, int percent,
    bool saveConfig) {
    if (!app || index < 0 ||
        index >= static_cast<int>(app->devices.size())) {
        return;
    }

    int next = std::clamp(percent, 0, kDeviceRatioMaxPercent);
    if (index >= static_cast<int>(app->deviceRatios.size())) {
        app->deviceRatios.resize(app->devices.size(), 100);
    }

    app->deviceRatios[static_cast<size_t>(index)] = next;

    if (app->engine.IsRunning()) {
        app->engine.SetDeviceVolume(app->devices[static_cast<size_t>(index)].id,
            static_cast<float>(next) / 100.0f);
    }

    if (saveConfig) {
        SaveCurrentDeviceConfigs(app);
    }

    if (app->deviceList) {
        InvalidateRect(app->deviceList, nullptr, TRUE);
    }
}

bool HasSelectedBluetooth(AppState* app) {
    // 只有被选中的蓝牙设备才需要提示用户可能存在延迟.
    for (size_t i = 0; i < app->devices.size() && i < app->selected.size();
         ++i) {
        if (app->selected[i] && DeviceUsesBluetooth(app->devices[i])) {
            return true;
        }
    }

    return false;
}

std::vector<std::wstring> CheckedDeviceIds(AppState* app) {
    std::vector<std::wstring> ids;

    // 保存设备 ID 而不是行号, 因为刷新设备后排序和数量都可能变化.
    for (size_t i = 0; i < app->devices.size() && i < app->selected.size();
         ++i) {
        if (app->selected[i]) {
            ids.push_back(app->devices[i].id);
        }
    }

    return ids;
}

std::vector<OutputSelection> CheckedDevices(AppState* app) {
    std::vector<OutputSelection> devices;

    // 每个输出都带上自己的相对比例, 这样系统音量变化时会一起缩放.
    for (size_t i = 0; i < app->devices.size() && i < app->selected.size();
         ++i) {
        if (app->selected[i]) {
            int percent = i < app->deviceRatios.size()
                ? app->deviceRatios[i]
                : 100;
            devices.push_back({
                app->devices[i].id,
                std::clamp(static_cast<float>(percent) / 100.0f, 0.0f,
                    static_cast<float>(kDeviceRatioMaxPercent) / 100.0f)
            });
        }
    }

    return devices;
}

std::vector<DeviceConfig> CurrentDeviceConfigs(AppState* app) {
    std::vector<DeviceConfig> configs;

    // 配置仍然写回同一格式, 只是 volume 现在代表相对比例.
    for (size_t i = 0; i < app->devices.size() && i < app->selected.size();
         ++i) {
        int percent = i < app->deviceRatios.size()
            ? app->deviceRatios[i]
            : 100;
        configs.push_back({
            app->devices[i].id,
            app->selected[i],
            std::clamp(static_cast<float>(percent) / 100.0f, 0.0f,
                static_cast<float>(kDeviceRatioMaxPercent) / 100.0f)
        });
    }

    return configs;
}

void SaveCurrentDeviceConfigs(AppState* app) {
    SaveDeviceConfigs(CurrentDeviceConfigs(app));
}

void PromoteSelectedDevices(AppState* app) {
    if (!app) {
        return;
    }

    std::vector<PlaybackDeviceInfo> orderedDevices;
    std::vector<bool> orderedSelected;
    std::vector<int> orderedRatios;
    orderedDevices.reserve(app->devices.size());
    orderedSelected.reserve(app->selected.size());
    orderedRatios.reserve(app->deviceRatios.size());

    for (size_t i = 0; i < app->devices.size() && i < app->selected.size();
         ++i) {
        if (app->selected[i]) {
            orderedDevices.push_back(app->devices[i]);
            orderedSelected.push_back(true);
            orderedRatios.push_back(
                i < app->deviceRatios.size() ? app->deviceRatios[i] : 100);
        }
    }

    for (size_t i = 0; i < app->devices.size() && i < app->selected.size();
         ++i) {
        if (!app->selected[i]) {
            orderedDevices.push_back(app->devices[i]);
            orderedSelected.push_back(false);
            orderedRatios.push_back(
                i < app->deviceRatios.size() ? app->deviceRatios[i] : 100);
        }
    }

    app->devices = std::move(orderedDevices);
    app->selected = std::move(orderedSelected);
    app->deviceRatios = std::move(orderedRatios);
}

// ############################
// Status helpers
// ############################

const wchar_t* StatusText(StatusKind kind) {
    switch (kind) {
    case StatusKind::Running:
        return L"Running";
    case StatusKind::Error:
        return L"Error";
    case StatusKind::Idle:
    default:
        return L"Idle";
    }
}

void InvalidateStatus(AppState* app) {
    if (app && app->statusPanel) {
        InvalidateRect(app->statusPanel, nullptr, TRUE);
    }
}

void SetStatus(AppState* app, const std::wstring& text) {
    // 空状态没有诊断价值, 所以界面退回到 Idle.
    if (text.empty() || text == L"Idle") {
        app->statusKind = StatusKind::Idle;
        app->statusDetail.clear();
        InvalidateStatus(app);
        return;
    }

    // 音频线程用 Error 前缀上报失败, 设备断开也按错误展示.
    if (StartsWith(text, L"Error") ||
        StartsWith(text, L"Cannot") ||
        StartsWith(text, L"Device disconnected") ||
        StartsWith(text, L"Already") ||
        StartsWith(text, L"Select ")) {
        app->statusKind = StatusKind::Error;
        app->statusDetail = text;
        InvalidateStatus(app);
        return;
    }

    // Running 后面的补充说明保留在第二行, 不污染主状态字段.
    if (StartsWith(text, L"Running")) {
        app->statusKind = StatusKind::Running;
        app->statusDetail.clear();

        if (StartsWith(text, L"Running: ")) {
            app->statusDetail = text.substr(9);
        }

        InvalidateStatus(app);
        return;
    }

    // Starting 和 Stopping 都是运行过程中的短暂状态, 主状态保持 Running.
    if (StartsWith(text, L"Starting") || StartsWith(text, L"Stopping")) {
        app->statusKind = StatusKind::Running;
        app->statusDetail.clear();
        InvalidateStatus(app);
        return;
    }

    app->statusKind = StatusKind::Error;
    app->statusDetail = text;
    InvalidateStatus(app);
}

// ############################
// Device list drawing
// ############################

int DeviceRowHeight(AppState* app) {
    return Scale(app, 84);
}

int DeviceRowExpandedHeight(AppState* app) {
    return Scale(app, 140);
}

int DeviceRowGap(AppState* app) {
    return Scale(app, kDeviceRowGap);
}

int DeviceScrollbarWidth(AppState* app) {
    return Scale(app, 10);
}

int DeviceContentHeight(AppState* app) {
    if (app->devices.empty()) {
        return 0;
    }

    int total = 0;

    for (size_t i = 0; i < app->devices.size(); ++i) {
        total += app->selected.size() > i && app->selected[i]
            ? DeviceRowExpandedHeight(app)
            : DeviceRowHeight(app);

        if (i + 1 < app->devices.size()) {
            total += DeviceRowGap(app);
        }
    }

    return total;
}

bool DeviceNeedsScroll(AppState* app) {
    RECT rect = {};
    GetClientRect(app->deviceList, &rect);

    return DeviceContentHeight(app) > RectHeight(rect);
}

int MaxDeviceScroll(AppState* app) {
    RECT rect = {};
    GetClientRect(app->deviceList, &rect);

    return std::max(0, DeviceContentHeight(app) - RectHeight(rect));
}

int DeviceItemHeight(AppState* app, size_t index) {
    if (index < app->selected.size() && app->selected[index]) {
        return DeviceRowExpandedHeight(app);
    }

    return DeviceRowHeight(app);
}

int DeviceItemTop(AppState* app, size_t index) {
    int top = 0;

    for (size_t i = 0; i < index && i < app->devices.size(); ++i) {
        top += DeviceItemHeight(app, i);

        if (i + 1 <= index) {
            top += DeviceRowGap(app);
        }
    }

    return top;
}

int DeviceItemBottom(AppState* app, size_t index) {
    return DeviceItemTop(app, index) + DeviceItemHeight(app, index);
}

void UpdateDeviceScrollBar(AppState* app) {
    if (!app || !app->deviceList) {
        return;
    }

    RECT rect = {};
    GetClientRect(app->deviceList, &rect);

    int contentHeight = DeviceContentHeight(app);
    int pageHeight = std::max(1, RectHeight(rect));
    int maxScroll = std::max(0, contentHeight - pageHeight);

    // 自绘滚动条在客户区内绘制, 避免原生非客户区滚动条留下脏区.
    app->deviceScroll = std::clamp(app->deviceScroll, 0, maxScroll);
    InvalidateRect(app->deviceList, nullptr, TRUE);
}

void SetDeviceScroll(AppState* app, int requestedScroll) {
    int nextScroll = std::clamp(requestedScroll, 0, MaxDeviceScroll(app));

    if (nextScroll == app->deviceScroll) {
        return;
    }

    app->deviceScroll = nextScroll;
    UpdateDeviceScrollBar(app);
    InvalidateRect(app->deviceList, nullptr, TRUE);
}

void EnsureDeviceVisible(AppState* app, int index) {
    if (index < 0 || index >= static_cast<int>(app->devices.size())) {
        return;
    }

    RECT rect = {};
    GetClientRect(app->deviceList, &rect);

    int itemTop = DeviceItemTop(app, static_cast<size_t>(index));
    int itemBottom = DeviceItemBottom(app, static_cast<size_t>(index));

    // 焦点行在可视区域上方时向上滚动.
    if (itemTop < app->deviceScroll) {
        SetDeviceScroll(app, itemTop);
        return;
    }

    // 焦点行在可视区域下方时向下滚动.
    if (itemBottom > app->deviceScroll + RectHeight(rect)) {
        SetDeviceScroll(app, itemBottom - RectHeight(rect));
    }
}

int DeviceIndexFromPoint(AppState* app, int y) {
    int contentY = y + app->deviceScroll;
    int top = 0;

    for (size_t i = 0; i < app->devices.size(); ++i) {
        int itemHeight = DeviceItemHeight(app, i);
        int bottom = top + itemHeight;

        if (contentY >= top && contentY < bottom) {
            return static_cast<int>(i);
        }

        top = bottom + DeviceRowGap(app);
    }

    return -1;
}

RECT DeviceItemRect(AppState* app, size_t index) {
    RECT client = {};
    GetClientRect(app->deviceList, &client);

    int top = DeviceItemTop(app, index) - app->deviceScroll;
    int height = DeviceItemHeight(app, index);

    return RECT{
        client.left + Scale(app, 1),
        top,
        client.right - Scale(app, 1),
        top + height
    };
}

RECT DeviceSliderTrackRect(AppState* app, size_t index) {
    RECT row = DeviceItemRect(app, index);
    int padding = Scale(app, 16);
    int labelWidth = Scale(app, 128);
    int sliderHeight = Scale(app, 6);
    int trackTop = row.bottom - Scale(app, 28);

    return RECT{
        row.left + padding + labelWidth,
        trackTop,
        row.right - padding,
        trackTop + sliderHeight
    };
}

RECT DeviceSliderThumbRect(AppState* app, size_t index) {
    RECT track = DeviceSliderTrackRect(app, index);
    int thumbSize = Scale(app, 16);
    int trackWidth = std::max(1, RectWidth(track));
    int percent = DeviceRatioPercent(app, index);
    int x = track.left + MulDiv(percent, trackWidth, kDeviceRatioMaxPercent);
    int centerY = track.top + RectHeight(track) / 2;

    return RECT{
        x - thumbSize / 2,
        centerY - thumbSize / 2,
        x + thumbSize / 2,
        centerY + thumbSize / 2
    };
}

bool PointInDeviceSlider(AppState* app, int index, POINT point) {
    RECT track = DeviceSliderTrackRect(app, static_cast<size_t>(index));
    RECT thumb = DeviceSliderThumbRect(app, static_cast<size_t>(index));
    InflateRect(&track, Scale(app, 6), Scale(app, 12));
    InflateRect(&thumb, Scale(app, 4), Scale(app, 4));
    return PtInRect(&track, point) || PtInRect(&thumb, point);
}

int DeviceRatioPercentFromX(AppState* app, size_t index, int x) {
    RECT track = DeviceSliderTrackRect(app, index);
    int trackLeft = static_cast<int>(track.left);
    int trackRight = static_cast<int>(track.right);
    int width = std::max(1, trackRight - trackLeft);
    int clampedX = std::clamp(x, trackLeft, trackRight);
    return std::clamp(MulDiv(clampedX - trackLeft, kDeviceRatioMaxPercent,
        width), 0, kDeviceRatioMaxPercent);
}

void UpdateDeviceSliderFromPoint(AppState* app, int index, int x) {
    if (!app || index < 0) {
        return;
    }

    int percent = DeviceRatioPercentFromX(app, static_cast<size_t>(index), x);
    SetDeviceRatioPercent(app, index, percent, true);
}

RECT DeviceThumbRect(AppState* app) {
    RECT client = {};
    GetClientRect(app->deviceList, &client);

    RECT empty = {};
    int contentHeight = DeviceContentHeight(app);
    int pageHeight = RectHeight(client);

    if (contentHeight <= pageHeight || pageHeight <= 0) {
        return empty;
    }

    int trackInset = Scale(app, 4);
    int trackTop = client.top + trackInset;
    int trackBottom = client.bottom - trackInset;
    int trackHeight = std::max(1, trackBottom - trackTop);
    int thumbHeight = std::max(Scale(app, 28),
        MulDiv(pageHeight, trackHeight, contentHeight));
    thumbHeight = std::min(thumbHeight, trackHeight);

    int maxScroll = std::max(1, contentHeight - pageHeight);
    int thumbTravel = std::max(1, trackHeight - thumbHeight);
    int thumbTop = trackTop + MulDiv(app->deviceScroll, thumbTravel,
        maxScroll);

    return RECT{
        client.right - DeviceScrollbarWidth(app) + Scale(app, 2),
        thumbTop,
        client.right - Scale(app, 3),
        thumbTop + thumbHeight
    };
}

void DrawDeviceScrollbar(AppState* app, HDC dc) {
    if (!DeviceNeedsScroll(app)) {
        return;
    }

    const Theme& theme = CurrentTheme(app);
    RECT thumb = DeviceThumbRect(app);

    FillRoundedRect(dc, thumb, Scale(app, 4), theme.borderStrong,
        theme.borderStrong);
}

void DrawCheckbox(HDC dc, const RECT& rect, bool checked, bool enabled,
    const Theme& theme) {
    COLORREF fill = checked ? theme.accent : theme.surface;
    COLORREF border = checked ? theme.accent : theme.borderStrong;

    if (!enabled) {
        fill = checked ? theme.textDisabled : theme.buttonDisabled;
        border = theme.border;
    }

    FillRoundedRect(dc, rect, std::max(4, RectHeight(rect) / 4), fill,
        border);

    if (!checked) {
        return;
    }

    COLORREF checkColor = enabled ? RGB(250, 250, 250) : theme.surface;
    int width = RectWidth(rect);
    int height = RectHeight(rect);
    HPEN pen = CreatePen(PS_SOLID, std::max(2, width / 8), checkColor);
    HGDIOBJ oldPen = SelectObject(dc, pen);

    MoveToEx(dc, rect.left + width / 4, rect.top + height / 2,
        nullptr);
    LineTo(dc, rect.left + width / 2 - 1, rect.bottom - height / 4);
    LineTo(dc, rect.right - width / 5, rect.top + height / 4);

    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawDevicePill(HDC dc, AppState* app, const std::wstring& text,
    bool isDefault, bool enabled, RECT row) {
    const Theme& theme = CurrentTheme(app);
    SIZE textSize = {};

    HGDIOBJ oldFont = SelectObject(dc, app->smallFont);
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()),
        &textSize);

    int maxWidth = std::max(Scale(app, 96),
        std::min(Scale(app, 150), RectWidth(row) / 3));
    int pillTextWidth = static_cast<int>(textSize.cx);
    int pillWidth = std::min(maxWidth, pillTextWidth + Scale(app, 18));
    int pillHeight = FontLineHeight(app->deviceList, app->smallFont) +
        Scale(app, 8);
    int pillTop = row.top + (RectHeight(row) - pillHeight) / 2;
    RECT pill = {
        row.right - pillWidth - Scale(app, 14),
        pillTop,
        row.right - Scale(app, 14),
        pillTop + pillHeight
    };

    COLORREF fill = isDefault ? theme.accentSoft : theme.surfaceHover;
    COLORREF border = isDefault ? theme.accent : theme.border;
    COLORREF color = isDefault ? theme.accent : theme.textMuted;

    if (!enabled) {
        fill = theme.buttonDisabled;
        border = theme.border;
        color = theme.textDisabled;
    }

    FillRoundedRect(dc, pill, Scale(app, 8), fill, border);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, &pill,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
}

void DrawRatioSlider(AppState* app, HDC dc, size_t index, RECT row,
    bool enabled) {
    const Theme& theme = CurrentTheme(app);
    RECT track = DeviceSliderTrackRect(app, index);
    RECT thumb = DeviceSliderThumbRect(app, index);
    int percent = DeviceRatioPercent(app, index);

    RECT label = {
        row.left + Scale(app, 48),
        track.top - Scale(app, 20),
        row.left + Scale(app, 180),
        track.top + Scale(app, 2)
    };

    std::wstring text = L"Relative volume: " +
        std::to_wstring(percent) + L"%";

    COLORREF labelColor = enabled ? theme.textMuted : theme.textDisabled;
    DrawTextLine(dc, app->smallFont, labelColor, text, label,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    COLORREF rail = app->dark ? RGB(76, 79, 86) : RGB(217, 223, 230);
    COLORREF fill = enabled ? theme.accent : theme.textDisabled;
    COLORREF thumbFill = enabled ? (app->dark ? RGB(246, 247, 250) :
        RGB(255, 255, 255)) : theme.buttonDisabled;

    RECT fillRect = track;
    fillRect.right = track.left + MulDiv(percent, RectWidth(track),
        kDeviceRatioMaxPercent);

    FillRoundedRect(dc, track, Scale(app, 4), rail, rail);
    FillRoundedRect(dc, fillRect, Scale(app, 4), fill, fill);
    FillRoundedRect(dc, thumb, Scale(app, 8), thumbFill, theme.accent);
}

void DrawDeviceList(AppState* app, HDC dc) {
    const Theme& theme = CurrentTheme(app);
    RECT client = {};
    GetClientRect(app->deviceList, &client);

    FillRect(dc, &client, app->windowBrush);

    if (app->devices.empty()) {
        RECT emptyRect = client;
        DrawTextLine(dc, app->bodyFont, theme.textMuted,
            L"No active output devices found.", emptyRect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    bool enabled = IsWindowEnabled(app->deviceList) != FALSE;
    int rightInset = DeviceNeedsScroll(app) ? DeviceScrollbarWidth(app) : 0;
    int checkboxSize = Scale(app, 18);
    int checkboxLeft = Scale(app, 16);
    int textLeft = Scale(app, 48);

    // 只绘制可见行, 避免设备很多时产生无意义的 GDI 绘制.
    for (int i = 0; i < static_cast<int>(app->devices.size()); ++i) {
        int index = static_cast<size_t>(i);
        RECT row = DeviceItemRect(app, index);

        if (row.top > client.bottom || row.bottom < client.top) {
            continue;
        }

        row.right -= rightInset;

        bool checked = i < static_cast<int>(app->selected.size()) &&
            app->selected[static_cast<size_t>(i)];
        bool hot = i == app->hotDeviceIndex && enabled;
        bool focused = i == app->focusDeviceIndex &&
            GetFocus() == app->deviceList;
        bool sliderHot = i == app->activeDeviceSliderIndex;

        COLORREF fill = hot ? theme.surfaceHover : theme.surface;
        COLORREF border = checked ? theme.accent : theme.border;
        int borderWidth = focused || sliderHot ? 2 : 1;

        if (!enabled) {
            fill = theme.surface;
            border = theme.border;
        }

        FillRoundedRect(dc, row, Scale(app, kCardRadius), fill, border,
            borderWidth);

        int rowHeight = DeviceItemHeight(app, index);
        int checkboxTop = row.top +
            std::max(Scale(app, 18), (rowHeight - checkboxSize) / 2);
        RECT checkbox = {
            row.left + checkboxLeft,
            checkboxTop,
            row.left + checkboxLeft + checkboxSize,
            checkboxTop + checkboxSize
        };
        DrawCheckbox(dc, checkbox, checked, enabled, theme);

        std::wstring label = DeviceSecondaryLabel(app->devices[i]);
        DrawDevicePill(dc, app, label, app->devices[i].isDefault, enabled,
            row);

        int labelReserve = std::max(Scale(app, 128),
            std::min(Scale(app, 180), RectWidth(row) / 3));
        int nameHeight = FontLineHeight(app->deviceList, app->bodyFont) +
            Scale(app, 2);
        int detailHeight = FontLineHeight(app->deviceList, app->smallFont) +
            Scale(app, 1);
        int textTop = row.top + Scale(app, 12);
        int textRight = std::max(row.left + textLeft + Scale(app, 60),
            row.right - labelReserve - Scale(app, 20));
        RECT nameRect = {
            row.left + textLeft,
            textTop,
            textRight,
            textTop + nameHeight
        };

        RECT typeRect = {
            row.left + textLeft,
            textTop + nameHeight,
            row.right - Scale(app, 20),
            textTop + nameHeight + detailHeight
        };

        COLORREF titleColor = enabled ? theme.text : theme.textDisabled;
        COLORREF detailColor = enabled ? theme.textMuted : theme.textDisabled;

        DrawTextLine(dc, app->bodyFont, titleColor, app->devices[i].name,
            nameRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE |
            DT_END_ELLIPSIS);
        DrawTextLine(dc, app->smallFont, detailColor,
            DeviceDetailLabel(app->devices[i]), typeRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (checked) {
            DrawRatioSlider(app, dc, index, row, enabled);
        }
    }

    DrawDeviceScrollbar(app, dc);
}

void ToggleDevice(AppState* app, int index) {
    if (app->engine.IsRunning()) {
        return;
    }

    if (index < 0 || index >= static_cast<int>(app->selected.size())) {
        return;
    }

    app->selected[static_cast<size_t>(index)] =
        !app->selected[static_cast<size_t>(index)];
    app->focusDeviceIndex = index;

    SaveCurrentDeviceConfigs(app);
    UpdateButtons(app);
    InvalidateRect(app->deviceList, nullptr, TRUE);
    InvalidateStatus(app);
}

// ############################
// Button drawing
// ############################

void DrawThemeButton(AppState* app, const DRAWITEMSTRUCT* item) {
    const Theme& theme = CurrentTheme(app);
    RECT rect = item->rcItem;
    bool disabled = (item->itemState & ODS_DISABLED) != 0;
    bool pressed = (item->itemState & ODS_SELECTED) != 0;
    bool focused = (item->itemState & ODS_FOCUS) != 0;
    bool hovered = (item->itemState & ODS_HOTLIGHT) != 0;

    FillRect(item->hDC, &rect, app->windowBrush);
    InflateRect(&rect, -1, -1);

    COLORREF fill = theme.button;
    COLORREF border = theme.border;
    if (pressed && !disabled) {
        fill = theme.buttonPressed;
    } else if (hovered && !disabled) {
        fill = theme.surfaceHover;
    }

    if (focused && !disabled) {
        border = theme.accent;
    }

    if (disabled) {
        fill = theme.buttonDisabled;
        border = theme.border;
    }

    FillRoundedRect(item->hDC, rect, Scale(app, 10), fill, border);

    std::wstring icon = app->dark ? L"\u263E" : L"\u2600";
    RECT iconRect = rect;
    DrawTextLine(item->hDC, app->smallFont,
        disabled ? theme.textDisabled : theme.accent, icon, iconRect,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawModernButton(AppState* app, const DRAWITEMSTRUCT* item) {
    if (item->CtlID == kThemeButton) {
        DrawThemeButton(app, item);
        return;
    }

    const Theme& theme = CurrentTheme(app);
    bool disabled = (item->itemState & ODS_DISABLED) != 0;
    bool pressed = (item->itemState & ODS_SELECTED) != 0;
    bool focused = (item->itemState & ODS_FOCUS) != 0;
    bool primary = item->CtlID == kStartButton;

    RECT rect = item->rcItem;
    FillRect(item->hDC, &rect, app->windowBrush);
    InflateRect(&rect, -1, -1);

    COLORREF fill = primary ? theme.accent : theme.button;
    COLORREF border = primary ? theme.accent : theme.border;
    COLORREF text = primary ? RGB(250, 250, 250) : theme.text;

    if (pressed && !disabled) {
        fill = primary ? theme.accentPressed : theme.buttonPressed;
    }

    if (disabled) {
        fill = theme.buttonDisabled;
        border = theme.border;
        text = theme.textDisabled;
    }

    if (focused && !disabled) {
        border = theme.accent;
    }

    FillRoundedRect(item->hDC, rect, Scale(app, 8), fill, border,
        focused ? 2 : 1);

    wchar_t label[128] = {};
    GetWindowTextW(item->hwndItem, label, static_cast<int>(_countof(label)));

    DrawTextLine(item->hDC, app->bodyFont, text, label, rect,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// ############################
// Status panel drawing
// ############################

COLORREF StatusColor(AppState* app) {
    const Theme& theme = CurrentTheme(app);

    switch (app->statusKind) {
    case StatusKind::Running:
        return theme.ok;
    case StatusKind::Error:
        return theme.error;
    case StatusKind::Idle:
    default:
        return theme.textMuted;
    }
}

std::wstring StatusSummary(AppState* app) {
    std::wstring summary = L"Status: ";
    summary += StatusText(app->statusKind);
    summary += L" - Selected devices: ";
    summary += std::to_wstring(SelectedDeviceCount(app));
    return summary;
}

std::wstring StatusDetail(AppState* app) {
    if (!app->statusDetail.empty()) {
        return app->statusDetail;
    }

    if (HasSelectedBluetooth(app)) {
        return L"Bluetooth devices may add latency.";
    }

    if (SelectedDeviceCount(app) == 0) {
        return L"No output devices selected.";
    }

    return L"Ready to duplicate audio to selected outputs.";
}

std::wstring SystemVolumeLabel(AppState* app) {
    if (!app->volumeAvailable) {
        return L"System volume unavailable";
    }

    return L"System volume: " +
        std::to_wstring(app->systemVolumePercent) + L"%";
}

RECT VolumeTrackRect(AppState* app) {
    RECT client = {};
    GetClientRect(app->statusPanel, &client);

    int padding = Scale(app, 16);
    int labelWidth = Scale(app, 142);
    int trackTop = client.bottom - Scale(app, 24);
    int trackHeight = Scale(app, 6);

    return RECT{
        client.left + padding + labelWidth,
        trackTop,
        client.right - padding,
        trackTop + trackHeight
    };
}

RECT VolumeThumbRect(AppState* app) {
    RECT track = VolumeTrackRect(app);
    int thumbSize = Scale(app, 16);
    int trackWidth = std::max(1, RectWidth(track));
    int thumbX = track.left + MulDiv(app->systemVolumePercent, trackWidth,
        100);
    int centerY = track.top + RectHeight(track) / 2;

    return RECT{
        thumbX - thumbSize / 2,
        centerY - thumbSize / 2,
        thumbX + thumbSize / 2,
        centerY + thumbSize / 2
    };
}

int VolumePercentFromX(AppState* app, int x) {
    RECT track = VolumeTrackRect(app);
    int width = std::max(1, RectWidth(track));
    int trackLeft = static_cast<int>(track.left);
    int trackRight = static_cast<int>(track.right);
    int clampedX = std::clamp(x, trackLeft, trackRight);

    return std::clamp(MulDiv(clampedX - trackLeft, 100, width), 0, 100);
}

bool PointInVolumeSlider(AppState* app, POINT point) {
    RECT track = VolumeTrackRect(app);
    RECT thumb = VolumeThumbRect(app);
    InflateRect(&track, Scale(app, 6), Scale(app, 10));
    InflateRect(&thumb, Scale(app, 4), Scale(app, 4));

    return PtInRect(&track, point) || PtInRect(&thumb, point);
}

void SetSystemVolumeFromPoint(AppState* app, int x) {
    if (!app || !app->volumeAvailable || !app->volumeController) {
        return;
    }

    int next = VolumePercentFromX(app, x);
    app->systemVolumePercent = next;

    if (!app->volumeController->SetVolumePercent(next)) {
        app->volumeAvailable = false;
    }

    InvalidateStatus(app);
}

void RefreshSystemVolume(AppState* app) {
    if (!app || !app->hwnd) {
        return;
    }

    if (!app->volumeController) {
        app->volumeController =
            std::make_unique<SystemVolumeController>();
    }

    app->volumeAvailable = app->volumeController->Open(app->hwnd);

    int percent = app->systemVolumePercent;

    if (app->volumeAvailable &&
        app->volumeController->GetVolumePercent(&percent)) {
        app->systemVolumePercent = percent;
    } else {
        app->volumeAvailable = false;
    }

    InvalidateStatus(app);
}

void DrawStatusPanel(AppState* app, HDC dc) {
    const Theme& theme = CurrentTheme(app);
    RECT client = {};
    GetClientRect(app->statusPanel, &client);

    FillRect(dc, &client, app->windowBrush);

    RECT panel = client;
    InflateRect(&panel, -1, -1);
    FillRoundedRect(dc, panel, Scale(app, kCardRadius), theme.surface,
        theme.border);

    int padding = Scale(app, 16);
    int summaryHeight = FontLineHeight(app->statusPanel, app->bodyFont) +
        Scale(app, 2);
    int detailHeight = FontLineHeight(app->statusPanel, app->smallFont) +
        Scale(app, 1);
    RECT dot = {
        panel.left + padding,
        panel.top + Scale(app, 15),
        panel.left + padding + Scale(app, 10),
        panel.top + Scale(app, 25)
    };
    HBRUSH dotBrush = CreateSolidBrush(StatusColor(app));
    HPEN dotPen = CreatePen(PS_NULL, 0, 0);
    HGDIOBJ oldBrush = SelectObject(dc, dotBrush);
    HGDIOBJ oldPen = SelectObject(dc, dotPen);
    Ellipse(dc, dot.left, dot.top, dot.right, dot.bottom);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(dotBrush);
    DeleteObject(dotPen);

    RECT summaryRect = {
        panel.left + Scale(app, 36),
        panel.top + Scale(app, 8),
        panel.right - padding,
        panel.top + Scale(app, 8) + summaryHeight
    };
    RECT detailRect = {
        panel.left + Scale(app, 36),
        summaryRect.bottom,
        panel.right - padding,
        summaryRect.bottom + detailHeight
    };

    COLORREF detailColor = app->statusKind == StatusKind::Error
        ? theme.error
        : theme.textMuted;

    if (app->statusDetail.empty() && HasSelectedBluetooth(app)) {
        detailColor = theme.warning;
    }

    DrawTextLine(dc, app->bodyFont, theme.text, StatusSummary(app),
        summaryRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE |
        DT_END_ELLIPSIS);
    DrawTextLine(dc, app->smallFont, detailColor, StatusDetail(app),
        detailRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE |
        DT_END_ELLIPSIS);

    RECT labelRect = {
        panel.left + padding,
        panel.bottom - Scale(app, 31),
        panel.left + padding + Scale(app, 136),
        panel.bottom - Scale(app, 10)
    };
    RECT track = VolumeTrackRect(app);
    RECT thumb = VolumeThumbRect(app);

    COLORREF labelColor = app->volumeAvailable
        ? theme.text
        : theme.textDisabled;
    COLORREF railColor = app->dark ? RGB(74, 77, 84) : RGB(216, 222, 230);
    COLORREF fillColor = app->volumeAvailable
        ? theme.accent
        : theme.textDisabled;
    COLORREF thumbColor = app->dark ? RGB(245, 247, 250) :
        RGB(255, 255, 255);

    DrawTextLine(dc, app->smallFont, labelColor, SystemVolumeLabel(app),
        labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE |
        DT_END_ELLIPSIS);

    RECT rail = track;
    RECT fill = track;
    fill.right = track.left + MulDiv(app->systemVolumePercent,
        RectWidth(track), 100);

    FillRoundedRect(dc, rail, Scale(app, 5), railColor, railColor);
    FillRoundedRect(dc, fill, Scale(app, 5), fillColor, fillColor);
    FillRoundedRect(dc, thumb, Scale(app, 8), thumbColor, theme.accent);
}

// ############################
// Layout and theme application
// ############################

void ApplyTheme(AppState* app) {
    const Theme& theme = CurrentTheme(app);
    ResetBrush(app->windowBrush, theme.background);
    ResetBrush(app->surfaceBrush, theme.surface);

    BOOL darkMode = app->dark ? TRUE : FALSE;
    int cornerPreference = kDwmCornerRound;
    int backdrop = kDwmBackdropNone;
    COLORREF borderColor = theme.border;
    COLORREF captionColor = theme.background;
    COLORREF captionTextColor = theme.text;

    DwmSetWindowAttribute(app->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
        &darkMode, sizeof(darkMode));
    DwmSetWindowAttribute(app->hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
        &cornerPreference, sizeof(cornerPreference));
    DwmSetWindowAttribute(app->hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
        &backdrop, sizeof(backdrop));
    DwmSetWindowAttribute(app->hwnd, DWMWA_BORDER_COLOR, &borderColor,
        sizeof(borderColor));
    DwmSetWindowAttribute(app->hwnd, DWMWA_CAPTION_COLOR, &captionColor,
        sizeof(captionColor));
    DwmSetWindowAttribute(app->hwnd, DWMWA_TEXT_COLOR, &captionTextColor,
        sizeof(captionTextColor));

    SetWindowTheme(app->deviceList,
        app->dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);

    RedrawWholeWindow(app);
}

void Resize(AppState* app) {
    RECT rect = {};
    GetClientRect(app->hwnd, &rect);

    int width = RectWidth(rect);
    int height = RectHeight(rect);
    int margin = Scale(app, 24);
    int top = Scale(app, 20);
    int themeWidth = std::max(Scale(app, 36),
        FontLineHeight(app->hwnd, app->smallFont) + Scale(app, 14));
    int themeHeight = std::max(Scale(app, 32),
        FontLineHeight(app->hwnd, app->smallFont) + Scale(app, 14));
    int titleHeight = FontLineHeight(app->hwnd, app->titleFont) +
        Scale(app, 8);
    int subtitleHeight = FontLineHeight(app->hwnd, app->bodyFont) +
        Scale(app, 6);
    int headingHeight = FontLineHeight(app->hwnd, app->headingFont) +
        Scale(app, 6);
    int buttonHeight = std::max(Scale(app, 34),
        FontLineHeight(app->hwnd, app->bodyFont) + Scale(app, 16));
    int statusHeight = FontLineHeight(app->hwnd, app->bodyFont) +
        FontLineHeight(app->hwnd, app->smallFont) * 2 + Scale(app, 44);

    int titleRight = width - margin - themeWidth - Scale(app, 14);
    titleRight = std::max(margin + Scale(app, 180), titleRight);

    MoveWindow(app->title, margin, top, titleRight - margin, titleHeight,
        TRUE);
    MoveWindow(app->subtitle, margin, top + titleHeight,
        titleRight - margin, subtitleHeight, TRUE);
    MoveWindow(app->themeButton, width - margin - themeWidth, top,
        themeWidth, themeHeight, TRUE);

    int headingY = top + titleHeight + subtitleHeight + Scale(app, 22);
    MoveWindow(app->deviceHeading, margin, headingY, width - margin * 2,
        headingHeight, TRUE);

    int statusY = height - margin - statusHeight;
    int buttonY = statusY - Scale(app, 12) - buttonHeight;
    int listY = headingY + headingHeight + Scale(app, 8);
    int listHeight = buttonY - Scale(app, 14) - listY;
    listHeight = std::max(Scale(app, 112), listHeight);

    MoveWindow(app->deviceList, margin, listY, width - margin * 2,
        listHeight, TRUE);

    MoveWindow(app->start, margin, buttonY, Scale(app, 88),
        buttonHeight, TRUE);
    MoveWindow(app->stop, margin + Scale(app, 96), buttonY,
        Scale(app, 88), buttonHeight, TRUE);
    MoveWindow(app->refresh, margin + Scale(app, 192), buttonY,
        Scale(app, 96), buttonHeight, TRUE);
    MoveWindow(app->statusPanel, margin, statusY, width - margin * 2,
        statusHeight, TRUE);

    UpdateDeviceScrollBar(app);
}

// ############################
// Device refresh and commands
// ############################

void RefreshDevices(AppState* app) {
    auto savedConfigs = LoadDeviceConfigs();
    auto previouslySelected = CheckedDeviceIds(app);
    auto oldRatios = app->deviceRatios;
    auto oldDevices = app->devices;

    std::wstring error;
    app->devices = EnumeratePlaybackDevices(&error);
    app->selected.assign(app->devices.size(), false);
    app->deviceRatios.assign(app->devices.size(), 100);
    RefreshSystemVolume(app);

    for (size_t i = 0; i < app->devices.size(); ++i) {
        bool checked = std::find(previouslySelected.begin(),
            previouslySelected.end(), app->devices[i].id) !=
            previouslySelected.end();
        int ratio = 100;

        // 如果当前没有临时选择, 就从配置文件恢复上次明确保存的选择.
        if (previouslySelected.empty()) {
            for (const auto& config : savedConfigs) {
                if (config.id == app->devices[i].id) {
                    checked = config.selected;
                    ratio = static_cast<int>(std::clamp(config.volume, 0.0f,
                        static_cast<float>(kDeviceRatioMaxPercent) / 100.0f) *
                        100.0f + 0.5f);
                    break;
                }
            }
        }

        for (size_t j = 0; j < oldDevices.size() && j < oldRatios.size(); ++j) {
            if (app->devices[i].id == oldDevices[j].id) {
                ratio = oldRatios[j];
                break;
            }
        }

        app->selected[i] = checked;
        app->deviceRatios[i] = std::clamp(ratio, 0, kDeviceRatioMaxPercent);
    }

    if (!error.empty()) {
        SetStatus(app, error);
    } else {
        SetStatus(app, L"Idle");
    }

    app->deviceScroll = 0;
    app->hotDeviceIndex = -1;
    app->focusDeviceIndex = app->devices.empty() ? -1 : 0;
    app->activeDeviceSliderIndex = -1;
    app->deviceSliderDragging = false;

    UpdateDeviceScrollBar(app);
    UpdateButtons(app);
    InvalidateRect(app->deviceList, nullptr, TRUE);
    InvalidateStatus(app);
}

void Start(AppState* app) {
    auto devices = CheckedDevices(app);

    if (devices.empty()) {
        return;
    }

    SaveCurrentDeviceConfigs(app);
    SetStatus(app, L"Starting...");

    std::wstring error;
    bool ok = app->engine.Start(devices,
        [hwnd = app->hwnd](const std::wstring& text) {
            auto* copy = new std::wstring(text);
            PostMessageW(hwnd, kStatusMessage, 0,
                reinterpret_cast<LPARAM>(copy));
        },
        &error);

    if (!ok) {
        SetStatus(app, error);
    } else {
        PromoteSelectedDevices(app);
        app->focusDeviceIndex = app->selected.empty() ? -1 : 0;
        app->deviceScroll = 0;
        SetStatus(app, L"Running");
        InvalidateRect(app->deviceList, nullptr, TRUE);
    }

    UpdateButtons(app);
}

void Stop(AppState* app) {
    SetStatus(app, L"Stopping...");
    app->engine.Stop();
    SetStatus(app, L"Idle");
    UpdateButtons(app);
}

void UpdateButtons(AppState* app) {
    bool running = app->engine.IsRunning();
    bool hasSelection = SelectedDeviceCount(app) > 0;

    EnableWindow(app->start, !running && hasSelection);
    EnableWindow(app->stop, running);
    EnableWindow(app->refresh, !running);
    EnableWindow(app->deviceList, !running);

    InvalidateRect(app->deviceList, nullptr, TRUE);
    InvalidateStatus(app);
}

// ############################
// Custom child window procedures
// ############################

LRESULT CALLBACK DeviceListProc(HWND hwnd, UINT message, WPARAM wParam,
    LPARAM lParam) {
    HWND parent = GetParent(hwnd);
    auto* app = reinterpret_cast<AppState*>(
        GetWindowLongPtrW(parent, GWLP_USERDATA));

    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(hwnd, &paint);

        if (app) {
            PaintBuffered(hwnd, dc, [app](HDC memoryDc) {
                DrawDeviceList(app, memoryDc);
            });
        }

        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (app) {
            UpdateDeviceScrollBar(app);
        }
        return 0;
    case WM_ENABLE:
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_MOUSEMOVE:
        if (app && app->deviceSliderDragging) {
            UpdateDeviceSliderFromPoint(app, app->activeDeviceSliderIndex,
                GET_X_LPARAM(lParam));
            return 0;
        }

        if (app && app->deviceThumbDragging) {
            RECT client = {};
            GetClientRect(hwnd, &client);

            int contentHeight = DeviceContentHeight(app);
            int pageHeight = RectHeight(client);
            int trackInset = Scale(app, 4);
            int trackHeight = std::max(1, pageHeight - trackInset * 2);
            int thumbHeight = RectHeight(DeviceThumbRect(app));
            int thumbTravel = std::max(1, trackHeight - thumbHeight);
            int maxScroll = std::max(1, contentHeight - pageHeight);
            int y = GET_Y_LPARAM(lParam) - app->deviceThumbDragOffset;
            int trackTop = client.top + trackInset;
            int next = MulDiv(y - trackTop, maxScroll, thumbTravel);

            SetDeviceScroll(app, next);
            return 0;
        }

        if (app && IsWindowEnabled(hwnd)) {
            int index = DeviceIndexFromPoint(app, GET_Y_LPARAM(lParam));

            if (index != app->hotDeviceIndex) {
                app->hotDeviceIndex = index;
                InvalidateRect(hwnd, nullptr, TRUE);
            }

            TRACKMOUSEEVENT event = {};
            event.cbSize = sizeof(event);
            event.dwFlags = TME_LEAVE;
            event.hwndTrack = hwnd;
            TrackMouseEvent(&event);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (app && !app->deviceThumbDragging) {
            app->hotDeviceIndex = -1;
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        if (app && IsWindowEnabled(hwnd)) {
            POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            RECT thumb = DeviceThumbRect(app);

            if (!IsRectEmpty(&thumb) && PtInRect(&thumb, point)) {
                app->deviceThumbDragging = true;
                app->deviceThumbDragOffset = point.y - thumb.top;
                SetCapture(hwnd);
                return 0;
            }

            if (!IsRectEmpty(&thumb) &&
                point.x >= thumb.left - Scale(app, 3)) {
                int direction = point.y < thumb.top ? -1 : 1;
                RECT client = {};
                GetClientRect(hwnd, &client);
                SetDeviceScroll(app, app->deviceScroll +
                    direction * RectHeight(client));
                return 0;
            }

            int index = DeviceIndexFromPoint(app, point.y);
            if (index >= 0 && index < static_cast<int>(app->devices.size()) &&
                app->selected[static_cast<size_t>(index)] &&
                PointInDeviceSlider(app, index, point)) {
                app->deviceSliderDragging = true;
                app->activeDeviceSliderIndex = index;
                SetCapture(hwnd);
                UpdateDeviceSliderFromPoint(app, index, point.x);
                return 0;
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (app && app->deviceSliderDragging) {
            UpdateDeviceSliderFromPoint(app, app->activeDeviceSliderIndex,
                GET_X_LPARAM(lParam));
            app->deviceSliderDragging = false;
            app->activeDeviceSliderIndex = -1;
            ReleaseCapture();
            return 0;
        }

        if (app && app->deviceThumbDragging) {
            app->deviceThumbDragging = false;
            ReleaseCapture();
            return 0;
        }

        if (app && IsWindowEnabled(hwnd)) {
            int index = DeviceIndexFromPoint(app, GET_Y_LPARAM(lParam));
            ToggleDevice(app, index);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (app) {
            app->deviceThumbDragging = false;
            app->deviceSliderDragging = false;
            app->activeDeviceSliderIndex = -1;
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (app) {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            int steps = delta / WHEEL_DELTA;

            if (steps == 0 && delta != 0) {
                steps = delta > 0 ? 1 : -1;
            }

            SetDeviceScroll(app, app->deviceScroll - steps * 42);
        }
        return 0;
    case WM_KEYDOWN:
        if (app && !app->devices.empty()) {
            int lastIndex = static_cast<int>(app->devices.size()) - 1;
            int next = app->focusDeviceIndex;

            if (next < 0) {
                next = 0;
            }

            if (wParam == VK_DOWN) {
                next = std::min(lastIndex, next + 1);
            } else if (wParam == VK_UP) {
                next = std::max(0, next - 1);
            } else if (wParam == VK_HOME) {
                next = 0;
            } else if (wParam == VK_END) {
                next = lastIndex;
            } else if (wParam == VK_SPACE) {
                ToggleDevice(app, next);
                return 0;
            } else {
                break;
            }

            app->focusDeviceIndex = next;
            EnsureDeviceVisible(app, next);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK StatusPanelProc(HWND hwnd, UINT message, WPARAM wParam,
    LPARAM lParam) {
    HWND parent = GetParent(hwnd);
    auto* app = reinterpret_cast<AppState*>(
        GetWindowLongPtrW(parent, GWLP_USERDATA));

    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(hwnd, &paint);

        if (app) {
            PaintBuffered(hwnd, dc, [app](HDC memoryDc) {
                DrawStatusPanel(app, memoryDc);
            });
        }

        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        if (app && app->volumeAvailable) {
            POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

            if (PointInVolumeSlider(app, point)) {
                app->volumeDragging = true;
                SetCapture(hwnd);
                SetSystemVolumeFromPoint(app, point.x);
                return 0;
            }
        }
        break;
    case WM_MOUSEMOVE:
        if (app && app->volumeDragging) {
            SetSystemVolumeFromPoint(app, GET_X_LPARAM(lParam));
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (app && app->volumeDragging) {
            app->volumeDragging = false;
            ReleaseCapture();
            SetSystemVolumeFromPoint(app, GET_X_LPARAM(lParam));
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (app) {
            app->volumeDragging = false;
        }
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// ############################
// Main window procedure
// ############################

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam,
    LPARAM lParam) {
    auto* app = reinterpret_cast<AppState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
    case WM_CREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = reinterpret_cast<AppState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->hwnd = hwnd;
        app->dpi = GetDpiForWindow(hwnd);

        app->title = CreateWindowW(L"STATIC", L"MultiTap",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_LEFT, 0, 0, 0, 0,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTitleLabel)),
            GetModuleHandleW(nullptr), nullptr);
        app->subtitle = CreateWindowW(L"STATIC",
            L"Play system audio through multiple output devices at once.",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_LEFT, 0, 0, 0, 0,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSubtitleLabel)),
            GetModuleHandleW(nullptr), nullptr);
        app->themeButton = CreateWindowW(L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW |
            WS_TABSTOP,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kThemeButton)),
            GetModuleHandleW(nullptr), nullptr);
        app->deviceHeading = CreateWindowW(L"STATIC", L"Output devices",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_LEFT, 0, 0, 0, 0,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDeviceHeading)),
            GetModuleHandleW(nullptr), nullptr);
        app->deviceList = CreateWindowExW(0, L"MultiTapDeviceList", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDeviceList)),
            GetModuleHandleW(nullptr), nullptr);
        app->start = CreateWindowW(L"BUTTON", L"Start",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW |
            WS_TABSTOP,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStartButton)),
            GetModuleHandleW(nullptr), nullptr);
        app->stop = CreateWindowW(L"BUTTON", L"Stop",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW |
            WS_TABSTOP,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStopButton)),
            GetModuleHandleW(nullptr), nullptr);
        app->refresh = CreateWindowW(L"BUTTON", L"Refresh",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW |
            WS_TABSTOP,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshButton)),
            GetModuleHandleW(nullptr), nullptr);
        app->statusPanel = CreateWindowExW(0, L"MultiTapStatusPanel", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusPanel)),
            GetModuleHandleW(nullptr), nullptr);

        RebuildFonts(app);

        ApplyTheme(app);
        RefreshDevices(app);
        Resize(app);
        return 0;
    }
    case WM_GETMINMAXINFO:
        if (lParam) {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            UINT dpi = app ? app->dpi : GetDpiForSystem();
            info->ptMinTrackSize.x = ScaleForDpi(kMinimumWindowWidth, dpi);
            info->ptMinTrackSize.y = ScaleForDpi(kMinimumWindowHeight, dpi);
        }
        return 0;
    case WM_DPICHANGED:
        if (app) {
            app->dpi = HIWORD(wParam);
            RebuildFonts(app);

            auto* suggested = reinterpret_cast<RECT*>(lParam);

            if (suggested) {
                SetWindowPos(hwnd, nullptr, suggested->left,
                    suggested->top, RectWidth(*suggested),
                    RectHeight(*suggested), SWP_NOZORDER |
                    SWP_NOACTIVATE);
            }

            Resize(app);
            RedrawWholeWindow(app);
        }
        return 0;
    case WM_SIZE:
        if (app) {
            Resize(app);
        }
        return 0;
    case WM_COMMAND:
        if (!app) {
            break;
        }

        switch (LOWORD(wParam)) {
        case kStartButton:
            Start(app);
            return 0;
        case kStopButton:
            Stop(app);
            return 0;
        case kRefreshButton:
            RefreshDevices(app);
            return 0;
        case kThemeButton:
            app->dark = !app->dark;
            ApplyTheme(app);
            return 0;
        default:
            break;
        }
        break;
    case WM_CTLCOLORSTATIC:
        if (app) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            HWND control = reinterpret_cast<HWND>(lParam);
            const Theme& theme = CurrentTheme(app);

            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, theme.background);

            if (control == app->subtitle) {
                SetTextColor(dc, theme.textMuted);
            } else {
                SetTextColor(dc, theme.text);
            }

            return reinterpret_cast<LRESULT>(app->windowBrush);
        }
        break;
    case WM_DRAWITEM:
        if (app) {
            auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);

            if (item && item->CtlType == ODT_BUTTON) {
                DrawModernButton(app, item);
                return TRUE;
            }
        }
        break;
    case WM_ERASEBKGND:
        if (app) {
            RECT rect = {};
            GetClientRect(hwnd, &rect);
            FillRect(reinterpret_cast<HDC>(wParam), &rect,
                app->windowBrush);
            return 1;
        }
        break;
    case kStatusMessage:
        if (app) {
            std::unique_ptr<std::wstring> text(
                reinterpret_cast<std::wstring*>(lParam));
            SetStatus(app, *text);
            UpdateButtons(app);
        }
        return 0;
    case kVolumeChangedMessage:
        if (app) {
            app->systemVolumePercent = std::clamp(
                static_cast<int>(wParam), 0, 100);
            app->volumeAvailable = true;
            InvalidateStatus(app);
        }
        return 0;
    case WM_CLOSE:
        if (app) {
            app->engine.Stop();
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (app) {
            if (app->volumeController) {
                app->volumeController->Close();
            }

            ReleaseFontObject(app->titleFont);
            ReleaseFontObject(app->headingFont);
            ReleaseFontObject(app->bodyFont);
            ReleaseFontObject(app->smallFont);

            if (app->windowBrush) {
                DeleteObject(app->windowBrush);
            }
            if (app->surfaceBrush) {
                DeleteObject(app->surfaceBrush);
            }
        }

        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");

    if (user32) {
        using SetContextProc = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto setContext = reinterpret_cast<SetContextProc>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));

        if (setContext &&
            setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            return;
        }
    }

    SetProcessDPIAware();
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX controls = {};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    const wchar_t* windowClassName = L"MultiTapWindow";
    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = windowClassName;
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassW(&windowClass);

    WNDCLASSW deviceListClass = {};
    deviceListClass.lpfnWndProc = DeviceListProc;
    deviceListClass.hInstance = instance;
    deviceListClass.hCursor = LoadCursorW(nullptr, IDC_HAND);
    deviceListClass.hbrBackground = nullptr;
    deviceListClass.lpszClassName = L"MultiTapDeviceList";
    deviceListClass.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassW(&deviceListClass);

    WNDCLASSW statusPanelClass = {};
    statusPanelClass.lpfnWndProc = StatusPanelProc;
    statusPanelClass.hInstance = instance;
    statusPanelClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    statusPanelClass.hbrBackground = nullptr;
    statusPanelClass.lpszClassName = L"MultiTapStatusPanel";
    statusPanelClass.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassW(&statusPanelClass);

    AppState app;
    UINT dpi = GetDpiForSystem();
    int windowWidth = ScaleForDpi(kDefaultWindowWidth, dpi);
    int windowHeight = ScaleForDpi(kDefaultWindowHeight, dpi);
    HWND hwnd = CreateWindowExW(0, windowClassName, L"MultiTap",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT, CW_USEDEFAULT, windowWidth, windowHeight,
        nullptr, nullptr, instance, &app);

    if (!hwnd) {
        CoUninitialize();
        return 1;
    }

    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd);

    MSG message = {};

    while (GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    CoUninitialize();
    return static_cast<int>(message.wParam);
}
