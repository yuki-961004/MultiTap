#pragma once

#include "Common.h"

#include <string>
#include <vector>

std::vector<PlaybackDeviceInfo> EnumeratePlaybackDevices(std::wstring* error);
std::wstring GetDefaultPlaybackDeviceId(std::wstring* error);

