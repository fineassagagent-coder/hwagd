#pragma once
#include <windows.h>
#include <cstdint>

// Default monitoring duration: 5 days = 432000 seconds
#define HWAGD_DEFAULT_MONITOR_DURATION_SECONDS 432000

// Initialize the monitoring timer. Writes install timestamp on first run.
// Returns FALSE if the monitoring period has already expired.
BOOL InitializeMonitoringTimer(HANDLE hShutdownEvent);

// Monitor thread procedure - checks elapsed time, signals shutdown on expiry.
DWORD WINAPI MonitorTimerThreadProc(LPVOID lpParam);

// Read the configured monitoring duration from registry (or return default).
uint64_t GetMonitorDurationSeconds();

// Check if the monitoring period has expired.
BOOL IsMonitoringExpired();
