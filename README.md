# Akai S5000 / S6000 / Z4 / Z8 / MPC4000 — 64-bit Windows WinUSB Proxy Shim & UI Scaler (v15.5)

[![Windows Support](https://img.shields.io/badge/Windows-7%20%7C%2010%20%7C%2011%20(64--bit)-0078D6?logo=windows&logoColor=white)](https://github.com/connectivezone/Aksys-shim-with-resizable-ui-no-logging)
[![ak.Sys Compatibility](https://img.shields.io/badge/ak.Sys-v1.53%20--%20v2.53-brightgreen)](https://github.com/connectivezone/Aksys-shim-with-resizable-ui-no-logging)
[![Hardware](https://img.shields.io/badge/Hardware-S5000%20%7C%20S6000%20%7C%20Z4%20%7C%20Z8%20%7C%20MPC4000-crimson)](https://github.com/connectivezone/Aksys-shim-with-resizable-ui-no-logging)
[![UI Scaler](https://img.shields.io/badge/UI%20Scaler-1.0x--2.0x%20%2B%20Free%20Resize-00b4d8)](https://github.com/connectivezone/Aksys-shim-with-resizable-ui-no-logging)
[![Build](https://img.shields.io/badge/Build-v15.5%20Release%20%2F%20No--Log-success)](https://github.com/connectivezone/Aksys-shim-with-resizable-ui-no-logging/releases)
[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

A lightweight 32-bit user-mode WinUSB proxy shim and UI scaler (`version.dll`) that allows **Akai ak.Sys (v1.53 – v2.53)** to run seamlessly on **64-bit Windows 11, Windows 10, and Windows 7** with the **Akai S5000, S6000, Z4, Z8, and MPC4000**—without custom kernel drivers, without disabling Windows Driver Signature Enforcement, and without test-signing mode.

In addition to restoring plug-and-play USB communication on modern 64-bit Windows, **v15.5** upgrades `ak.Sys` with **real-time resizable editor windows**, **floating zoom controls**, **direct drag & drop file delivery to Windows Explorer folders and Desktop**, and **simultaneous multi-sampler support**.

---

## ⚡ The Problems Solved

### 1. 64-Bit USB Connectivity Without Unsigned Kernel Drivers
The original 1999–2004 Akai USB drivers (`akS56USB.sys` for S5000/S6000 and `akZ48USB.sys` for Z4/Z8/MPC4000) are legacy 32-bit Windows 98/2000/XP WDM kernel drivers. Modern 64-bit Windows strictly enforces kernel driver signatures, preventing those legacy drivers from loading.

This project leverages **Microsoft's built-in, digitally signed `WinUSB` (`winusb.sys`) driver** for direct hardware communication. The shim sits in user space and transparently intercepts legacy Win32 and SetupAPI I/O calls inside `S56Device.dll`, `Z48Device.dll`, and any other plugin DLL inside `aksys\devices\` via dynamic Import Address Table (IAT) hooks and API trampolines.

### 2. Resizable & Scalable UI
`ak.Sys` was designed in 1999–2002 with fixed-pixel bitmaps that appear tiny on modern displays. This shim adds a complete 32-bit `DIBSection` offscreen backbuffer and halftone GDI scaling engine that makes every `ak.Sys` editor window freely resizable and zoomable.

### Key Capabilities
* **Universal Multi-Machine Support (S5000, S6000, Z4, Z8, MPC4000)**: Automatically enumerates all connected Akai Professional USB devices (`VID_09E8` across all Product IDs and custom Zadig `DeviceInterfaceGUIDs`).
* **Floating Zoom Control Bar (`1.0x` – `2.0x` + Aspect Ratio Lock)**: Runs on its own dedicated UI message pump thread so it never stalls during heavy USB sample transfers.
* **Direct Shell Drop Bridge (v15.5 Fix)**: Intercepts the drop target window in Windows Explorer (or Desktop) and directly delivers the downloaded `.WAV`, `.AKP`, and `.AKM` files to the target directory.
* **Remote Panel LCD Viewers (S5000 / S6000 / Z4 / Z8 / MPC4000)**: Handles the complete LCD bitmap stream with multi-chunk bulk packet reassembly, strips out raw `RGB(255, 0, 255)` magenta chroma-key corners, and applies a geometrically scaled rounded bezel region.
* **Zero Disk I/O Overhead**: All diagnostic file logging is completely stripped out to maximize USB throughput.

---

## 📦 Compatibility Matrix

| Hardware | Requirement | Target Driver | ak.Sys Device Plugin |
| :--- | :--- | :--- | :--- |
| **Akai S5000** | IB-S56USB expansion card installed | WinUSB | `S56Device.dll` |
| **Akai S6000** | Standard USB port or IB-S56USB | WinUSB | `S56Device.dll` |
| **Akai Z4** | Built-in USB port | WinUSB | `Z48Device.dll` |
| **Akai Z8** | Built-in USB port | WinUSB | `Z48Device.dll` |
| **Akai MPC4000** | Built-in USB port | WinUSB | `Z48Device.dll` / `MPC4000Device.dll` |

| Operating System | Supported | Driver Method |
| :--- | :---: | :--- |
| **Windows 11 (64-bit)** | ✅ | Native Device Manager or Zadig |
| **Windows 10 (64-bit)** | ✅ | Native Device Manager or Zadig |
| **Windows 7 (64-bit)** | ✅ | Zadig USB installer |

---

## 🚀 Setup & Driver Installation

Follow these steps to configure your sampler with Microsoft's official WinUSB driver on 64-bit Windows.

### Method 1: Using Windows Device Manager (No Extra Software Required)

This is the standard native Windows 10/11 method:

1. **Connect & Power On**: Turn on your Akai sampler and plug the USB cable into your Windows PC.
2. **Open Device Manager**:
   - Press `Win + X` and select **Device Manager** (or press `Win + R`, type `devmgmt.msc`, and press Enter).
3. **Locate the Sampler**:
   - Look under **Other devices** (or **Universal Serial Bus controllers**).
   - Your sampler will typically appear with a yellow exclamation mark (`⚠️`) under one of the following names:
     - `AKAI S5000/S6000 USB` or `IB-S56USB`
     - `AKAI Z4` / `AKAI Z8` / `Akai Z Series`
     - `AKAI MPC4000` / `MPC4000`
     - `Unknown USB Device` (Hardware ID starting with `USB\VID_09E8`)
4. **Update Driver**:
   - Right-click the sampler entry and select **Update driver**.
   - Click **Browse my computer for drivers**.
   - Click **Let me pick from a list of available drivers on my computer**.
5. **Select Device Category**:
   - In the hardware type list, select **Universal Serial Bus devices** (near the bottom) and click **Next**.
   *(If prompted with a list of manufacturers right away, look for `WinUsb Device`)*.
6. **Select the WinUSB Driver**:
   - In the **Manufacturer** column, select **WinUsb Device**.
   - In the **Model** column, select **WinUsb Device**.
   - Click **Next**.
7. **Confirm Installation**:
   - If Windows displays an "Update Driver Warning" prompt stating that Windows cannot verify compatibility, click **Yes** to continue.
8. **Verify**:
   - The device will now appear under **Universal Serial Bus devices** as **WinUsb Device** with no warning icon.

---

### Method 2: Using Zadig (Fastest 1-Click Method for Win 11, 10 & 7)

If Device Manager does not show `WinUsb Device` in the manufacturer list, or if you are running 64-bit Windows 7, use Zadig:

1. Download the free, open-source tool **[Zadig](https://zadig.akeo.ie/)**.
2. Connect and power on your Akai sampler.
3. Launch `Zadig.exe`.
4. In the top menu bar, click **Options** &rarr; check **List All Devices**.
5. In the main dropdown box, select your Akai sampler:
   * **S5000 / S6000**: Shows as `AKAI S5000/S6000` or USB ID `09E8 005E`.
   * **Z4**: Shows as `AKAI Z4` or USB ID `09E8 006A`.
   * **Z8**: Shows as `AKAI Z8` or USB ID `09E8 006B`.
   * **MPC4000**: Shows as `AKAI MPC4000` or USB ID `09E8 006C`.
6. To the right of the green arrow, ensure the target driver is set to **WinUSB (v6.1.7600.16385 or newer)**.
7. Click **Replace Driver** (or **Install Driver**).
8. Wait a few seconds until the message *"The driver was installed successfully"* appears.

---

## 💻 Installing `version.dll`

Once your sampler is bound to WinUSB:

1. Close `aksys.exe` if it is currently running.
2. Copy `version.dll` into the `devices` subfolder of your ak.Sys installation:
   ```
   C:\Program Files (x86)\AKAIPRO\aksys\devices\version.dll
   ```
3. *(Recommended)* Also place a copy into the main directory alongside `aksys.exe`:
   ```
   C:\Program Files (x86)\AKAIPRO\aksys\version.dll
   ```

### Directory Verification
```
C:\Program Files (x86)\AKAIPRO\aksys\
├── aksys.exe
├── AKComCtrls.dll
├── version.dll                  <-- Recommended root proxy
└── devices\
    ├── S56Device.dll            <-- Original S5k/S6k driver module
    ├── Z48Device.dll            <-- Original Z4/Z8/MPC4k driver module
    └── version.dll              <-- REQUIRED: Drop shim here
```

---

## 🖱️ Drag-and-Drop Sample Transfers

With **v15.5**, bi-directional drag-and-drop works directly with Windows Explorer:

* **PC &rarr; Sampler**: Drag any `.WAV` sample or `.AKP` program from any folder on your PC directly onto the sampler memory tree or disk tree inside `ak.Sys`.
* **Sampler &rarr; PC**: Drag any sample or program icon out of `ak.Sys` and release it directly over any open **Windows File Explorer folder** or onto the **Desktop**. The shim intercepts the drop, stages the file, and immediately copies it into your destination directory.
* **Context Menu Alternative**: You can also right-click any item in `ak.Sys` and choose **Save** or **Save As...** to save files directly using the standard Windows Save dialog.

---

## 🔍 UI Scaling & Floating Toolbar

When `ak.Sys` starts, the shim launches a floating tool window: **`ak.sys Zoom Controls (v15.5)`**:
* **Preset Buttons**: Click **1.0x**, **1.25x**, **1.5x** (default), **1.75x**, or **2.0x** to rescale all open sub-windows simultaneously.
* **Manual Window Resizing**: Grab any edge or corner of an editor window (Program Editor, Remote LCD Panel, Multi Mixer, EB20 FX) and drag to resize freely.
* **Lock Original Aspect Ratio**: Keep this box checked to prevent the synthesis racks and LCD screens from stretching disproportionately during manual resizing.

---

## 🛠️ Building from Source

To compile `version.dll` yourself using Visual Studio:

1. Open an **Administrator x86 Native Tools Command Prompt for VS** (2017, 2019, 2022, or 2026).
2. Navigate to your project folder:
   ```cmd
   cd /d "C:\path\to\Aksys-shim-folder"
   ```
3. Run the automated build script:
   ```cmd
   .\build_akai_win11_shim.bat
   ```
   Or compile manually with `cl.exe`:
   ```cmd
   cl.exe /nologo /O2 /LD /EHsc akS56USB_WinUSB_Shim.cpp /link /OUT:version.dll /MACHINE:X86 setupapi.lib winusb.lib user32.lib gdi32.lib advapi32.lib ole32.lib shell32.lib shlwapi.lib oleaut32.lib
   ```
4. The batch file will compile `version.dll`, configure folder permissions, and copy the DLL into `aksys\` and `aksys\devices\`.

---

## ❓ Troubleshooting

* **Sampler not detected in ak.Sys**:
  * Open Device Manager and verify the sampler appears under **Universal Serial Bus devices** as `WinUsb Device` without a warning icon.
  * Check that `version.dll` exists inside `C:\Program Files (x86)\AKAIPRO\aksys\devices\`.
* **Sampler shows "USB Error" on its LCD screen**:
  * Clear the prompt using the sampler's front-panel soft keys, or turn the sampler off and on again before launching `ak.Sys`.
* **Program Editor opens with only the top knob bar visible**:
  * Click **`Themes ▼`** in the Program Editor header to expand the lower synthesis racks, or run `build_akai_win11_shim.bat` to clear collapsed layout registry flags in `HKCU\Software\AKSys\ak.Sys`.
* **Dropped files don't appear after dragging to Explorer**:
  * Make sure you drag to an active Windows Explorer folder window or your Desktop. If Windows UAC permissions block the drop, right-click the item in `ak.Sys` and use **Save As...**.

---

## 📄 License & Disclaimer

This project is an open-source community compatibility initiative distributed under the [MIT License](LICENSE).

*Akai, S5000, S6000, Z4, Z8, MPC4000, and ak.Sys are trademarks or registered trademarks of their respective owners. This project is not affiliated with or endorsed by inMusic Brands or Akai Professional.*