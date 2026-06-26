#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

// File-based diagnostic log. Replaces the old stderr Log singleton: everything
// is appended to a log file so long sessions (and the request/response flow that
// drives them) can be inspected after the fact.
//
// Location: $MINICODE_LOG if set, otherwise ~/.minicode/minicode.log.

inline std::string mclog_path() {
    if (const char* p = std::getenv("MINICODE_LOG")) {
        if (*p) return p;
    }
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    std::filesystem::path base = (home && *home) ? std::filesystem::path(home)
                                                 : std::filesystem::path(".");
    return (base / ".minicode" / "minicode.log").string();
}

// Append a message to the log file (best-effort; failures are ignored). Call
// sites include their own trailing newline.
inline void mclog(const std::string& msg) {
    static std::mutex mtx;
    static std::ofstream out = [] {
        std::filesystem::path p = mclog_path();
        std::error_code ec;
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
        return std::ofstream(p, std::ios::app);
    }();

    std::lock_guard<std::mutex> lock(mtx);
    if (out) {
        out << msg;
        out.flush();
    }
}
