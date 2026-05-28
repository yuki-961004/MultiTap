# MultiTap

MultiTap is a tiny Windows 11 desktop app that duplicates the current system audio output to additional playback devices.

Typical use: keep your normal wired headphones as the Windows default output, open MultiTap, tick a Bluetooth headset or USB speaker, and click **Start**.

## What It Does

- Lists active Windows playback devices.
- Captures the default playback device with WASAPI loopback.
- Copies that audio stream to every selected non-default playback device.
- If the Windows default playback device is not selected, MultiTap temporarily mutes it while running and restores its previous mute state when stopped.
- Lets each non-default copied output use its own remembered ratio relative to the Windows default output volume.
- Includes a small light/dark theme toggle.
- Keeps the default playback device untouched, because Windows is already playing audio there.
- Saves the last selected devices in `%APPDATA%\MultiTap\config.txt`.

## What It Does Not Do

- No recording.
- No per-app routing.
- No mixer, EQ, effects, or volume controls.
- No virtual audio cable or driver installation.
- No ASIO, Voicemeeter, DAW, or external runtime.

## Build On Windows 11

Requirements:

- CMake
- Visual Studio with the C++ desktop toolchain

From a Developer PowerShell or Developer Command Prompt:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

The executable will be created at:

```text
build\Release\MultiTap.exe
```

The build also copies the latest release executable to:

```text
release\MultiTap.exe
```

This project is native C++ and Win32. It does not require C#, .NET, or NuGet packages. Release builds use the static MSVC runtime to make the `.exe` easier to run on other Windows 11 PCs.

## How To Use

1. Set your main listening device as the Windows default output.
2. Launch `MultiTap.exe`.
3. Tick one or more playback devices.
4. Select a non-default device in the list and adjust its ratio slider if needed.
5. Click **Start**.
6. Click **Stop** before disconnecting Bluetooth or USB devices when possible.

Only checked devices are intended to be audible. If the default playback device is unchecked, MultiTap uses it as the loopback capture source but temporarily mutes the endpoint itself.

The Windows default output volume acts as the main volume control. MultiTap's per-device slider is a remembered ratio, so a device set to 70% follows Windows volume at 70% of that level.

## Known Limitations

- Bluetooth latency is unavoidable. Bluetooth headphones may lag behind wired headphones.
- Different physical devices have independent clocks. Long sessions may drift slightly out of sync.
- MultiTap includes basic built-in conversion for common shared-mode formats: 32-bit float and 16/24/32-bit PCM, with simple linear resampling and basic channel mapping.
- Very unusual endpoint formats may still be rejected.
- Perfect synchronization across unrelated audio devices is not guaranteed by Windows shared-mode WASAPI.
- If a selected output device disconnects, MultiTap stops and reports the failed device.

## Technical Note

The audio pipeline is:

```text
Windows default playback endpoint
        |
        | WASAPI loopback capture, shared mode
        v
MultiTap audio thread
        |
        | copy captured PCM packets
        v
Selected non-default playback endpoints, WASAPI render, shared mode
```

MultiTap intentionally skips rendering back into the default playback device. Rendering loopback audio into the same endpoint being captured can create echo or recursive feedback, and the default endpoint is already audible because Windows is playing to it directly.

The audio thread uses bounded WASAPI endpoint buffers only. If an output device is too slow or disconnects, the app stops instead of building an unbounded queue.
