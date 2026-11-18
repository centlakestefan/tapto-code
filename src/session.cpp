// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/session.h"

#include "tapto/paths.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_set>

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
    if (!s.plugins.empty()) {
        j["plugins"] = json::array();
        for (const auto& p : s.plugins) {
            j["plugins"].push_back({{"name", p.name}, {"writable", p.writable}});
        }
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
        if (j.contains("plugins") && j["plugins"].is_array()) {
            for (const auto& p : j["plugins"]) {
                s.plugins.push_back({p.value("name", ""), p.value("writable", false)});
            }
        }
    } catch (const json::exception&) {
        error = path.string() + " is not a saved session";
        return std::nullopt;
    }
    s.history = std::move(j["history"]);
    return s;
}

namespace {
const char* const kNoteOpen = "<folder-access-changed>\n";
const char* const kNoteClose = "\n</folder-access-changed>\n\n";
} // namespace

std::string with_folder_note(const std::string& note, const std::string& prompt) {
    return kNoteOpen + note + kNoteClose + prompt;
}

std::string without_folder_note(const std::string& text) {
    if (text.rfind(kNoteOpen, 0) != 0) return text;
    const std::size_t close = text.find(kNoteClose);
    if (close == std::string::npos) return text;
    return text.substr(close + std::string(kNoteClose).size());
}

std::vector<TranscriptEntry> session_transcript(const json& history) {
    // The message all three clients seed the history with after /compact
    // (libtapto's beginWithSummary).
    static const std::string kSummaryPrefix =
        "(The conversation prior to this point has been summarized for context):";

    std::vector<TranscriptEntry> out;
    if (!history.is_array()) return out;

    for (const auto& msg : history) {
        if (!msg.is_object()) continue;
        const std::string role = msg.value("role", std::string());
        const bool user = role == "user";
        const bool model = role == "assistant" || role == "model";
        if (!user && !model) continue; // openai's "tool" and "system" turns

        // The message's text, and whether it also carries anything that is
        // not text the user or the model wrote.
        std::string text;
        bool other = false;
        auto add_text = [&](const json& t, const char* sep) {
            if (!t.is_string()) return;
            if (!text.empty()) text += sep;
            text += t.get<std::string>();
        };
        if (msg.contains("content")) {
            const json& c = msg["content"];
            if (c.is_string()) {
                add_text(c, "");
            } else if (c.is_array()) {
                // claude and openai blocks: text is shown; thinking is the
                // model's own; tool_use, tool_result, image and image_url are
                // what a tool did.
                for (const auto& block : c) {
                    if (!block.is_object()) continue;
                    const std::string type = block.value("type", std::string());
                    if (type == "text") add_text(block.value("text", json()), "\n");
                    else if (type != "thinking" && type != "redacted_thinking") other = true;
                }
            }
        }
        if (msg.contains("parts") && msg["parts"].is_array()) {
            // gemini: text parts are shown unless they are a thought, and run
            // on, since one answer can come split over several; functionCall,
            // functionResponse and inlineData are a tool's.
            for (const auto& part : msg["parts"]) {
                if (!part.is_object()) continue;
                if (part.contains("text")) {
                    if (!part.value("thought", false)) add_text(part["text"], "");
                } else {
                    other = true;
                }
            }
        }

        // Trailing blank lines and spaces do not show.
        const std::size_t end = text.find_last_not_of(" \t\r\n");
        text.erase(end == std::string::npos ? 0 : end + 1);

        if (user) {
            if (other) continue; // tool results and images, not a prompt
            if (text.rfind(kSummaryPrefix, 0) == 0) {
                out.push_back({TranscriptEntry::Who::Summary, std::string()});
                continue;
            }
            text = without_folder_note(text);
            if (!text.empty()) out.push_back({TranscriptEntry::Who::User, std::move(text)});
        } else if (!text.empty()) {
            // A model turn that only called tools has no text, and the texts
            // around tool calls are one answer to the same prompt.
            if (!out.empty() && out.back().who == TranscriptEntry::Who::Model) {
                out.back().text += "\n\n" + text;
            } else {
                out.push_back({TranscriptEntry::Who::Model, std::move(text)});
            }
        }
    }
    return out;
}

std::vector<std::string> recall_history(const std::vector<std::string>& typed,
                                        const json& history, const std::string& pending,
                                        std::size_t cap) {
    std::vector<std::string> all = typed;
    for (auto& e : session_transcript(history)) {
        if (e.who == TranscriptEntry::Who::User) all.push_back(std::move(e.text));
    }
    if (!pending.empty()) all.push_back(pending);

    // Newest first, so the first sight of an entry is the place it keeps.
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (auto it = all.rbegin(); it != all.rend() && out.size() < cap; ++it) {
        if (it->empty() || !seen.insert(*it).second) continue;
        out.push_back(std::move(*it));
    }
    std::reverse(out.begin(), out.end());
    return out;
}

} // namespace tapto
