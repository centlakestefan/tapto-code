// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/session.h"

#include "tapto/paths.h"

#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;
using nlohmann::json;

namespace tapto {

namespace {

// Bumped when the layout changes in a way an older reader would misread.
constexpr int kSessionVersion = 1;

std::string local_timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

} // namespace

fs::path session_path() {
    return config_path(Level::Local).parent_path() / "session.json";
}

std::string save_session(const fs::path& path, SavedSession s) {
    s.saved_at = local_timestamp();
    json j;
    j["version"] = kSessionVersion;
    j["provider"] = s.provider;
    j["dialect"] = s.dialect;
    j["model"] = s.model;
    j["saved_at"] = s.saved_at;
    j["folders"] = json::array();
    for (const auto& g : s.folders) {
        j["folders"].push_back({{"path", g.path}, {"writable", g.writable}});
    }
    if (!s.pending_prompt.empty()) j["pending_prompt"] = s.pending_prompt;
    j["history"] = std::move(s.history);

    // A tool result can carry bytes that are not UTF-8; replacing them keeps
    // the save from throwing, and the model saw the same substitution.
    std::string text = j.dump(-1, ' ', false, json::error_handler_t::replace);

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return "cannot create " + path.parent_path().string() + ": " + ec.message();

    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return "cannot write " + tmp.string();
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) {
            fs::remove(tmp, ec);
            return "cannot write " + tmp.string();
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return "cannot replace " + path.string() + ": " + ec.message();
    }
    return "";
}

std::optional<SavedSession> load_session(const fs::path& path, std::string& error) {
    error.clear();
    std::error_code ec;
    if (!fs::exists(path, ec)) return std::nullopt;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path.string();
        return std::nullopt;
    }
    std::stringstream buf;
    buf << in.rdbuf();

    json j = json::parse(buf.str(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        error = path.string() + " is not a saved session";
        return std::nullopt;
    }
    if (j.value("version", 0) != kSessionVersion) {
        error = path.string() + " was saved by a different version of tapto-code";
        return std::nullopt;
    }
    if (!j.contains("history") || !j["history"].is_array()) {
        error = path.string() + " holds no conversation";
        return std::nullopt;
    }

    SavedSession s;
    try {
        s.provider = j.value("provider", "");
        s.dialect = j.value("dialect", "");
        s.model = j.value("model", "");
        s.saved_at = j.value("saved_at", "");
        s.pending_prompt = j.value("pending_prompt", "");
        if (j.contains("folders") && j["folders"].is_array()) {
            for (const auto& f : j["folders"]) {
                s.folders.push_back({f.value("path", ""), f.value("writable", false)});
            }
        }
    } catch (const json::exception&) {
        error = path.string() + " is not a saved session";
        return std::nullopt;
    }
    s.history = std::move(j["history"]);
    return s;
}

} // namespace tapto
