#pragma once
#include <windows.h>
#include <string>
#include <re2/re2.h>

// Redacts credit card numbers, SSNs, and PHI placeholders from input text.
// Uses RE2 for linear-time, backtracking-free execution.
inline std::wstring RedactPII(const std::wstring& input) {
    if (input.empty()) return input;

    // Convert UTF-16 to UTF-8 for RE2
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0) return input;
    std::string utf8(utf8Len, '\0');  // Fixed: was '\\' in v3.0
    WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, &utf8[0], utf8Len, nullptr, nullptr);

    // Credit card patterns (simplified - extend as needed)
    re2::RE2::GlobalReplace(&utf8, re2::RE2("\\b[45][0-9]{3}[- ]?[0-9]{4}[- ]?[0-9]{4}[- ]?[0-9]{4}\\b"), "[REDACTED_CC]");
    // SSN pattern
    re2::RE2::GlobalReplace(&utf8, re2::RE2("\\b[0-9]{3}-[0-9]{2}-[0-9]{4}\\b"), "[REDACTED_SSN]");
    // PHI placeholder - extend with actual patterns as needed
    re2::RE2::GlobalReplace(&utf8, re2::RE2("(?i)\\b(?:patient|diagnosis|medical record)\\b"), "[REDACTED_PHI]");

    // Convert UTF-8 back to UTF-16
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return input;
    std::wstring result(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &result[0], wlen);
    return result;
}
