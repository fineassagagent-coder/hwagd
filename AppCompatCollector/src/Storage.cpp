#include "hwagd_structs.h"
#include <windows.h>
#include <dpapi.h>
#define SQLITE_HAS_CODEC
#include <sqlcipher/sqlite3.h>
#include <iostream>
#include <vector>

#pragma comment(lib, "Crypt32.lib")

// Read DPAPI blob from registry, decrypt, return raw binary key
std::vector<BYTE> UnprotectMasterKey() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\CompatRuntime", 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return {};
    DWORD size = 0;
    RegGetValueW(hKey, NULL, L"MasterKey", RRF_RT_REG_BINARY, NULL, NULL, &size);
    if (size == 0) { RegCloseKey(hKey); return {}; }
    std::vector<BYTE> protectedBlob(size);
    if (RegGetValueW(hKey, NULL, L"MasterKey", RRF_RT_REG_BINARY, NULL, protectedBlob.data(), &size) != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return {};
    }
    RegCloseKey(hKey);

    DATA_BLOB dataIn = { size, protectedBlob.data() };
    DATA_BLOB dataOut;
    if (CryptUnprotectData(&dataIn, NULL, NULL, NULL, NULL, 0, &dataOut)) {
        std::vector<BYTE> rawKey(dataOut.pbData, dataOut.pbData + dataOut.cbData);
        LocalFree(dataOut.pbData);
        return rawKey;
    }
    return {};
}

// Generate a new AES-256 master key and protect it with DPAPI
std::vector<BYTE> GenerateAndProtectMasterKey() {
    HCRYPTPROV hProv;
    CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT);
    BYTE rawKey[32];
    CryptGenRandom(hProv, sizeof(rawKey), rawKey);
    CryptReleaseContext(hProv, 0);

    DATA_BLOB dataIn = { sizeof(rawKey), rawKey };
    DATA_BLOB dataOut;
    if (CryptProtectData(&dataIn, L"hwagd_master_key", NULL, NULL, NULL,
                         CRYPTPROTECT_UI_FORBIDDEN, &dataOut)) {
        HKEY hKey;
        RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\CompatRuntime",
                        0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
        RegSetValueExW(hKey, L"MasterKey", 0, REG_BINARY, dataOut.pbData, dataOut.cbData);
        RegCloseKey(hKey);
        LocalFree(dataOut.pbData);
    }

    std::vector<BYTE> result(rawKey, rawKey + sizeof(rawKey));
    SecureZeroMemory(rawKey, sizeof(rawKey));
    return result;
}

void InitializeSecureStorage(sqlite3** db) {
    // Ensure hidden program data directory exists
    CreateDirectoryW(L"C:\\ProgramData\\ApplicationCompatibility", NULL);
    sqlite3_open("C:\\ProgramData\\ApplicationCompatibility\\ephemeral_cache.db", db);
    sqlite3_busy_timeout(*db, 5000);

    std::vector<BYTE> rawKey = UnprotectMasterKey();
    if (rawKey.empty()) {
        rawKey = GenerateAndProtectMasterKey();
    }
    if (!rawKey.empty()) {
        sqlite3_key(*db, rawKey.data(), static_cast<int>(rawKey.size()));
        SecureZeroMemory(rawKey.data(), rawKey.size());
    }

    sqlite3_exec(*db, "PRAGMA locking_mode = EXCLUSIVE;", NULL, NULL, NULL);
    sqlite3_exec(*db, "PRAGMA journal_mode = DELETE;", NULL, NULL, NULL);
    sqlite3_exec(*db, "PRAGMA synchronous = NORMAL;", NULL, NULL, NULL);

    const char* schema =
        "DROP TABLE IF EXISTS semantic_nodes;"
        "DROP TABLE IF EXISTS action_edges;"
        "CREATE TABLE IF NOT EXISTS ApplicationStateNode ("
        "node_id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "timestamp INTEGER, process_name TEXT, window_title TEXT, uia_matrix_json TEXT, minhash_sig BLOB);"
        "CREATE TABLE IF NOT EXISTS VisualAnchors ("
        "anchor_id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "node_id INTEGER, sha256_hash TEXT, png_base64_crop TEXT);"
        "CREATE TABLE IF NOT EXISTS WorkflowEdges ("
        "edge_id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "source_node_id INTEGER, dest_node_id INTEGER, edge_type TEXT, correlation_weight REAL);"
        "CREATE TABLE IF NOT EXISTS graph_batches ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "timestamp INTEGER, payload_json TEXT);";
    sqlite3_exec(*db, schema, NULL, NULL, NULL);
}

// Cryptographic Erasure - deletes DPAPI key and SQLite file.
void CryptographicErase() {
    RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\CompatRuntime", L"MasterKey");
    DeleteFileW(L"C:\\ProgramData\\ApplicationCompatibility\\ephemeral_cache.db");
}
