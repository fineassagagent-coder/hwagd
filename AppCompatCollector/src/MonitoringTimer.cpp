#include "MonitoringTimer.h"
#include <windows.h>
#include <dpapi.h>
#include <iostream>
#include <vector>
#include <chrono>

#pragma comment(lib, "Crypt32.lib")

static HANDLE g_hMonitorShutdownEvent = NULL;

// Get current UTC time as seconds since epoch
static uint64_t GetCurrentUtcEpoch() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    // Convert from 100-nanosecond intervals since 1601-01-01 to seconds since Unix epoch
    return (uli.QuadPart - 116444736000000000ULL) / 10000000ULL;
}

// Read DPAPI-protected install timestamp from registry
static uint64_t ReadInstallTimestamp() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\CompatRuntime",
                      0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return 0;

    DWORD size = 0;
    RegGetValueW(hKey, NULL, L"InstallTimeBlob", RRF_RT_REG_BINARY, NULL, NULL, &size);
    if (size == 0) { RegCloseKey(hKey); return 0; }

    std::vector<BYTE> protectedBlob(size);
    RegGetValueW(hKey, NULL, L"InstallTimeBlob", RRF_RT_REG_BINARY, NULL, protectedBlob.data(), &size);
    RegCloseKey(hKey);

    DATA_BLOB dataIn = { size, protectedBlob.data() };
    DATA_BLOB dataOut;
    if (CryptUnprotectData(&dataIn, NULL, NULL, NULL, NULL, 0, &dataOut)) {
        uint64_t timestamp = 0;
        if (dataOut.cbData >= sizeof(uint64_t)) {
            memcpy(&timestamp, dataOut.pbData, sizeof(uint64_t));
        }
        LocalFree(dataOut.pbData);
        return timestamp;
    }
    return 0;
}

// Write DPAPI-protected install timestamp to registry
static BOOL WriteInstallTimestamp(uint64_t timestamp) {
    DATA_BLOB dataIn = { sizeof(uint64_t), (BYTE*)&timestamp };
    DATA_BLOB dataOut;
    if (!CryptProtectData(&dataIn, L"hwagd_install_time", NULL, NULL, NULL,
                          CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN, &dataOut))
        return FALSE;

    HKEY hKey;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\CompatRuntime",
                                     0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    if (status == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"InstallTimeBlob", 0, REG_BINARY, dataOut.pbData, dataOut.cbData);
        RegCloseKey(hKey);
    }
    LocalFree(dataOut.pbData);
    return (status == ERROR_SUCCESS);
}

uint64_t GetMonitorDurationSeconds() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\CompatRuntime",
                      0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return HWAGD_DEFAULT_MONITOR_DURATION_SECONDS;

    DWORD duration = 0;
    DWORD size = sizeof(DWORD);
    DWORD type = 0;
    if (RegGetValueW(hKey, NULL, L"MonitorDurationSeconds", RRF_RT_REG_DWORD, &type, &duration, &size) == ERROR_SUCCESS
        && duration > 0) {
        RegCloseKey(hKey);
        return (uint64_t)duration;
    }
    RegCloseKey(hKey);
    return HWAGD_DEFAULT_MONITOR_DURATION_SECONDS;
}

BOOL IsMonitoringExpired() {
    return FALSE;
}

BOOL InitializeMonitoringTimer(HANDLE hShutdownEvent) {
    g_hMonitorShutdownEvent = hShutdownEvent;

    // Check if install timestamp exists; if not, write it now (first run)
    uint64_t installTime = ReadInstallTimestamp();
    if (installTime == 0) {
        installTime = GetCurrentUtcEpoch();
        WriteInstallTimestamp(installTime);
    }

    // Check if already expired
    if (IsMonitoringExpired()) {
        return FALSE; // Already expired - caller should exit
    }
    return TRUE;
}

DWORD WINAPI MonitorTimerThreadProc(LPVOID lpParam) {
    HANDLE hShutdownEvent = (HANDLE)lpParam;
    if (!hShutdownEvent) return 1;

    while (true) {
        // Wait 60 seconds or until shutdown is signaled
        DWORD waitResult = WaitForSingleObject(hShutdownEvent, 60000);
        if (waitResult == WAIT_OBJECT_0) {
            // Shutdown signaled externally
            break;
        }

        // Check if monitoring period has expired
        if (IsMonitoringExpired()) {
            // Signal shutdown to all threads
            SetEvent(hShutdownEvent);
            break;
        }
    }
    return 0;
}
