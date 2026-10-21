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
`ak.Sys` was designed in 1999–2002 with fixed-pixel bitmaps that appear small on modern displays. This shim adds a complete 32-bit `DIBSection` offscreen backbuffer and halftone GDI scaling engine that makes every `ak.Sys` editor window freely resizable and zoomable.

### Key Capabilities
* **Universal Multi-Machine Support (S5000, S6000, Z4, Z8, MPC4000)**: Automatically enumerates all connected Akai Professional USB devices (`VID_09E8` across all Product IDs and custom Zadig `DeviceInterfaceGUIDs`).
* **Floating Zoom Control Bar (`1.0x` – `2.0x` + Aspect Ratio Lock)**: Runs on its own dedicated UI message pump thread so it never stalls during heavy USB sample transfers.
* **Direct Shell Drop Bridge (v15.5 Fix)**: Intercepts the drop target window in Windows Explorer (or Desktop) and directly delivers the downloaded `.WAV`, `.AKP`, and `.AKM` files to the target directory.
* **Remote Panel LCD Viewers (S5000 / S6000 / Z4 / Z8 / MPC4000)**: Handles the complete LCD bitmap stream with multi-chunk bulk packet reassembly, strips out raw `RGB(255, 0, 255)` magenta chroma-key corners, and applies a geometrically scaled rounded bezel region.
* **Zero Disk I/O Overhead**: All diagnostic file logging is completely stripped out to maximize USB throughput.