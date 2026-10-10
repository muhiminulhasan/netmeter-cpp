<p align="center">
  <img src="NetMeter.png" alt="NetMeter icon" width="96">
</p>

<h1 align="center">NetMeter</h1>

A small Windows network speed meter that displays current download and upload rates beside the taskbar notification area.

## Features

- Shows download and upload rates in the taskbar area.
- Uses the Windows taskbar theme for readable text.
- Recreates its taskbar meter and notification-area icon after Windows Explorer restarts.
- Provides an **Exit Network Meter** command from the notification-area icon's right-click menu.
- Uses Windows interface counters and does not require a third-party library.
- Statically links the MSVC runtime, so the Release executable does not require `VCRUNTIME140.dll` or `MSVCP140.dll` to be installed separately.

## Requirements

- Windows 10 or Windows 11
- For building: Visual Studio 2022 Build Tools (or Visual Studio) with the C++ workload, CMake, and a Windows SDK

## Build

Run these commands from the project directory in PowerShell or a Visual Studio developer terminal:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The executable is created at `build/Release/NetMeter.exe`.

## Run

Run `NetMeter.exe`. The meter appears beside the notification area, and the app's icon may be in the taskbar's hidden-icons menu. Right-click the icon and select **Exit Network Meter** to close it.

## How the rate is measured

NetMeter asks Windows which network interface it would use to reach `1.1.1.1`, then samples that interface's received and sent byte counters every 500 ms. It converts the counter differences to bits per second and displays decimal units (`1 Mbps = 1,000,000 bits per second`).

The reading is total traffic on the selected interface, including other apps and background traffic. It is not limited to a particular process. With split-tunnel VPNs or multiple routes, Windows may route some traffic through another interface, so the meter may not include that traffic.

## Project files

- `netmeter.cpp` — Windows application source
- `NetMeter.ico`, `NetMeter.png`, and `NetMeter.rc` — bundled network icon for the tray, executable, and README
- `CMakeLists.txt` — CMake build configuration
