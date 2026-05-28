#pragma once

#include <string>

struct PlaybackDeviceInfo {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

struct OutputSelection {
    std::wstring id;
    float volume = 1.0f;
};

struct DeviceConfig {
    std::wstring id;
    bool selected = false;
    float volume = 1.0f;
};
