#pragma once

#include "Common.h"

#include <string>
#include <vector>

std::vector<std::wstring> LoadSelectedDeviceIds();
void SaveSelectedDeviceIds(const std::vector<std::wstring>& ids);
std::vector<DeviceConfig> LoadDeviceConfigs();
void SaveDeviceConfigs(const std::vector<DeviceConfig>& configs);
