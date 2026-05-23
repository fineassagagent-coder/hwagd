#pragma once
#ifndef TEMPORAL_CORRELATION_H
#define TEMPORAL_CORRELATION_H

#include "hwagd_structs.h"
#include <vector>
#include <string>
#include <windows.h>

const int NUM_HASH_FUNCTIONS = 100;
const int SHINGLE_SIZE = 3;

struct TemporalDataNode {
    uint64_t timestamp;
    std::wstring processName;
    std::wstring extractedOcrText; // the raw tokens
    std::vector<uint64_t> minhashSignature;
};

// Computes the M-integer signature for a given text. Optionally uses character-level n-grams
std::vector<uint64_t> ComputeMinHashSignature(const std::wstring& text, bool useCharLevelShingles = true);

// Derives Jaccard Similarity index from two fixed MinHash Signatures
double CalculateJaccardSimilarity(const std::vector<uint64_t>& sigA, const std::vector<uint64_t>& sigB);

// The background thread procedure for Asynchronous Temporal Correlation
DWORD WINAPI CorrelationThreadProc(LPVOID lpParam);

#endif // TEMPORAL_CORRELATION_H
