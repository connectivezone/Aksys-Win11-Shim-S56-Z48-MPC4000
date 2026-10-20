# Akai S5000 / S6000 / Z4 / Z8 / MPC4000 — 64-bit Windows WinUSB Proxy Shim & UI Scaler (v15.0)

[![Windows Support](https://img.shields.io/badge/Windows-7%20%7C%2010%20%7C%2011%20(64--bit)-0078D6?logo=windows&logoColor=white)](https://github.com/connectivezone/Aksys-shim-with-resizable-ui-no-logging)
[![ak.Sys Compatibility](https://img.shields.io/badge/ak.Sys-v1.53%20%E2%80%93%20v2.53%20Compatible-brightgreen)](https://github.com/connectivezone/Aksys-shim-with-resizable-ui-no-logging)
[![Hardware](https://img.shields.io/badge/Hardware-S5000%20%7C%20S6000%20%7C%20Z4%20%7C%20Z8%20%7C%20MPC4000-crimson)](https://github.com/connectivezone/Aksys-shim-with-resizable-ui-no-logging)
[![UI Scaler](https://img.shields.io/badge/UI%20Scaler-1.0x--2.0x%20%2B%20Free%20Resize-00b4d8)](https://github.com/connectivezone/Aksys-shim-with-resizable-ui-no-logging)
[![Build](https://img.shields.io/badge/Build-v15.0%20Universal%20Release-success)](https://github.com/connectivezone/Aksys-shim-with-resizable-ui-no-logging/releases)
[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

A lightweight 32-bit user-mode WinUSB proxy shim and UI scaler (`version.dll`) that allows **Akai ak.Sys (v1.53 – v2.53)** to run seamlessly on **64-bit Windows 11, Windows 10, and Windows 7** with the **Akai S5000, S6000, Z4, Z8, and MPC4000**—without custom kernel drivers, without disabling Windows Driver Signature Enforcement, and without test-signing mode.

In addition to restoring plug-and-play USB communication on modern 64-bit Windows, **v15.0** upgrades `ak.Sys` with **real-time resizable editor windows**, **floating zoom controls**, and **simultaneous multi-sampler support**.

---

## ⚡ The Problems Solved

### 1. 64-Bit USB Connectivity Without Unsigned Kernel Drivers
The original 1999–2004 Akai USB drivers (`akS56USB.sys` for S5000/S6000 and `akZ48USB.sys` for Z4/Z8/MPC4000) are legacy 32-bit Windows 98/2000/XP WDM kernel drivers. Modern 64-bit Windows versions strictly enforce 64-bit kernel driver signing, preventing those legacy `.sys` drivers from loading.

Rather than installing unsigned 64-bit third-party kernel drivers or putting Windows into insecure test-signing modes, this project leverages **Microsoft's built-in, digitally signed `WinUSB` (`winusb.sys`) driver** for direct hardware communication. The shim sits in user space and transparently intercepts legacy Win32 and SetupAPI I/O calls inside `S56Device.dll`, `Z48Device.dll`, and any other plugin DLL inside `aksys\devices\` via dynamic Import Address Table (IAT) hooks and API trampolines.

### 2. Resizable & Scalable UI for Modern Displays
`ak.Sys` was designed in 1999–2002 with fixed-pixel bitmaps that appear small on modern displays, and its sub-windows could not be resized. This shim adds a 32-bit `DIBSection` offscreen backbuffer and halftone GDI scaling engine that makes every `ak.Sys` editor window freely resizable and zoomable.

---

## ✨ Key Capabilities (v15.0 Universal Release)

* **Universal Multi-Machine Support (S5000, S6000, Z4, Z8, MPC4000)**:
  * Automatically enumerates all connected Akai Professional USB devices (`VID_09E8` across all Product IDs and custom Zadig `DeviceInterfaceGUIDs`).
  * Dynamically queries USB interface descriptors to discover the exact Bulk IN, Bulk OUT, and Interrupt endpoints per unit.
  * Supports **up to 16 Akai samplers connected simultaneously** (e.g., an S6000 and an MPC4000 or Z8 running side-by-side in the same `ak.Sys` session).
* **Floating Zoom Control Bar (`1.0x` – `2.0x` + Aspect Ratio Lock)**:
  * Runs on its own dedicated UI message pump thread so it never stalls during heavy USB sample transfers.
  * Includes one-click presets (`1.0x`, `1.25x`, `1.5x` default, `1.75x`, `2.0x`) and a **Lock Original Aspect Ratio When Resizing** toggle for manual window border dragging.
* **Remote Panel LCD Viewers (S5000 / S6000 / Z4 / Z8 / MPC4000)**:
  * Handles the complete LCD bitmap stream with multi-chunk 64-byte bulk packet reassembly, strips out raw `RGB(255, 0, 255)` magenta chroma-key corners, and applies a geometrically scaled rounded bezel region tailored to both S5000/S6000 and Z4/Z8/MPC4000 panels.
* **Program, Multi/Mixer & 4-Bus FX Editors**:
  * Scales all synthesis racks (*Common/Program Tune*, *Keyspan Piano Roll*, *Keygroup Filter/LFOs/Q-Links*, and *Zones/Envelopes/Modulation*), 32-channel Multi Mixers (`1600×522` base), and 4-channel EB20 / Z48 / MPC4000 FX editors with zero scrollbar clipping or black overhang bars.
  * Automatically repairs stuck or collapsed Program Editor layout keys in `HKCU\Software\AKSys\ak.Sys` across `S56`, `Z48`, and `MPC4000` registry trees.
* **Scaled Owner-Drawn Dropdown Menus & 100% Accurate Mouse Mapping**:
  * Intercepts `WM_MEASUREITEM` and `WM_DRAWITEM` to scale popup dropdown menus (`MIDI 1A..16B`, `OUTPUT`, `FX CHAN`, `Themes ▼`, filter types, Q-Link targets, etc.).
  * Inverse-maps mouse coordinates (`ScreenToClient`, `ClientToScreen`, `WM_LBUTTONDOWN`, `WM_MOUSEMOVE`) so rotary knobs, sliders, envelope handles, and soft-keys respond at their exact scaled screen positions.
* **Zero Disk I/O Overhead**:
  * All diagnostic file logging is completely stripped out in this production release to maximize USB throughput and eliminate disk wear.

---

## 📦 Compatibility Matrix

| Hardware / Environment | Supported | Notes / Requirements |
| :--- | :---: | :--- |
| **Akai S5000 / S6000** | ✅ | Requires the **IB-S56USB** expansion board installed |
| **Akai Z4 / Z8** | ✅ | Built-in USB device port (`Z48Device.dll`) |
| **Akai MPC4000** | ✅ | Built-in USB device port (`Z48Device.dll`) |
| **Akai ak.Sys** | ✅ | Compatible with **v1.53 through v2.53** (x86 32-bit application) |
| **Windows 11 (64-bit)** | ✅ | Bind USB device to **WinUSB** via Device Manager or [Zadig](https://zadig.akeo.ie/) |
| **Windows 10 (64-bit)** | ✅ | Bind USB device to **WinUSB** via Device Manager or [Zadig](https://zadig.akeo.ie/) |
| **Windows 7 (64-bit)** | ✅ | Bind USB device to **WinUSB** via [Zadig](https://zadig.akeo.ie/) |

---

## 🚀 Quick Start (Pre-Built Binary)

If you downloaded `version.dll` from the [Releases](https://github.com/connectivezone/Aksys-shim-with-resizable-ui-no-logging/releases) tab:

### Step 1: Bind Your Akai Sampler / MPC4000 to Microsoft WinUSB

#### Option A: One-Click Setup with Zadig (Recommended for All Models)
1. Power on your **S5000, S6000, Z4, Z8, or MPC4000** and connect the USB cable to your PC.
2. Download and run **[Zadig](https://zadig.akeo.ie/)** as Administrator.
3. Click **Options** &rarr; **List All Devices** and select your Akai unit (Vendor ID `09E8`, e.g., `AKAI S5000/S6000 USB`, `AKAI Z4/Z8`, or `AKAI MPC4000`).
4. Select **WinUSB (`v6.x...`)** as the target driver and click **Install Driver** (or **Replace Driver**).

#### Option B: Built-In Windows 10 / 11 Device Manager (No External Tools)
1. Power on your Akai machine and connect the USB cable to your PC.
2. Press `Win + X` and select **Device Manager**.
3. Under **Other devices**, locate your Akai sampler or MPC4000 (with a yellow warning icon).
4. Right-click the device and choose **Update driver** &rarr; **Browse my computer for drivers** &rarr; **Let me pick from a list of available drivers on my computer**.
5. Select **Universal Serial Bus devices** (near the bottom) and click **Next**.
6. Select:
   * **Manufacturer**: `WinUsb Device`
   * **Model**: `WinUsb Device`
7. Click **Next**, click **Yes** to confirm, and finish the installation.

### Step 2: Copy `version.dll` into `ak.Sys`

1. Ensure `aksys.exe` is closed.
2. Copy `version.dll` into the `devices` subfolder of your `ak.Sys` installation directory (default path):
   ```text
   C:\Program Files (x86)\AKAIPRO\aksys\devices\version.dll
   ```
3. Copy `version.dll` into the root directory alongside `aksys.exe`:
   ```text
   C:\Program Files (x86)\AKAIPRO\aksys\version.dll
   ```

#### Directory Layout Verification
```text
C:\Program Files (x86)\AKAIPRO\aksys\
├── aksys.exe
├── AKComCtrls.dll
├── version.dll                  <-- REQUIRED: Root proxy & UI Scaler
└── devices\
    ├── S56Device.dll            <-- Original Akai S5000/S6000 module (KEEP THIS!)
    ├── Z48Device.dll            <-- Original Akai Z4/Z8/MPC4000 module (KEEP THIS!)
    └── version.dll              <-- REQUIRED: Device proxy
```

### Step 3: Launch `ak.Sys` & Use the Zoom Controls
1. Power on your sampler(s) and start `ak.Sys`.
2. Your **S5000, S6000, Z4, Z8, or MPC4000** will appear in the device browser tree ready for sample transfers, program editing, and Remote Panel control.
3. Use the floating **Zoom Controls** toolbar to switch between `1.0x`, `1.25x`, `1.5x` (default), `1.75x`, and `2.0x` scaling, or drag the borders/corners of any sub-window to resize freely.

---

## 🔬 How It Works (Technical Overview)

The shim employs user-mode DLL sideloading via the system `version.dll` library name:

```text
[aksys.exe / S56Device.dll / Z48Device.dll / AKComCtrls.dll / MFC42.dll]
          │
          ▼ (Loads local version.dll proxy)
   ┌────────────────────────────────────────────────────────────────────┐
   │                   version.dll Shim (v15.0)                         │
   │  - Export forwarding to C:\Windows\SysWOW64\version.dll            │
   │  - Multi-Device USB Engine: Auto-discovers VID_09E8 units & pipes  │
   │  - USB IAT Hooking: CreateFileA, ReadFile, WriteFile,              │
   │                     DeviceIoControl, CancelIo, SetupAPI            │
   │  - UI / GDI Hooking: BitBlt, GetClientRect, InvalidateRect,        │
   │                      SetWindowRgn, ScreenToClient, ClientToScreen, │
   │                      WM_MEASUREITEM, WM_DRAWITEM                   │
   │  - 32-Bit DIBSection Backbuffer + Halftone Stretch Engine          │
   │  - 64 KB Read Buffer + Multi-Packet LCD Frame Assembly             │
   └───────────────────────────────┬────────────────────────────────────┘
                                   │
                                   ▼ (Translates to WinUSB calls)
                           [winusb.sys (Kernel)]
                                   │
                                   ▼ (Auto-Detected Bulk IN / Bulk OUT)
            [Akai S5000 / S6000 / Z4 / Z8 / MPC4000 Hardware]
```

1. **DLL Proxy & Dynamic Plugin Hooking**: `ak.Sys` and its device modules link Windows versioning APIs (`GetFileVersionInfoA`, `VerQueryValueA`, etc.). Placing `version.dll` in the application and `devices` folders loads the proxy shim automatically. The watcher thread dynamically scans `aksys\devices\*.dll` to patch the IAT of `S56Device.dll`, `Z48Device.dll`, and any other loaded Akai device modules.
2. **Universal SetupAPI & Pipe Emulation**: `S56Device.dll` and `Z48Device.dll` query SetupAPI for legacy Akai device interfaces and open endpoint handles using suffixes like `\PIPE00`, `\PIPE01`, `\PIPE_00`, or `\PIPE_01`. The shim intercepts `SetupDiGetClassDevsA`, `SetupDiEnumDeviceInterfaces`, `SetupDiGetDeviceInterfaceDetailA`, and `CreateFileA`, routing each virtual pipe handle to the matching `WinUSB` Bulk IN / Bulk OUT pipe on the corresponding hardware unit.
3. **Overlapped Completion & Framebuffer Assembly**: Emulates kernel-style asynchronous overlapped behavior (`STATUS_PENDING` / `ERROR_IO_PENDING`) and aggregates multi-packet 64-byte bulk transfers into a 64 KB buffer so LCD remote screen streams and large `.WAV` sample transfers never drop packets.
4. **Offscreen Backbuffer & UI Scaler**: Subclasses `AfxFrameOrView42` editor frames and their inner `Afx:` canvases. `Hook_GetClientRect` reports 1x native dimensions back to `ak.Sys` so its internal layout engine renders cleanly into a persistent 32-bit top-down `DIBSection` backbuffer (`hBackDC`), which is then chroma-cleaned and stretched to the resized window via `StretchBlt(..., HALFTONE)`. Mouse and menu coordinates are inverse-scaled in real time.

---

## 🛠️ Building from Source

The shim compiles into a 32-bit x86 DLL using the Microsoft Visual C++ compiler (`cl.exe`).

### Prerequisites
* Microsoft Visual Studio (2017, 2019, 2022, or 2026 Build Tools) with Desktop C++ (x86) components.
* Windows SDK (providing `winusb.lib`, `setupapi.lib`, `gdi32.lib`, `user32.lib`).

### Build Steps

1. Open an **Administrator Command Prompt** (or **x86 Native Tools Command Prompt for VS**—the build script automatically locates `vcvars32.bat`).
2. Clone the repository:
   ```bash
   git clone https://github.com/connectivezone/Aksys-shim-with-resizable-ui-no-logging.git
   cd Aksys-shim-with-resizable-ui-no-logging
   ```
3. Run the automated build & install script (which closes any running `aksys.exe`, resets stuck collapsed editor keys in `HKCU\Software\AKSys\ak.Sys` for S56/Z48/MPC4000, compiles `version.dll`, and copies it into both `ak.Sys` directories):
   ```cmd
   .\build_shim.bat
   ```
   Or invoke the MSVC compiler directly from an **x86 Native Tools Command Prompt**:
   ```cmd
   cl.exe /nologo /O2 /LD /EHsc akS56USB_WinUSB_Shim.cpp /link /OUT:version.dll /MACHINE:X86 setupapi.lib winusb.lib user32.lib gdi32.lib advapi32.lib ole32.lib
   ```

---

## ❓ Troubleshooting

* **Sampler / MPC4000 not showing in `ak.Sys`**:
  * Ensure you are running **`ak.Sys` v2.53** if you are using a **Z4, Z8, or MPC4000** (v1.53 only includes `S56Device.dll`, whereas v2.53 includes both `S56Device.dll` and `Z48Device.dll`).
  * Verify in Device Manager (or Zadig) that your Akai unit (`VID_09E8`) is bound to `WinUSB`.
  * Verify that `version.dll` is present in both `C:\Program Files (x86)\AKAIPRO\aksys\` and `C:\Program Files (x86)\AKAIPRO\aksys\devices\`.
* **Machine displays "USB Error" on its front-panel LCD**:
  * If the machine previously encountered a timeout from an aborted session, clear the prompt using the front-panel soft keys or power-cycle the unit before reopening `ak.Sys`.
* **Program Editor opens showing only the top row of knobs**:
  * Click **`Themes ▼`** in the top-left corner of the Program Editor's header bar to toggle the lower synthesis racks on, or run `build_shim.bat` to automatically reset stuck Registry layout flags in `HKCU\Software\AKSys\ak.Sys`.

---

## 📄 License & Disclaimer

This project is open-source software distributed under the [MIT License](LICENSE).

*Akai, Akai Professional, S5000, S6000, Z4, Z8, MPC4000, and ak.Sys are trademarks or registered trademarks of their respective owners. This project is an independent community compatibility initiative and is not affiliated with, endorsed by, or associated with inMusic Brands or Akai Professional.*