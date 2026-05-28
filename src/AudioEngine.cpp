#include "AudioEngine.h"

#include "ComPtr.h"
#include "DeviceEnumerator.h"

#include <Audioclient.h>
#include <Avrt.h>
#include <Windows.h>
#include <Endpointvolume.h>
#include <Ksmedia.h>
#include <Mmdeviceapi.h>
#include <Objbase.h>

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace {

constexpr REFERENCE_TIME kRequestedDuration = 10000000; // 1 second

std::wstring HResultText(HRESULT hr) {
    wchar_t buffer[64] = {};
    swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(hr));
    return buffer;
}

struct CoTaskMemDeleter {
    void operator()(void* ptr) const {
        CoTaskMemFree(ptr);
    }
};

using WaveFormatPtr = std::unique_ptr<WAVEFORMATEX, CoTaskMemDeleter>;

std::wstring DeviceNameFromId(IMMDeviceEnumerator* enumerator, const std::wstring& id) {
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDevice(id.c_str(), device.put()))) {
        return id;
    }

    std::wstring error;
    auto devices = EnumeratePlaybackDevices(&error);
    for (const auto& item : devices) {
        if (item.id == id) {
            return item.name;
        }
    }
    return id;
}

struct RenderEndpoint {
    std::wstring id;
    std::wstring name;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    WaveFormatPtr mixFormat;
    UINT32 bufferFrames = 0;
    std::vector<BYTE> convertBuffer;
};

GUID SubFormatOf(const WAVEFORMATEX& format) {
    WORD tag = format.wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && format.cbSize >= 22) {
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        return extensible.SubFormat;
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT) {
        return KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return KSDATAFORMAT_SUBTYPE_PCM;
}

bool IsFloatFormat(const WAVEFORMATEX& format) {
    return IsEqualGUID(SubFormatOf(format), KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
}

bool IsPcmFormat(const WAVEFORMATEX& format) {
    return IsEqualGUID(SubFormatOf(format), KSDATAFORMAT_SUBTYPE_PCM);
}

bool IsConvertibleFormat(const WAVEFORMATEX& format) {
    if (IsFloatFormat(format)) {
        return format.wBitsPerSample == 32;
    }
    if (!IsPcmFormat(format)) {
        return false;
    }
    return format.wBitsPerSample == 16 || format.wBitsPerSample == 24 || format.wBitsPerSample == 32;
}

float ReadSample(const BYTE* data, const WAVEFORMATEX& format, UINT32 frame, UINT32 channel) {
    const BYTE* sample = data + static_cast<size_t>(frame) * format.nBlockAlign +
        static_cast<size_t>(channel) * (format.wBitsPerSample / 8);

    if (IsFloatFormat(format) && format.wBitsPerSample == 32) {
        float value = 0.0f;
        memcpy(&value, sample, sizeof(value));
        return value;
    }

    if (IsPcmFormat(format)) {
        if (format.wBitsPerSample == 16) {
            int16_t value = 0;
            memcpy(&value, sample, sizeof(value));
            return static_cast<float>(value / 32768.0f);
        }
        if (format.wBitsPerSample == 24) {
            int32_t value = static_cast<int32_t>(sample[0] | (sample[1] << 8) | (sample[2] << 16));
            if (value & 0x00800000) {
                value |= static_cast<int32_t>(0xff000000);
            }
            return static_cast<float>(value / 8388608.0f);
        }
        if (format.wBitsPerSample == 32) {
            int32_t value = 0;
            memcpy(&value, sample, sizeof(value));
            return static_cast<float>(value / 2147483648.0f);
        }
    }

    return 0.0f;
}

void WriteSample(BYTE* data, const WAVEFORMATEX& format, UINT32 frame, UINT32 channel, float value) {
    value = std::clamp(value, -1.0f, 1.0f);
    BYTE* sample = data + static_cast<size_t>(frame) * format.nBlockAlign +
        static_cast<size_t>(channel) * (format.wBitsPerSample / 8);

    if (IsFloatFormat(format) && format.wBitsPerSample == 32) {
        memcpy(sample, &value, sizeof(value));
        return;
    }

    if (IsPcmFormat(format)) {
        if (format.wBitsPerSample == 16) {
            int32_t scaled = static_cast<int32_t>(value * 32767.0f);
            int16_t out = static_cast<int16_t>(std::clamp(scaled, -32768, 32767));
            memcpy(sample, &out, sizeof(out));
            return;
        }
        if (format.wBitsPerSample == 24) {
            int32_t out = std::clamp(static_cast<int32_t>(value * 8388607.0f), -8388608, 8388607);
            sample[0] = static_cast<BYTE>(out & 0xff);
            sample[1] = static_cast<BYTE>((out >> 8) & 0xff);
            sample[2] = static_cast<BYTE>((out >> 16) & 0xff);
            return;
        }
        if (format.wBitsPerSample == 32) {
            double scaled = static_cast<double>(value) * 2147483647.0;
            int32_t out = static_cast<int32_t>(std::clamp(scaled, -2147483648.0, 2147483647.0));
            memcpy(sample, &out, sizeof(out));
            return;
        }
    }
}

float ReadMappedSample(const BYTE* source, const WAVEFORMATEX& sourceFormat, UINT32 frame, UINT32 outputChannel) {
    UINT32 sourceChannels = sourceFormat.nChannels;
    if (sourceChannels == 1) {
        return ReadSample(source, sourceFormat, frame, 0);
    }
    if (outputChannel < sourceChannels) {
        return ReadSample(source, sourceFormat, frame, outputChannel);
    }
    return ReadSample(source, sourceFormat, frame, sourceChannels - 1);
}

float ReadChannelConvertedSample(const BYTE* source, const WAVEFORMATEX& sourceFormat, UINT32 frame, UINT32 outputChannel, UINT32 outputChannels) {
    if (outputChannels == 1 && sourceFormat.nChannels > 1) {
        float sum = 0.0f;
        for (UINT32 channel = 0; channel < sourceFormat.nChannels; ++channel) {
            sum += ReadSample(source, sourceFormat, frame, channel);
        }
        return sum / static_cast<float>(sourceFormat.nChannels);
    }
    return ReadMappedSample(source, sourceFormat, frame, outputChannel);
}

UINT32 ConvertedFrameCount(UINT32 sourceFrames, const WAVEFORMATEX& sourceFormat, const WAVEFORMATEX& destinationFormat) {
    double ratio = static_cast<double>(destinationFormat.nSamplesPerSec) / static_cast<double>(sourceFormat.nSamplesPerSec);
    UINT32 frames = static_cast<UINT32>(sourceFrames * ratio + 0.5);
    return std::max<UINT32>(1, frames);
}

void ConvertSamples(BYTE* destination, UINT32 destinationFrames, const WAVEFORMATEX& destinationFormat,
    const BYTE* source, UINT32 sourceFrames, const WAVEFORMATEX& sourceFormat, float volume, bool silent) {
    size_t byteCount = static_cast<size_t>(destinationFrames) * destinationFormat.nBlockAlign;
    if (silent || volume <= 0.0f || sourceFrames == 0) {
        memset(destination, 0, byteCount);
        return;
    }

    double sourceRate = static_cast<double>(sourceFormat.nSamplesPerSec);
    double destinationRate = static_cast<double>(destinationFormat.nSamplesPerSec);
    UINT32 destinationChannels = destinationFormat.nChannels;

    for (UINT32 outFrame = 0; outFrame < destinationFrames; ++outFrame) {
        double sourcePosition = static_cast<double>(outFrame) * sourceRate / destinationRate;
        UINT32 frame0 = static_cast<UINT32>(sourcePosition);
        if (frame0 >= sourceFrames) {
            frame0 = sourceFrames - 1;
        }
        UINT32 frame1 = std::min(frame0 + 1, sourceFrames - 1);
        float blend = static_cast<float>(sourcePosition - frame0);

        for (UINT32 channel = 0; channel < destinationChannels; ++channel) {
            float a = ReadChannelConvertedSample(source, sourceFormat, frame0, channel, destinationChannels);
            float b = ReadChannelConvertedSample(source, sourceFormat, frame1, channel, destinationChannels);
            float value = (a + (b - a) * blend) * volume;
            WriteSample(destination, destinationFormat, outFrame, channel, value);
        }
    }
}

} // namespace

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() {
    Stop();
}

bool AudioEngine::IsRunning() const {
    return running_.load();
}

bool AudioEngine::Start(const std::vector<OutputSelection>& selectedDevices, StatusCallback callback, std::wstring* error) {
    if (error) {
        error->clear();
    }
    if (running_.load()) {
        if (error) {
            *error = L"Already running.";
        }
        return false;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    if (selectedDevices.empty()) {
        if (error) {
            *error = L"Select at least one playback device.";
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(volumeMutex_);
        volumes_ = selectedDevices;
    }
    stopRequested_.store(false);
    running_.store(true);
    worker_ = std::thread(&AudioEngine::Worker, this, selectedDevices, std::move(callback));
    return true;
}

void AudioEngine::Stop() {
    stopRequested_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false);
}

void AudioEngine::Publish(StatusCallback& callback, const std::wstring& text) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (callback) {
        callback(text);
    }
}

void AudioEngine::SetDeviceVolume(const std::wstring& deviceId, float volume) {
    std::lock_guard<std::mutex> lock(volumeMutex_);
    for (auto& item : volumes_) {
        if (item.id == deviceId) {
            item.volume = std::clamp(volume, 0.0f, 1.0f);
            return;
        }
    }
    volumes_.push_back({ deviceId, std::clamp(volume, 0.0f, 1.0f) });
}

float AudioEngine::VolumeForDevice(const std::wstring& deviceId) const {
    std::lock_guard<std::mutex> lock(volumeMutex_);
    for (const auto& item : volumes_) {
        if (item.id == deviceId) {
            return std::clamp(item.volume, 0.0f, 1.0f);
        }
    }
    return 1.0f;
}

void AudioEngine::Worker(std::vector<OutputSelection> selectedDevices, StatusCallback callback) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comInitialized = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        Publish(callback, L"Error: cannot initialize COM: " + HResultText(hr));
        running_.store(false);
        return;
    }

    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    ComPtr<IAudioEndpointVolume> defaultEndpointVolume;
    BOOL previousDefaultMute = FALSE;
    bool restoreDefaultMute = false;

    auto cleanup = [&]() {
        if (restoreDefaultMute && defaultEndpointVolume) {
            defaultEndpointVolume->SetMute(previousDefaultMute, nullptr);
        }
        if (mmcss) {
            AvRevertMmThreadCharacteristics(mmcss);
        }
        if (comInitialized) {
            CoUninitialize();
        }
        running_.store(false);
    };

    ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator.put()));
    if (FAILED(hr)) {
        Publish(callback, L"Error: cannot create audio enumerator: " + HResultText(hr));
        cleanup();
        return;
    }

    ComPtr<IMMDevice> captureDevice;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, captureDevice.put());
    if (FAILED(hr)) {
        Publish(callback, L"Error: cannot open default playback device: " + HResultText(hr));
        cleanup();
        return;
    }

    wchar_t* defaultIdRaw = nullptr;
    std::wstring defaultId;
    if (SUCCEEDED(captureDevice->GetId(&defaultIdRaw)) && defaultIdRaw) {
        defaultId = defaultIdRaw;
        CoTaskMemFree(defaultIdRaw);
    }

    bool defaultSelected = false;
    for (const auto& selection : selectedDevices) {
        if (selection.id == defaultId) {
            defaultSelected = true;
            break;
        }
    }

    hr = captureDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(defaultEndpointVolume.put()));
    if (FAILED(hr)) {
        Publish(callback, L"Error: cannot control default device volume: " + HResultText(hr));
        cleanup();
        return;
    }

    if (SUCCEEDED(defaultEndpointVolume->GetMute(&previousDefaultMute))) {
        restoreDefaultMute = true;
    } else {
        Publish(callback, L"Error: cannot read default device mute state.");
        cleanup();
        return;
    }

    ComPtr<IAudioClient> captureClient;
    hr = captureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(captureClient.put()));
    if (FAILED(hr)) {
        Publish(callback, L"Error: cannot activate loopback capture: " + HResultText(hr));
        cleanup();
        return;
    }

    WAVEFORMATEX* rawMix = nullptr;
    hr = captureClient->GetMixFormat(&rawMix);
    if (FAILED(hr) || !rawMix) {
        Publish(callback, L"Error: cannot read default device mix format: " + HResultText(hr));
        cleanup();
        return;
    }
    WaveFormatPtr mixFormat(rawMix);

    hr = captureClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
        kRequestedDuration, 0, mixFormat.get(), nullptr);
    if (FAILED(hr)) {
        Publish(callback, L"Error: cannot start loopback capture: " + HResultText(hr));
        cleanup();
        return;
    }

    ComPtr<IAudioCaptureClient> capture;
    hr = captureClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(capture.put()));
    if (FAILED(hr)) {
        Publish(callback, L"Error: cannot access loopback capture service: " + HResultText(hr));
        cleanup();
        return;
    }

    std::vector<RenderEndpoint> outputs;
    for (const auto& selection : selectedDevices) {
        const auto& id = selection.id;
        if (id == defaultId) {
            continue;
        }

        RenderEndpoint endpoint;
        endpoint.id = id;
        endpoint.name = DeviceNameFromId(enumerator.get(), id);

        hr = enumerator->GetDevice(id.c_str(), endpoint.device.put());
        if (FAILED(hr)) {
            Publish(callback, L"Device disconnected: " + endpoint.name);
            cleanup();
            return;
        }

        hr = endpoint.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(endpoint.client.put()));
        if (FAILED(hr)) {
            Publish(callback, L"Error opening " + endpoint.name + L": " + HResultText(hr));
            cleanup();
            return;
        }

        WAVEFORMATEX* endpointMixRaw = nullptr;
        hr = endpoint.client->GetMixFormat(&endpointMixRaw);
        if (FAILED(hr) || !endpointMixRaw) {
            Publish(callback, L"Error reading mix format for " + endpoint.name + L": " + HResultText(hr));
            cleanup();
            return;
        }
        endpoint.mixFormat.reset(endpointMixRaw);

        if (!IsConvertibleFormat(*mixFormat) || !IsConvertibleFormat(*endpoint.mixFormat)) {
            Publish(callback, L"Error: " + endpoint.name + L" uses an unsupported audio format.");
            cleanup();
            return;
        }

        WAVEFORMATEX* closestRaw = nullptr;
        hr = endpoint.client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, endpoint.mixFormat.get(), &closestRaw);
        WaveFormatPtr closest(closestRaw);
        if (FAILED(hr)) {
            Publish(callback, L"Error: " + endpoint.name + L" does not support its mix format: " + HResultText(hr));
            cleanup();
            return;
        }

        hr = endpoint.client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kRequestedDuration, 0, endpoint.mixFormat.get(), nullptr);
        if (FAILED(hr)) {
            Publish(callback, L"Error initializing " + endpoint.name + L": " + HResultText(hr));
            cleanup();
            return;
        }

        hr = endpoint.client->GetBufferSize(&endpoint.bufferFrames);
        if (FAILED(hr)) {
            Publish(callback, L"Error reading buffer size for " + endpoint.name + L": " + HResultText(hr));
            cleanup();
            return;
        }

        hr = endpoint.client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(endpoint.render.put()));
        if (FAILED(hr)) {
            Publish(callback, L"Error accessing render service for " + endpoint.name + L": " + HResultText(hr));
            cleanup();
            return;
        }

        outputs.push_back(std::move(endpoint));
    }

    if (outputs.empty()) {
        Publish(callback, L"Running: default device is already playing; no extra device selected.");
    } else if (!defaultSelected) {
        Publish(callback, L"Running: default device muted while active.");
    } else {
        Publish(callback, L"Running");
    }

    for (auto& output : outputs) {
        hr = output.client->Start();
        if (FAILED(hr)) {
            Publish(callback, L"Error starting " + output.name + L": " + HResultText(hr));
            cleanup();
            return;
        }
    }

    hr = captureClient->Start();
    if (FAILED(hr)) {
        Publish(callback, L"Error starting loopback capture: " + HResultText(hr));
        cleanup();
        return;
    }

    if (defaultEndpointVolume) {
        hr = defaultEndpointVolume->SetMute(defaultSelected ? FALSE : TRUE, nullptr);
        if (FAILED(hr)) {
            captureClient->Stop();
            for (auto& output : outputs) {
                output.client->Stop();
            }
            Publish(callback, L"Error setting default device mute state: " + HResultText(hr));
            cleanup();
            return;
        }
    }

    while (!stopRequested_.load()) {
        UINT32 packetFrames = 0;
        hr = capture->GetNextPacketSize(&packetFrames);
        if (FAILED(hr)) {
            Publish(callback, L"Device disconnected or capture failed: " + HResultText(hr));
            break;
        }

        if (packetFrames == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr)) {
            Publish(callback, L"Device disconnected or capture buffer failed: " + HResultText(hr));
            break;
        }

        bool packetOk = true;
        float defaultVolume = 1.0f;
        if (!defaultSelected && defaultEndpointVolume) {
            float scalar = 1.0f;
            if (SUCCEEDED(defaultEndpointVolume->GetMasterVolumeLevelScalar(&scalar))) {
                defaultVolume = std::clamp(scalar, 0.0f, 1.0f);
            }
        }

        for (auto& output : outputs) {
            UINT32 outputFrames = ConvertedFrameCount(frames, *mixFormat, *output.mixFormat);
            while (!stopRequested_.load()) {
                UINT32 padding = 0;
                hr = output.client->GetCurrentPadding(&padding);
                if (FAILED(hr)) {
                    Publish(callback, L"Device disconnected: " + output.name);
                    packetOk = false;
                    break;
                }

                UINT32 available = output.bufferFrames > padding ? output.bufferFrames - padding : 0;
                if (available >= outputFrames) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }

            if (!packetOk || stopRequested_.load()) {
                break;
            }

            BYTE* renderData = nullptr;
            hr = output.render->GetBuffer(outputFrames, &renderData);
            if (FAILED(hr)) {
                Publish(callback, L"Device disconnected: " + output.name);
                packetOk = false;
                break;
            }

            ConvertSamples(renderData, outputFrames, *output.mixFormat, data, frames, *mixFormat,
                VolumeForDevice(output.id) * defaultVolume, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);

            hr = output.render->ReleaseBuffer(outputFrames, 0);
            if (FAILED(hr)) {
                Publish(callback, L"Device disconnected: " + output.name);
                packetOk = false;
                break;
            }
        }

        capture->ReleaseBuffer(frames);

        if (!packetOk) {
            break;
        }
    }

    captureClient->Stop();
    for (auto& output : outputs) {
        output.client->Stop();
    }

    if (stopRequested_.load()) {
        Publish(callback, L"Idle");
    }
    cleanup();
}
