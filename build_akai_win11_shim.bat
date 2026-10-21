@echo off
setlocal
cd /d "%~dp0"
echo ============================================================================
echo  Akai S5000 / S6000 / Z4 / Z8 / MPC4000 WinUSB + UI Scaler Builder
echo  Version: v15.5 Universal (Direct Shell Drop Bridge + Multi-Sampler Support)
echo ============================================================================
echo.

echo [*] Closing any running aksys.exe instances to avoid DLL lock...
taskkill /F /IM aksys.exe >nul 2>nul

echo [*] Removing any old diagnostic log at C:\Users\Public\akai_shim_debug.log...
if exist "C:\Users\Public\akai_shim_debug.log" del /F /Q "C:\Users\Public\akai_shim_debug.log" >nul 2>nul

echo [*] Resetting stuck collapsed editor keys in HKCU\Software\AKSys\ak.Sys...
reg delete "HKCU\Software\AKSys\ak.Sys\S56" /f >nul 2>nul
reg delete "HKCU\Software\AKSys\ak.Sys\Z48" /f >nul 2>nul
reg delete "HKCU\Software\AKSys\ak.Sys\MPC4000" /f >nul 2>nul
reg delete "HKCU\Software\AKSys\ak.Sys\AKAI S6000" /f >nul 2>nul
reg delete "HKCU\Software\AKSys\ak.Sys\AKAI S5000" /f >nul 2>nul
reg delete "HKCU\Software\AKSys\ak.Sys\AKAI Z4" /f >nul 2>nul
reg delete "HKCU\Software\AKSys\ak.Sys\AKAI Z8" /f >nul 2>nul
reg delete "HKCU\Software\AKSys\ak.Sys\AKAI MPC4000" /f >nul 2>nul
reg delete "HKCU\Software\AKSys\ak.Sys\Demo S56K 1" /f >nul 2>nul
reg delete "HKCU\Software\AKSys\ak.Sys\Demo S56K 2" /f >nul 2>nul
reg delete "HKCU\Software\AKSys\ak.Sys\aksys" /v edlistWP /f >nul 2>nul

if exist "%LOCALAPPDATA%\VirtualStore\Program Files (x86)\AKAIPRO\aksys" (
    echo [*] Cleaning UAC VirtualStore cache for aksys...
    rd /S /Q "%LOCALAPPDATA%\VirtualStore\Program Files (x86)\AKAIPRO\aksys" >nul 2>nul
)

set "CPP_FILE=akS56USB_WinUSB_Shim.cpp"
if not exist "%CPP_FILE%" (
    echo [!] ERROR: Could not find akS56USB_WinUSB_Shim.cpp in:
    echo     %CD%
    pause
    exit /b 1
)

echo [*] Using C++ source file: "%CPP_FILE%"

where cl.exe >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo [*] Searching for Visual Studio x86 Native Tools environment...
    for %%Y in (2026 2022 2019 2017) do (
        for %%E in (Community Professional Enterprise BuildTools) do (
            if exist "%ProgramFiles%\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\vcvars32.bat" (
                call "%ProgramFiles%\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\vcvars32.bat"
                goto :compile
            )
            if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\vcvars32.bat" (
                call "%ProgramFiles(x86)%\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\vcvars32.bat"
                goto :compile
            )
        )
    )
)

:compile
where cl.exe >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo.
    echo [!] Could not locate cl.exe.
    echo [!] Please open 'x86 Native Tools Command Prompt for VS' and run this script again.
    pause
    exit /b 1
)

echo [*] Compiling 32-bit version.dll proxy (v15.5 Universal S56/Z48/MPC4000 Release)...
cl.exe /nologo /O2 /LD /EHsc "%CPP_FILE%" /link /OUT:version.dll /MACHINE:X86 setupapi.lib winusb.lib user32.lib gdi32.lib advapi32.lib ole32.lib shell32.lib shlwapi.lib oleaut32.lib
if %ERRORLEVEL% neq 0 (
    echo.
    echo [!] Compilation failed. Make sure you run this from an x86 Native Tools Command Prompt.
    pause
    exit /b 1
)

echo.
echo [+] SUCCESS! Built 32-bit version.dll (v15.5 Universal).
echo.

set "DEV_DIR=C:\Program Files (x86)\AKAIPRO\aksys\devices"
set "ROOT_DIR=C:\Program Files (x86)\AKAIPRO\aksys"

if not exist "%DEV_DIR%" goto :check_root
echo [*] Copying version.dll to "%DEV_DIR%"...
copy /Y "version.dll" "%DEV_DIR%\version.dll" >nul 2>nul
if %ERRORLEVEL% equ 0 (
    echo [+] Installed: "%DEV_DIR%\version.dll"
) else (
    echo [!] ERROR: Could not overwrite "%DEV_DIR%\version.dll" (Run Command Prompt as Administrator!).
)

:check_root
if not exist "%ROOT_DIR%" goto :instructions
echo [*] Copying version.dll to "%ROOT_DIR%"...
copy /Y "version.dll" "%ROOT_DIR%\version.dll" >nul 2>nul
if %ERRORLEVEL% equ 0 (
    echo [+] Installed: "%ROOT_DIR%\version.dll"
)

echo [*] Granting user write permissions to "%ROOT_DIR%" for Drag & Drop...
icacls "%ROOT_DIR%" /grant *S-1-5-32-545:(OI)(CI)M /T >nul 2>nul

:instructions
echo.
echo ============================================================================
echo  v15.5 UNIVERSAL PRODUCTION RELEASE READY (DIRECT SHELL DROP BRIDGE)
echo ============================================================================
pause