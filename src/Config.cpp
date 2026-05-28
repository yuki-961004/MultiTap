#include "Config.h"

#include <ShlObj.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

std::filesystem::path ConfigPath() {
    PWSTR appData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        return L"multitap.cfg";
    }

    std::filesystem::path path(appData);
    CoTaskMemFree(appData);
    path /= L"MultiTap";
    path /= L"config.txt";
    return path;
}

std::string NarrowAscii(const std::wstring& value) {
    std::string out;
    out.reserve(value.size());
    for (wchar_t ch : value) {
        out.push_back(ch >= 0 && ch <= 0x7f ? static_cast<char>(ch) : '?');
    }
    return out;
}

std::wstring WidenAscii(const std::string& value) {
    return std::wstring(value.begin(), value.end());
}

float ParseVolume(const std::string& value) {
    try {
        return std::clamp(std::stof(value), 0.0f, 1.0f);
    } catch (...) {
        return 1.0f;
    }
}

} // namespace

std::vector<DeviceConfig> LoadDeviceConfigs() {
    std::vector<DeviceConfig> configs;
    std::ifstream input(ConfigPath());
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }

        std::stringstream stream(line);
        std::string selected;
        std::string volume;
        std::string id;

        if (std::getline(stream, selected, '\t') &&
            std::getline(stream, volume, '\t') &&
            std::getline(stream, id)) {
            configs.push_back({ WidenAscii(id), selected == "1", ParseVolume(volume) });
        } else {
            configs.push_back({ WidenAscii(line), true, 1.0f });
        }
    }
    return configs;
}

std::vector<std::wstring> LoadSelectedDeviceIds() {
    std::vector<std::wstring> ids;
    for (const auto& config : LoadDeviceConfigs()) {
        if (config.selected) {
            ids.push_back(config.id);
        }
    }
    return ids;
}

void SaveSelectedDeviceIds(const std::vector<std::wstring>& ids) {
    auto path = ConfigPath();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::trunc);
    for (const auto& id : ids) {
        output << NarrowAscii(id) << '\n';
    }
}

void SaveDeviceConfigs(const std::vector<DeviceConfig>& configs) {
    auto path = ConfigPath();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::trunc);
    for (const auto& config : configs) {
        output << (config.selected ? "1" : "0") << '\t'
               << std::clamp(config.volume, 0.0f, 1.0f) << '\t'
               << NarrowAscii(config.id) << '\n';
    }
}
