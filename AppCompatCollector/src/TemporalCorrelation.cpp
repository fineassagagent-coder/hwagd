#include "TemporalCorrelation.h"
#include <sstream>
#include <algorithm>
#include <limits>
#include <unordered_set>

extern std::atomic<bool> g_bRunning;
extern HANDLE g_hShutdownEvent;

uint64_t SeededHash(const std::wstring& str, int seed) {
    std::hash<std::wstring> hasher;
    uint64_t hashVal = hasher(str);
    // Execute a non-cryptographic Murmur-style avalanche mix to ensure permutation independence
    hashVal ^= seed + 0x9e3779b9 + (hashVal << 6) + (hashVal >> 2);
    return hashVal;
}

std::vector<std::wstring> GenerateShingles(const std::wstring& text, int k, bool useCharLevel) {
    std::vector<std::wstring> shingles;
    if (useCharLevel) {
        // Character-level n-grams (better for numeric/structured data)
        if (text.length() < static_cast<size_t>(k)) {
            if (!text.empty()) shingles.push_back(text);
            return shingles;
        }
        for (size_t i = 0; i <= text.length() - k; ++i) {
            shingles.push_back(text.substr(i, k));
        }
        return shingles;
    } else {
        // Word-level n-grams
        std::wistringstream stream(text);
        std::wstring word;
        std::vector<std::wstring> words;
        
        while (stream >> word) {
            words.push_back(word);
        }

        if (words.size() < static_cast<size_t>(k)) {
            if (!words.empty()) shingles.push_back(text);
            return shingles;
        }

        for (size_t i = 0; i <= words.size() - k; ++i) {
            std::wstring shingle = L"";
            for (int j = 0; j < k; ++j) {
                shingle += words[i + j];
            }
            shingles.push_back(shingle);
        }
        return shingles;
    }
}

std::vector<uint64_t> ComputeMinHashSignature(const std::wstring& text, bool useCharLevelShingles) {
    std::vector<uint64_t> signature(NUM_HASH_FUNCTIONS, (std::numeric_limits<uint64_t>::max)());

    // Default to char level for robust extraction of small financial fields
    std::vector<std::wstring> shingles = GenerateShingles(text, SHINGLE_SIZE, useCharLevelShingles);

    for (const auto& shingle : shingles) {
        for (int i = 0; i < NUM_HASH_FUNCTIONS; ++i) {
            uint64_t hashVal = SeededHash(shingle, i);
            if (hashVal < signature[i]) {
                signature[i] = hashVal;
            }
        }
    }
    return signature;
}

double CalculateJaccardSimilarity(const std::vector<uint64_t>& sigA, const std::vector<uint64_t>& sigB) {
    if (sigA.size() != sigB.size() || sigA.empty()) return 0.0;
    
    int matches = 0;
    for (size_t i = 0; i < sigA.size(); ++i) {
        if (sigA[i] == sigB[i]) {
            matches++;
        }
    }
    return static_cast<double>(matches) / sigA.size();
}

// TODO: This thread needs to query SQLite for the history buffer to do the correlation.
// Currently stubbed. It will be fully implemented once SQLite Storage functions are updated.
DWORD WINAPI CorrelationThreadProc(LPVOID lpParam) {
    (void)lpParam;
    while (g_bRunning.load()) {
        // Run every 15 minutes as recommended by audit
        DWORD waitResult = WaitForSingleObject(g_hShutdownEvent, 900000);
        if (waitResult == WAIT_OBJECT_0) {
            break;
        }
        
        // SQLite correlation pass will be injected here.
    }
    return 0;
}
