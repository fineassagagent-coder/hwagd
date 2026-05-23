#include "hwagd_structs.h"
#include "PIIRedactor.h"
#include <queue>
#include <mutex>
#include <fstream>
#include <nlohmann/json.hpp>
#include "TemporalCorrelation.h"
#include <sqlcipher/sqlite3.h>

using json = nlohmann::json;

// Thread-safe graph assembly queues
static std::queue<ApplicationStateNode> g_nodeQueue;
static std::queue<UserActionEdge> g_edgeQueue;
static std::mutex g_graphMutex;

// Callback invoked by UIA_Observer when a new application state is captured
void OnApplicationStateChanged(const ApplicationStateNode& node) {
    std::lock_guard<std::mutex> lock(g_graphMutex);
    ApplicationStateNode sanitized = node;
    // Redact sensitive data from UIA properties and OCR text
    sanitized.activeElementUiaName = RedactPII(node.activeElementUiaName);
    sanitized.activeElementUiaType = RedactPII(node.activeElementUiaType);
    sanitized.extractedOcrText = RedactPII(node.extractedOcrText);

    // Compute MinHash signature on the redacted OCR text + UIA name
    std::wstring minHashInput = sanitized.activeElementUiaName + L" " + sanitized.extractedOcrText;
    sanitized.minhashSignature = ComputeMinHashSignature(minHashInput, true);

    g_nodeQueue.push(sanitized);
}

// Callback invoked by InputCapture when a keystroke or mouse click is intercepted
void OnUserActionIntercepted(const UserActionEdge& edge) {
    std::lock_guard<std::mutex> lock(g_graphMutex);
    UserActionEdge sanitized = edge;
    sanitized.actionPayload = RedactPII(edge.actionPayload);
    g_edgeQueue.push(sanitized);
}

// Serialize the current graph state to JSON for batched egress and flush to SQLite.
// Called by the background egress thread in AppCompatCollector.
std::string FlushGraphToSQLiteAndJSON(sqlite3* db) {
    std::lock_guard<std::mutex> lock(g_graphMutex);

    json j;
    j["nodes"] = json::array();
    j["edges"] = json::array();

    while (!g_nodeQueue.empty()) {
        auto& node = g_nodeQueue.front();
        json nodeObj;
        nodeObj["process"] = WideToUtf8(node.processName);
        nodeObj["window_title"] = WideToUtf8(node.windowTitle);
        nodeObj["uia_name"] = WideToUtf8(node.activeElementUiaName);
        nodeObj["uia_type"] = WideToUtf8(node.activeElementUiaType);
        nodeObj["ocr_text"] = WideToUtf8(node.extractedOcrText);
        nodeObj["uia_matrix_json"] = node.uiaMatrixJson;
        nodeObj["minhash_sig"] = node.minhashSignature;
        
        json boundsObj;
        boundsObj["left"] = node.windowBounds.left;
        boundsObj["top"] = node.windowBounds.top;
        boundsObj["right"] = node.windowBounds.right;
        boundsObj["bottom"] = node.windowBounds.bottom;
        boundsObj["width"] = node.windowBounds.right - node.windowBounds.left;
        boundsObj["height"] = node.windowBounds.bottom - node.windowBounds.top;
        nodeObj["window_bounds"] = boundsObj;

        if (!node.visualAnchor.base64Png.empty()) {
            json anchorObj;
            anchorObj["png_base64"] = node.visualAnchor.base64Png;
            anchorObj["sha256_hash"] = node.visualAnchor.base64Sha256Hash;
            nodeObj["visual_anchor"] = anchorObj;
        }
        nodeObj["timestamp"] = node.timestamp;
        j["nodes"].push_back(nodeObj);
        
        if (db) {
            sqlite3_stmt* stmt = nullptr;
            const char* sqlNode = "INSERT INTO ApplicationStateNode (timestamp, process_name, window_title, uia_matrix_json, minhash_sig) VALUES (?, ?, ?, ?, ?);";
            if (sqlite3_prepare_v2(db, sqlNode, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(stmt, 1, node.timestamp);
                std::string proc = WideToUtf8(node.processName);
                sqlite3_bind_text(stmt, 2, proc.c_str(), -1, SQLITE_TRANSIENT);
                std::string win = WideToUtf8(node.windowTitle);
                sqlite3_bind_text(stmt, 3, win.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 4, node.uiaMatrixJson.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_blob(stmt, 5, node.minhashSignature.data(), node.minhashSignature.size() * sizeof(uint64_t), SQLITE_TRANSIENT);
                
                if (sqlite3_step(stmt) == SQLITE_DONE) {
                    sqlite3_int64 nodeId = sqlite3_last_insert_rowid(db);
                    
                    // Phase 2: Save the visual anchor if one exists
                    if (!node.visualAnchor.base64Png.empty()) {
                        sqlite3_finalize(stmt);
                        stmt = nullptr;
                        const char* sqlAnchor = "INSERT INTO VisualAnchors (node_id, sha256_hash, png_base64_crop) VALUES (?, ?, ?);";
                        if (sqlite3_prepare_v2(db, sqlAnchor, -1, &stmt, nullptr) == SQLITE_OK) {
                            sqlite3_bind_int64(stmt, 1, nodeId);
                            sqlite3_bind_text(stmt, 2, node.visualAnchor.base64Sha256Hash.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_bind_text(stmt, 3, node.visualAnchor.base64Png.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_step(stmt);
                        } else {
                            std::ofstream errLog("C:\\ProgramData\\ApplicationCompatibility\\db_error.log", std::ios::app);
                            errLog << "SQL Anchor Prepare Error: " << sqlite3_errmsg(db) << "\n";
                        }
                    }
                } else {
                    std::ofstream errLog("C:\\ProgramData\\ApplicationCompatibility\\db_error.log", std::ios::app);
                    errLog << "SQL Node Insert Error: " << sqlite3_errmsg(db) << "\n";
                }
            } else {
                std::ofstream errLog("C:\\ProgramData\\ApplicationCompatibility\\db_error.log", std::ios::app);
                errLog << "SQL Node Prepare Error: " << sqlite3_errmsg(db) << "\n";
            }
            if (stmt) sqlite3_finalize(stmt);
        }
        
        g_nodeQueue.pop();
    }

    while (!g_edgeQueue.empty()) {
        auto& edge = g_edgeQueue.front();
        json edgeObj;
        edgeObj["action_type"] = WideToUtf8(edge.actionType);
        edgeObj["action_payload"] = WideToUtf8(edge.actionPayload);
        edgeObj["source_timestamp"] = edge.sourceTimestamp;
        edgeObj["dest_timestamp"] = edge.destTimestamp;
        j["edges"].push_back(edgeObj);
        
        if (db) {
            sqlite3_stmt* stmt = nullptr;
            const char* sqlEdge = "INSERT INTO WorkflowEdges (source_node_id, dest_node_id, edge_type, correlation_weight) VALUES (?, ?, ?, ?);";
            if (sqlite3_prepare_v2(db, sqlEdge, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(stmt, 1, edge.sourceTimestamp); // Approx using timestamp for hard events
                sqlite3_bind_int64(stmt, 2, edge.destTimestamp);
                std::string eType = WideToUtf8(edge.actionType);
                sqlite3_bind_text(stmt, 3, eType.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_double(stmt, 4, 1.0);
                if (sqlite3_step(stmt) != SQLITE_DONE) {
                    std::ofstream errLog("C:\\ProgramData\\ApplicationCompatibility\\db_error.log", std::ios::app);
                    errLog << "SQL Edge Insert Error: " << sqlite3_errmsg(db) << "\n";
                }
            } else {
                std::ofstream errLog("C:\\ProgramData\\ApplicationCompatibility\\db_error.log", std::ios::app);
                errLog << "SQL Edge Prepare Error: " << sqlite3_errmsg(db) << "\n";
            }
            if (stmt) sqlite3_finalize(stmt);
        }
        
        g_edgeQueue.pop();
    }

    return j.dump(2); // pretty-print with 2 spaces
}
