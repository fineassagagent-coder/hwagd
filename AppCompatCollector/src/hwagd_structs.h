#pragma once
#ifndef HWAGD_STRUCTS_H
#define HWAGD_STRUCTS_H

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

// Utility: Convert UTF-16 wstring to UTF-8 string (used throughout the codebase)
inline std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string utf8(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &utf8[0], len, nullptr, nullptr);
    return utf8;
}

// Defines the data structure containing the deeply extracted UIA context
struct UIAContextMatrix {
    std::wstring automationId;
    std::wstring valueText;
    std::wstring rangeText;
    std::wstring legacyValue;
};

// Structural definition for the serialized visual anchor payload
struct VisualAnchorArtifact {
    std::string base64Png;
    std::string base64Sha256Hash;
};

// Represents a verified vertex (application state) in the deterministic workflow graph.
struct ApplicationStateNode {
    std::wstring processName;
    std::wstring windowTitle;
    std::wstring activeElementUiaName;
    std::wstring activeElementUiaType;
    std::wstring extractedOcrText;
    std::string uiaMatrixJson; // JSON string of serialized UIAContextMatrix list
    std::vector<uint64_t> minhashSignature; // Packed 100-integer signature for similarity
    VisualAnchorArtifact visualAnchor; // Stores local crop, inserted into VisualAnchors table
    DWORD processId;
    HWND hwnd;
    RECT windowBounds; // Spatial bounding box of the active window
    uint64_t timestamp;
};


// Represents the deterministic action connecting two states (the edge of the graph).
struct UserActionEdge {
    std::wstring actionType;      // "KEYSTROKE", "MOUSE_CLICK"
    std::wstring actionPayload;   // UTF-16 character or spatial coordinates
    uint64_t sourceTimestamp;
    uint64_t destTimestamp;
};

// Aggregate payload batch for SQLite serialization and eventual SMB egress.
struct SemanticGraphBatch {
    std::vector<ApplicationStateNode> nodes;
    std::vector<UserActionEdge> edges;
};

// HMAC-SHA256 integrity wrapper for batched payloads.
struct PayloadHMAC {
    std::string payloadJson;   // Serialized JSON of SemanticGraphBatch
    std::string hmacSha256;    // HMAC-SHA256 of payloadJson, key shared with IT server
};

typedef void (*StateChangeCallback)(const ApplicationStateNode& node);
typedef void (*ActionInterceptCallback)(const UserActionEdge& edge);

#endif // HWAGD_STRUCTS_H
