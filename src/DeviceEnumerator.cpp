#include "DeviceEnumerator.h"

#include "ComPtr.h"

#include <Windows.h>
#include <Mmdeviceapi.h>
#include <Objbase.h>
#include <Propsys.h>

#include <algorithm>

namespace {

constexpr PROPERTYKEY kPKEYDeviceFriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } },
    14
};

std::wstring HResultText(HRESULT hr) {
    wchar_t buffer[64] = {};
    swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(hr));
    return buffer;
}

std::wstring DeviceName(IMMDevice* device) {
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, store.put()))) {
        return L"Unknown playback device";
    }

    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name = L"Unknown playback device";
    if (SUCCEEDED(store->GetValue(kPKEYDeviceFriendlyName, &value)) &&
        value.vt == VT_LPWSTR && value.pwszVal) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    return name;
}

std::wstring DeviceId(IMMDevice* device) {
    wchar_t* rawId = nullptr;
    std::wstring id;
    if (SUCCEEDED(device->GetId(&rawId)) && rawId) {
        id = rawId;
        CoTaskMemFree(rawId);
    }
    return id;
}

} // namespace

std::wstring GetDefaultPlaybackDeviceId(std::wstring* error) {
    if (error) {
        error->clear();
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator.put()));
    if (FAILED(hr)) {
        if (error) {
            *error = L"Cannot create audio device enumerator: " + HResultText(hr);
        }
        return {};
    }

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put());
    if (FAILED(hr)) {
        if (error) {
            *error = L"Cannot find the default playback device: " + HResultText(hr);
        }
        return {};
    }

    return DeviceId(device.get());
}

std::vector<PlaybackDeviceInfo> EnumeratePlaybackDevices(std::wstring* error) {
    if (error) {
        error->clear();
    }

    std::vector<PlaybackDeviceInfo> devices;
    std::wstring defaultId = GetDefaultPlaybackDeviceId(nullptr);

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator.put()));
    if (FAILED(hr)) {
        if (error) {
            *error = L"Cannot create audio device enumerator: " + HResultText(hr);
        }
        return devices;
    }

    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.put());
    if (FAILED(hr)) {
        if (error) {
            *error = L"Cannot enumerate playback devices: " + HResultText(hr);
        }
        return devices;
    }

    UINT count = 0;
    collection->GetCount(&count);
    devices.reserve(count);

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.put()))) {
            continue;
        }

        PlaybackDeviceInfo info;
        info.id = DeviceId(device.get());
        info.name = DeviceName(device.get());
        info.isDefault = !defaultId.empty() && info.id == defaultId;
        devices.push_back(std::move(info));
    }

    return devices;
}
