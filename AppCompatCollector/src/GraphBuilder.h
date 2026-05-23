#pragma once
#include <string>

// Serializes the current in-memory semantic graph to JSON.
// Called by the background egress thread in AppCompatCollector.
struct sqlite3; // Forward declaration

std::string FlushGraphToSQLiteAndJSON(sqlite3* db);
