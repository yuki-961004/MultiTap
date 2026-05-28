#pragma once

#include "Common.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class AudioEngine {
public:
    using StatusCallback = std::function<void(const std::wstring&)>;

    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool Start(const std::vector<OutputSelection>& selectedDevices, StatusCallback callback, std::wstring* error);
    void Stop();
    bool IsRunning() const;
    void SetDeviceVolume(const std::wstring& deviceId, float volume);

private:
    void Worker(std::vector<OutputSelection> selectedDevices, StatusCallback callback);
    void Publish(StatusCallback& callback, const std::wstring& text);
    float VolumeForDevice(const std::wstring& deviceId) const;

    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::thread worker_;
    mutable std::mutex callbackMutex_;
    mutable std::mutex volumeMutex_;
    std::vector<OutputSelection> volumes_;
};
