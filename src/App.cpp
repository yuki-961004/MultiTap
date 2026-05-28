#include "AudioEngine.h"
#include "Config.h"
#include "DeviceEnumerator.h"

#include <Windows.h>
#include <Commctrl.h>
#include <Dwmapi.h>
#include <Objbase.h>
#include <Shlwapi.h>
#include <Uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr int kDeviceList = 1001;
constexpr int kStartButton = 1002;
constexpr int kStopButton = 1003;
constexpr int kRefreshButton = 1004;
constexpr int kVolumeSlider = 1005;
constexpr int kVolumeLabel = 1006;
constexpr int kTitleLabel = 1007;
constexpr int kSubtitleLabel = 1008;
constexpr int kThemeButton = 1009;
constexpr int kMinButton = 1010;
constexpr int kMaxButton = 1011;
constexpr int kCloseButton = 1012;
constexpr UINT kStatusMessage = WM_APP + 1;
constexpr UINT kSliderChangedMessage = WM_APP + 2;
constexpr int kTitleBarHeight = 44;
constexpr int kResizeBorder = 8;

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

struct Theme {
    COLORREF window;
    COLORREF surface;
    COLORREF text;
    COLORREF muted;
    COLORREF accent;
    COLORREF border;
    COLORREF button;
    COLORREF buttonPressed;
    COLORREF buttonDisabled;
};

constexpr Theme kLightTheme = {
    RGB(243, 246, 250),
    RGB(250, 251, 253),
    RGB(31, 31, 31),
    RGB(96, 96, 96),
    RGB(0, 95, 184),
    RGB(213, 217, 222),
    RGB(252, 252, 252),
    RGB(235, 241, 249),
    RGB(239, 241, 244)
};

constexpr Theme kDarkTheme = {
    RGB(32, 32, 32),
    RGB(44, 44, 44),
    RGB(243, 243, 243),
    RGB(190, 190, 190),
    RGB(96, 174, 255),
    RGB(73, 73, 73),
    RGB(58, 58, 58),
    RGB(72, 72, 72),
    RGB(45, 45, 45)
};

struct AppState {
    HWND hwnd = nullptr;
    HWND list = nullptr;
    HWND start = nullptr;
    HWND stop = nullptr;
    HWND refresh = nullptr;
    HWND themeButton = nullptr;
    HWND minButton = nullptr;
    HWND maxButton = nullptr;
    HWND closeButton = nullptr;
    HWND title = nullptr;
    HWND subtitle = nullptr;
    HWND volumeSlider = nullptr;
    HWND volumeLabel = nullptr;
    HWND status = nullptr;
    std::vector<PlaybackDeviceInfo> devices;
    std::vector<int> volumes;
    AudioEngine engine;
    bool dark = false;
    bool useBackdrop = true;
    HFONT titleFont = nullptr;
    HFONT bodyFont = nullptr;
    HBRUSH windowBrush = nullptr;
    HBRUSH surfaceBrush = nullptr;
};

std::wstring WithDefaultLabel(const PlaybackDeviceInfo& device) {
    return device.isDefault ? device.name + L"  (default source; muted if unchecked)" : device.name;
}

const Theme& CurrentTheme(AppState* app) {
    return app->dark ? kDarkTheme : kLightTheme;
}

void ResetBrush(HBRUSH& brush, COLORREF color) {
    if (brush) {
        DeleteObject(brush);
    }
    brush = CreateSolidBrush(color);
}

HFONT MakeFont(int pointSize, int weight) {
    HDC dc = GetDC(nullptr);
    int height = -MulDiv(pointSize, GetDeviceCaps(dc, LOGPIXELSY), 72);
    ReleaseDC(nullptr, dc);
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
}

void ApplyTheme(AppState* app) {
    const auto& theme = CurrentTheme(app);
    ResetBrush(app->windowBrush, theme.window);
    ResetBrush(app->surfaceBrush, theme.surface);

    BOOL darkMode = app->dark ? TRUE : FALSE;
    DwmSetWindowAttribute(app->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    int cornerPreference = kDwmCornerRound;
    int backdrop = kDwmBackdropMainWindow;
    COLORREF borderColor = theme.border;
    COLORREF captionColor = app->dark ? RGB(32, 32, 32) : RGB(243, 246, 250);
    COLORREF captionTextColor = theme.text;
    DwmSetWindowAttribute(app->hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPreference, sizeof(cornerPreference));
    HRESULT backdropResult = DwmSetWindowAttribute(app->hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    app->useBackdrop = SUCCEEDED(backdropResult);
    DwmSetWindowAttribute(app->hwnd, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));
    DwmSetWindowAttribute(app->hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));
    DwmSetWindowAttribute(app->hwnd, DWMWA_TEXT_COLOR, &captionTextColor, sizeof(captionTextColor));

    SetWindowTheme(app->list, app->dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    SetWindowTheme(app->start, nullptr, nullptr);
    SetWindowTheme(app->stop, nullptr, nullptr);
    SetWindowTheme(app->refresh, nullptr, nullptr);
    SetWindowTheme(app->themeButton, nullptr, nullptr);
    SetWindowTheme(app->minButton, nullptr, nullptr);
    SetWindowTheme(app->maxButton, nullptr, nullptr);
    SetWindowTheme(app->closeButton, nullptr, nullptr);

    ListView_SetBkColor(app->list, theme.surface);
    ListView_SetTextBkColor(app->list, theme.surface);
    ListView_SetTextColor(app->list, theme.text);
    ListView_SetOutlineColor(app->list, theme.border);

    SetWindowTextW(app->themeButton, app->dark ? L"Light" : L"Dark");
    InvalidateRect(app->hwnd, nullptr, TRUE);
    InvalidateRect(app->list, nullptr, TRUE);
    InvalidateRect(app->volumeSlider, nullptr, TRUE);
    InvalidateRect(app->start, nullptr, TRUE);
    InvalidateRect(app->stop, nullptr, TRUE);
    InvalidateRect(app->refresh, nullptr, TRUE);
    InvalidateRect(app->themeButton, nullptr, TRUE);
    InvalidateRect(app->minButton, nullptr, TRUE);
    InvalidateRect(app->maxButton, nullptr, TRUE);
    InvalidateRect(app->closeButton, nullptr, TRUE);
}

void DrawModernButton(AppState* app, const DRAWITEMSTRUCT* item) {
    const auto& theme = CurrentTheme(app);
    bool disabled = (item->itemState & ODS_DISABLED) != 0;
    bool pressed = (item->itemState & ODS_SELECTED) != 0;
    bool focused = (item->itemState & ODS_FOCUS) != 0;

    FillRect(item->hDC, &item->rcItem, app->windowBrush);

    COLORREF fill = theme.button;
    COLORREF outline = theme.border;
    COLORREF text = theme.text;

    if (pressed) {
        fill = theme.buttonPressed;
    }
    if (disabled) {
        fill = theme.buttonDisabled;
        outline = app->dark ? RGB(58, 58, 58) : RGB(224, 226, 229);
        text = app->dark ? RGB(120, 120, 120) : RGB(150, 150, 150);
    }
    if (item->CtlID == kStartButton && !disabled) {
        fill = pressed ? RGB(0, 78, 154) : RGB(0, 95, 184);
        outline = fill;
        text = RGB(255, 255, 255);
    }

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, focused ? 2 : 1, focused ? theme.accent : outline);
    HGDIOBJ oldBrush = SelectObject(item->hDC, brush);
    HGDIOBJ oldPen = SelectObject(item->hDC, pen);

    RECT rc = item->rcItem;
    InflateRect(&rc, -1, -1);
    RoundRect(item->hDC, rc.left, rc.top, rc.right, rc.bottom, 9, 9);

    SelectObject(item->hDC, oldBrush);
    SelectObject(item->hDC, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    wchar_t label[128] = {};
    GetWindowTextW(item->hwndItem, label, static_cast<int>(_countof(label)));
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, text);
    SelectObject(item->hDC, app->bodyFont);
    DrawTextW(item->hDC, label, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

int GetSliderValue(HWND slider) {
    return static_cast<int>(GetWindowLongPtrW(slider, GWLP_USERDATA));
}

void SetSliderValue(HWND slider, int value) {
    SetWindowLongPtrW(slider, GWLP_USERDATA, std::clamp(value, 0, 100));
    InvalidateRect(slider, nullptr, TRUE);
}

int SliderValueFromX(HWND slider, int x) {
    RECT rc;
    GetClientRect(slider, &rc);
    int left = 12;
    int right = std::max<int>(left + 1, rc.right - 12);
    x = std::clamp(x, left, right);
    return static_cast<int>((x - left) * 100 / static_cast<double>(right - left) + 0.5);
}

void UpdateSliderFromPoint(HWND slider, int x) {
    int value = SliderValueFromX(slider, x);
    SetSliderValue(slider, value);
    SendMessageW(GetParent(slider), kSliderChangedMessage, static_cast<WPARAM>(value), reinterpret_cast<LPARAM>(slider));
}

LRESULT CALLBACK SliderProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        SetSliderValue(hwnd, 100);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        HWND parent = GetParent(hwnd);
        auto* app = reinterpret_cast<AppState*>(GetWindowLongPtrW(parent, GWLP_USERDATA));
        RECT rc;
        GetClientRect(hwnd, &rc);

        if (app) {
            const auto& theme = CurrentTheme(app);
            FillRect(dc, &rc, app->windowBrush);

            bool enabled = IsWindowEnabled(hwnd) != FALSE;
            int centerY = (rc.top + rc.bottom) / 2;
            int left = 12;
            int right = std::max<int>(left + 1, rc.right - 12);
            int value = GetSliderValue(hwnd);
            int thumbX = left + static_cast<int>((right - left) * value / 100.0);

            COLORREF rail = app->dark ? RGB(78, 78, 78) : RGB(210, 214, 220);
            COLORREF fill = enabled ? theme.accent : (app->dark ? RGB(92, 92, 92) : RGB(180, 184, 190));
            COLORREF thumb = enabled ? (app->dark ? RGB(245, 245, 245) : RGB(255, 255, 255)) : (app->dark ? RGB(120, 120, 120) : RGB(220, 220, 220));
            COLORREF thumbBorder = enabled ? theme.accent : theme.border;

            RECT railRc{ left, centerY - 3, right, centerY + 3 };
            HBRUSH railBrush = CreateSolidBrush(rail);
            HBRUSH fillBrush = CreateSolidBrush(fill);
            HBRUSH thumbBrush = CreateSolidBrush(thumb);
            HPEN noPen = CreatePen(PS_NULL, 0, 0);
            HPEN thumbPen = CreatePen(PS_SOLID, 1, thumbBorder);
            HGDIOBJ oldBrush = SelectObject(dc, railBrush);
            HGDIOBJ oldPen = SelectObject(dc, noPen);

            RoundRect(dc, railRc.left, railRc.top, railRc.right, railRc.bottom, 6, 6);
            RECT fillRc{ left, centerY - 3, thumbX, centerY + 3 };
            SelectObject(dc, fillBrush);
            RoundRect(dc, fillRc.left, fillRc.top, fillRc.right, fillRc.bottom, 6, 6);

            SelectObject(dc, thumbBrush);
            SelectObject(dc, thumbPen);
            Ellipse(dc, thumbX - 8, centerY - 8, thumbX + 8, centerY + 8);

            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(railBrush);
            DeleteObject(fillBrush);
            DeleteObject(thumbBrush);
            DeleteObject(noPen);
            DeleteObject(thumbPen);
        } else {
            FillRect(dc, &rc, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        UpdateSliderFromPoint(hwnd, GET_X_LPARAM(lParam));
        return 0;
    case WM_MOUSEMOVE:
        if (GetCapture() == hwnd && (wParam & MK_LBUTTON)) {
            UpdateSliderFromPoint(hwnd, GET_X_LPARAM(lParam));
        }
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) {
            ReleaseCapture();
            UpdateSliderFromPoint(hwnd, GET_X_LPARAM(lParam));
        }
        return 0;
    case WM_ENABLE:
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void SetStatus(AppState* app, const std::wstring& text) {
    SetWindowTextW(app->status, text.c_str());
}

std::vector<std::wstring> CheckedDeviceIds(AppState* app) {
    std::vector<std::wstring> ids;
    int count = ListView_GetItemCount(app->list);
    for (int i = 0; i < count; ++i) {
        if (ListView_GetCheckState(app->list, i) && i < static_cast<int>(app->devices.size())) {
            ids.push_back(app->devices[static_cast<size_t>(i)].id);
        }
    }
    return ids;
}

int SelectedIndex(AppState* app) {
    return ListView_GetNextItem(app->list, -1, LVNI_SELECTED);
}

std::vector<OutputSelection> CheckedDevices(AppState* app) {
    std::vector<OutputSelection> devices;
    int count = ListView_GetItemCount(app->list);
    for (int i = 0; i < count; ++i) {
        if (ListView_GetCheckState(app->list, i) && i < static_cast<int>(app->devices.size())) {
            int volume = i < static_cast<int>(app->volumes.size()) ? app->volumes[static_cast<size_t>(i)] : 100;
            devices.push_back({ app->devices[static_cast<size_t>(i)].id, static_cast<float>(volume) / 100.0f });
        }
    }
    return devices;
}

std::vector<DeviceConfig> CurrentDeviceConfigs(AppState* app) {
    std::vector<DeviceConfig> configs;
    int count = ListView_GetItemCount(app->list);
    for (int i = 0; i < count && i < static_cast<int>(app->devices.size()); ++i) {
        int volume = i < static_cast<int>(app->volumes.size()) ? app->volumes[static_cast<size_t>(i)] : 100;
        configs.push_back({
            app->devices[static_cast<size_t>(i)].id,
            ListView_GetCheckState(app->list, i) != FALSE,
            static_cast<float>(volume) / 100.0f
        });
    }
    return configs;
}

void SaveCurrentDeviceConfigs(AppState* app) {
    SaveDeviceConfigs(CurrentDeviceConfigs(app));
}

void SetVolumeCell(AppState* app, int index) {
    if (index < 0 || index >= static_cast<int>(app->devices.size())) {
        return;
    }

    std::wstring text;
    if (app->devices[static_cast<size_t>(index)].isDefault) {
        text = L"Windows";
    } else {
        int volume = index < static_cast<int>(app->volumes.size()) ? app->volumes[static_cast<size_t>(index)] : 100;
        text = std::to_wstring(volume) + L"%";
    }
    ListView_SetItemText(app->list, index, 1, text.data());
}

void UpdateVolumeControls(AppState* app) {
    int index = SelectedIndex(app);
    bool valid = index >= 0 && index < static_cast<int>(app->devices.size());
    bool adjustable = valid && !app->devices[static_cast<size_t>(index)].isDefault;
    EnableWindow(app->volumeSlider, adjustable);

    if (!valid) {
        SetWindowTextW(app->volumeLabel, L"Selected ratio: -");
        SetSliderValue(app->volumeSlider, 100);
        return;
    }

    if (app->devices[static_cast<size_t>(index)].isDefault) {
        SetWindowTextW(app->volumeLabel, L"Selected ratio: Windows system volume");
        SetSliderValue(app->volumeSlider, 100);
        return;
    }

    int volume = index < static_cast<int>(app->volumes.size()) ? app->volumes[static_cast<size_t>(index)] : 100;
    std::wstring label = L"Selected ratio: " + std::to_wstring(volume) + L"%";
    SetWindowTextW(app->volumeLabel, label.c_str());
    SetSliderValue(app->volumeSlider, volume);
}

void UpdateButtons(AppState* app) {
    bool running = app->engine.IsRunning();
    bool hasSelection = !CheckedDeviceIds(app).empty();
    EnableWindow(app->start, !running && hasSelection);
    EnableWindow(app->stop, running);
    EnableWindow(app->refresh, !running);
    EnableWindow(app->list, !running);
    UpdateVolumeControls(app);
}

void Resize(AppState* app) {
    RECT rc;
    GetClientRect(app->hwnd, &rc);
    int margin = 12;
    int headerH = 70;
    int buttonH = 30;
    int sliderH = 34;
    int statusH = 24;
    int gap = 8;
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    int titleButtonH = 36;
    int titleButtonY = 4;
    int closeW = 46;
    int themeW = 74;
    MoveWindow(app->closeButton, width - closeW, titleButtonY, closeW, titleButtonH, TRUE);
    MoveWindow(app->maxButton, width - closeW * 2, titleButtonY, closeW, titleButtonH, TRUE);
    MoveWindow(app->minButton, width - closeW * 3, titleButtonY, closeW, titleButtonH, TRUE);
    MoveWindow(app->themeButton, width - closeW * 3 - themeW - 8, titleButtonY, themeW, titleButtonH, TRUE);

    int contentTop = kTitleBarHeight + headerH;
    int listH = height - contentTop - margin * 2 - buttonH - sliderH - statusH - gap * 2;
    if (listH < 80) {
        listH = 80;
    }

    MoveWindow(app->title, margin, kTitleBarHeight + 8, width - margin * 2, 30, TRUE);
    MoveWindow(app->subtitle, margin, kTitleBarHeight + 40, width - margin * 2, 24, TRUE);
    MoveWindow(app->list, margin, contentTop, width - margin * 2, listH, TRUE);

    int y = contentTop + listH + gap;
    MoveWindow(app->start, margin, y, 84, buttonH, TRUE);
    MoveWindow(app->stop, margin + 92, y, 84, buttonH, TRUE);
    MoveWindow(app->refresh, margin + 184, y, 116, buttonH, TRUE);
    int sliderY = y + buttonH + gap;
    MoveWindow(app->volumeLabel, margin, sliderY + 4, 230, statusH, TRUE);
    MoveWindow(app->volumeSlider, margin + 230, sliderY, width - margin * 2 - 230, sliderH, TRUE);
    MoveWindow(app->status, margin, sliderY + sliderH + 2, width - margin * 2, statusH, TRUE);

    ListView_SetColumnWidth(app->list, 0, width - margin * 3 - 90);
    ListView_SetColumnWidth(app->list, 1, 90);
}

void RefreshDevices(AppState* app) {
    auto savedConfigs = LoadDeviceConfigs();
    auto previouslySelected = CheckedDeviceIds(app);

    auto oldDevices = app->devices;
    auto oldVolumes = app->volumes;
    ListView_DeleteAllItems(app->list);
    std::wstring error;
    app->devices = EnumeratePlaybackDevices(&error);
    app->volumes.assign(app->devices.size(), 100);
    for (size_t i = 0; i < app->devices.size(); ++i) {
        for (const auto& config : savedConfigs) {
            if (app->devices[i].id == config.id) {
                app->volumes[i] = static_cast<int>(std::clamp(config.volume, 0.0f, 1.0f) * 100.0f + 0.5f);
                break;
            }
        }
        for (size_t j = 0; j < oldDevices.size() && j < oldVolumes.size(); ++j) {
            if (app->devices[i].id == oldDevices[j].id) {
                app->volumes[i] = oldVolumes[j];
                break;
            }
        }
    }
    if (!error.empty()) {
        SetStatus(app, error);
    } else {
        SetStatus(app, L"Idle");
    }

    for (size_t i = 0; i < app->devices.size(); ++i) {
        auto text = WithDefaultLabel(app->devices[i]);
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = text.data();
        ListView_InsertItem(app->list, &item);
        SetVolumeCell(app, static_cast<int>(i));

        bool checked = std::find(previouslySelected.begin(), previouslySelected.end(), app->devices[i].id) != previouslySelected.end();
        if (previouslySelected.empty()) {
            for (const auto& config : savedConfigs) {
                if (config.id == app->devices[i].id) {
                    checked = config.selected;
                    break;
                }
            }
        }
        ListView_SetCheckState(app->list, static_cast<int>(i), checked);
    }

    UpdateButtons(app);
}

void Start(AppState* app) {
    auto devices = CheckedDevices(app);
    if (devices.empty()) {
        return;
    }

    SaveCurrentDeviceConfigs(app);
    if (devices.size() == 1) {
        SetStatus(app, L"Running one selected device; duplication may be unnecessary.");
    } else {
        SetStatus(app, L"Starting...");
    }

    std::wstring error;
    bool ok = app->engine.Start(devices, [hwnd = app->hwnd](const std::wstring& text) {
        auto* copy = new std::wstring(text);
        PostMessageW(hwnd, kStatusMessage, 0, reinterpret_cast<LPARAM>(copy));
    }, &error);

    if (!ok) {
        SetStatus(app, error);
    }
    UpdateButtons(app);
}

void Stop(AppState* app) {
    SetStatus(app, L"Stopping...");
    app->engine.Stop();
    SetStatus(app, L"Idle");
    UpdateButtons(app);
}

bool ScreenPointInWindow(HWND child, POINT screenPoint) {
    if (!child) {
        return false;
    }
    RECT rc;
    GetWindowRect(child, &rc);
    return PtInRect(&rc, screenPoint) != FALSE;
}

LRESULT HitTestWindow(AppState* app, HWND hwnd, LPARAM lParam) {
    POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

    if (ScreenPointInWindow(app->themeButton, pt) ||
        ScreenPointInWindow(app->minButton, pt) ||
        ScreenPointInWindow(app->maxButton, pt) ||
        ScreenPointInWindow(app->closeButton, pt)) {
        return HTCLIENT;
    }

    RECT wr;
    GetWindowRect(hwnd, &wr);
    if (!IsZoomed(hwnd)) {
        bool left = pt.x >= wr.left && pt.x < wr.left + kResizeBorder;
        bool right = pt.x < wr.right && pt.x >= wr.right - kResizeBorder;
        bool top = pt.y >= wr.top && pt.y < wr.top + kResizeBorder;
        bool bottom = pt.y < wr.bottom && pt.y >= wr.bottom - kResizeBorder;

        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
    }

    POINT client = pt;
    ScreenToClient(hwnd, &client);
    if (client.y >= 0 && client.y < kTitleBarHeight) {
        return HTCAPTION;
    }
    return HTCLIENT;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
    case WM_NCHITTEST:
        if (app) {
            return HitTestWindow(app, hwnd, lParam);
        }
        break;
    case WM_CREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = reinterpret_cast<AppState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->hwnd = hwnd;

        app->title = CreateWindowW(L"STATIC", L"MultiTap", WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTitleLabel)), GetModuleHandleW(nullptr), nullptr);
        app->subtitle = CreateWindowW(L"STATIC", L"Duplicate system audio to selected playback devices.", WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSubtitleLabel)), GetModuleHandleW(nullptr), nullptr);
        app->themeButton = CreateWindowW(L"BUTTON", L"Dark", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kThemeButton)), GetModuleHandleW(nullptr), nullptr);
        app->minButton = CreateWindowW(L"BUTTON", L"_", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMinButton)), GetModuleHandleW(nullptr), nullptr);
        app->maxButton = CreateWindowW(L"BUTTON", L"[]", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMaxButton)), GetModuleHandleW(nullptr), nullptr);
        app->closeButton = CreateWindowW(L"BUTTON", L"X", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseButton)), GetModuleHandleW(nullptr), nullptr);

        app->list = CreateWindowExW(0, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDeviceList)), GetModuleHandleW(nullptr), nullptr);
        ListView_SetExtendedListViewStyle(app->list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        LVCOLUMNW column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<wchar_t*>(L"Playback devices");
        column.cx = 500;
        ListView_InsertColumn(app->list, 0, &column);

        LVCOLUMNW volumeColumn = {};
        volumeColumn.mask = LVCF_TEXT | LVCF_WIDTH;
        volumeColumn.pszText = const_cast<wchar_t*>(L"Volume");
        volumeColumn.cx = 90;
        ListView_InsertColumn(app->list, 1, &volumeColumn);

        app->start = CreateWindowW(L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStartButton)), GetModuleHandleW(nullptr), nullptr);
        app->stop = CreateWindowW(L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStopButton)), GetModuleHandleW(nullptr), nullptr);
        app->refresh = CreateWindowW(L"BUTTON", L"Refresh devices", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshButton)), GetModuleHandleW(nullptr), nullptr);
        app->volumeLabel = CreateWindowW(L"STATIC", L"Selected ratio: -", WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kVolumeLabel)), GetModuleHandleW(nullptr), nullptr);
        app->volumeSlider = CreateWindowW(L"MultiTapSlider", L"", WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kVolumeSlider)), GetModuleHandleW(nullptr), nullptr);
        SetSliderValue(app->volumeSlider, 100);
        app->status = CreateWindowW(L"STATIC", L"Idle", WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);

        app->titleFont = MakeFont(18, FW_SEMIBOLD);
        app->bodyFont = MakeFont(9, FW_NORMAL);
        SendMessageW(app->title, WM_SETFONT, reinterpret_cast<WPARAM>(app->titleFont), TRUE);
        SendMessageW(app->subtitle, WM_SETFONT, reinterpret_cast<WPARAM>(app->bodyFont), TRUE);
        SendMessageW(app->themeButton, WM_SETFONT, reinterpret_cast<WPARAM>(app->bodyFont), TRUE);
        SendMessageW(app->minButton, WM_SETFONT, reinterpret_cast<WPARAM>(app->bodyFont), TRUE);
        SendMessageW(app->maxButton, WM_SETFONT, reinterpret_cast<WPARAM>(app->bodyFont), TRUE);
        SendMessageW(app->closeButton, WM_SETFONT, reinterpret_cast<WPARAM>(app->bodyFont), TRUE);
        SendMessageW(app->list, WM_SETFONT, reinterpret_cast<WPARAM>(app->bodyFont), TRUE);
        SendMessageW(app->start, WM_SETFONT, reinterpret_cast<WPARAM>(app->bodyFont), TRUE);
        SendMessageW(app->stop, WM_SETFONT, reinterpret_cast<WPARAM>(app->bodyFont), TRUE);
        SendMessageW(app->refresh, WM_SETFONT, reinterpret_cast<WPARAM>(app->bodyFont), TRUE);
        SendMessageW(app->volumeLabel, WM_SETFONT, reinterpret_cast<WPARAM>(app->bodyFont), TRUE);
        SendMessageW(app->status, WM_SETFONT, reinterpret_cast<WPARAM>(app->bodyFont), TRUE);

        ApplyTheme(app);
        RefreshDevices(app);
        Resize(app);
        return 0;
    }
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
        case kMinButton:
            ShowWindow(hwnd, SW_MINIMIZE);
            return 0;
        case kMaxButton:
            ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            return 0;
        case kCloseButton:
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        default:
            break;
        }
        break;
    case WM_CTLCOLORSTATIC:
        if (app) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            HWND control = reinterpret_cast<HWND>(lParam);
            const auto& theme = CurrentTheme(app);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, control == app->subtitle || control == app->status ? theme.muted : theme.text);
            return reinterpret_cast<LRESULT>(app->useBackdrop ? GetStockObject(NULL_BRUSH) : app->windowBrush);
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
            if (app->useBackdrop) {
                return 1;
            }
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, app->windowBrush);
            return 1;
        }
        break;
    case kSliderChangedMessage:
        if (app && reinterpret_cast<HWND>(lParam) == app->volumeSlider) {
            int index = SelectedIndex(app);
            if (index >= 0 && index < static_cast<int>(app->devices.size()) && !app->devices[static_cast<size_t>(index)].isDefault) {
                int volume = static_cast<int>(wParam);
                app->volumes[static_cast<size_t>(index)] = volume;
                SetVolumeCell(app, index);
                UpdateVolumeControls(app);
                app->engine.SetDeviceVolume(app->devices[static_cast<size_t>(index)].id, static_cast<float>(volume) / 100.0f);
                SaveCurrentDeviceConfigs(app);
            }
        }
        return 0;
    case WM_NOTIFY:
        if (app) {
            auto* hdr = reinterpret_cast<NMHDR*>(lParam);
            if (hdr->idFrom == kDeviceList && hdr->code == LVN_ITEMCHANGED) {
                UpdateButtons(app);
                if (!app->engine.IsRunning()) {
                    SaveCurrentDeviceConfigs(app);
                }
            }
        }
        break;
    case kStatusMessage:
        if (app) {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
            SetStatus(app, *text);
            if (*text != L"Running" && *text != L"Starting...") {
                UpdateButtons(app);
            }
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
            if (app->titleFont) {
                DeleteObject(app->titleFont);
            }
            if (app->bodyFont) {
                DeleteObject(app->bodyFont);
            }
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

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    const wchar_t* className = L"MultiTapWindow";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = className;

    RegisterClassW(&wc);

    WNDCLASSW sliderClass = {};
    sliderClass.lpfnWndProc = SliderProc;
    sliderClass.hInstance = instance;
    sliderClass.hCursor = LoadCursorW(nullptr, IDC_HAND);
    sliderClass.hbrBackground = nullptr;
    sliderClass.lpszClassName = L"MultiTapSlider";
    RegisterClassW(&sliderClass);

    AppState app;
    DWORD style = WS_OVERLAPPED | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    HWND hwnd = CreateWindowExW(0, className, L"MultiTap",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT, 660, 460,
        nullptr, nullptr, instance, &app);

    if (!hwnd) {
        CoUninitialize();
        return 1;
    }

    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
