// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/session.h"

#include "tapto/compact.h"
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

namespace {

// Trailing blank lines and spaces do not show.
void trim_end(std::string& text) {
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    text.erase(end == std::string::npos ? 0 : end + 1);
}

// A model message's pieces in the order the screen showed them: the chain of
// thought, the text, the tool calls. Texts in one message run on, joined as
// each dialect splits them.
void model_pieces(const json& msg, const TranscriptWork* work, int iteration,
                  std::vector<TranscriptEntry>& out) {
    using Who = TranscriptEntry::Who;
    auto add = [&](Who who, std::string text, const char* sep) {
        if (who == Who::Tool) {
            out.push_back({who, std::move(text), iteration});
            return;
        }
        if (!out.empty() && out.back().who == who && out.back().iteration == -iteration) {
            // the same message's: run on
            out.back().text += sep + text;
            return;
        }
        // -iteration marks the entry as this message's until it is closed
        out.push_back({who, std::move(text), -iteration});
    };
    auto tool = [&](const std::string& name, const json& input) {
        if (!work) return;
        add(Who::Tool, work->label ? work->label(name, input) : name, "");
    };

    // openai: the reasoning field, the content, the calls
    const bool thoughts = work && work->reasoning;
    if (thoughts) {
        for (const char* field : {"reasoning_content", "reasoning"}) {
            if (msg.contains(field) && msg[field].is_string() && !msg[field].get<std::string>().empty()) {
                add(Who::Reasoning, msg[field].get<std::string>(), "");
                break;
            }
        }
    }
    if (msg.contains("content")) {
        const json& c = msg["content"];
        if (c.is_string()) {
            add(Who::Model, c.get<std::string>(), "");
        } else if (c.is_array()) {
            // claude blocks: text, thinking, tool_use in their order
            for (const auto& block : c) {
                if (!block.is_object()) continue;
                const std::string type = block.value("type", std::string());
                if (type == "text" && block.contains("text") && block["text"].is_string())
                    add(Who::Model, block["text"].get<std::string>(), "\n");
                else if (type == "thinking" && thoughts && block.contains("thinking") &&
                         block["thinking"].is_string())
                    add(Who::Reasoning, block["thinking"].get<std::string>(), "\n");
                else if (type == "tool_use")
                    tool(block.value("name", std::string()), block.value("input", json::object()));
            }
        }
    }
    if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
        for (const auto& call : msg["tool_calls"]) {
            if (!call.is_object() || !call.contains("function") || !call["function"].is_object()) continue;
            const json& fn = call["function"];
            json input = json::object();
            if (fn.contains("arguments") && fn["arguments"].is_string()) {
                input = json::parse(fn["arguments"].get<std::string>(), nullptr, false);
                if (input.is_discarded() || !input.is_object()) input = json::object();
            }
            tool(fn.value("name", std::string()), input);
        }
    }
    if (msg.contains("parts") && msg["parts"].is_array()) {
        // gemini: text parts run on, since one answer can come split over
        // several; a thought is reasoning; functionCall a tool's
        for (const auto& part : msg["parts"]) {
            if (!part.is_object()) continue;
            if (part.contains("text") && part["text"].is_string()) {
                if (!part.value("thought", false)) add(Who::Model, part["text"].get<std::string>(), "");
                else if (thoughts) add(Who::Reasoning, part["text"].get<std::string>(), "");
            } else if (part.contains("functionCall") && part["functionCall"].is_object()) {
                tool(part["functionCall"].value("name", std::string()),
                     part["functionCall"].value("args", json::object()));
            }
        }
    }
}

} // namespace

std::vector<TranscriptEntry> session_transcript(const json& history, const TranscriptWork* work) {
    using Who = TranscriptEntry::Who;
    // The message all three clients seed the history with after /compact
    // (libtapto's beginWithSummary).
    static const std::string kSummaryPrefix =
        "(The conversation prior to this point has been summarized for context):";

    std::vector<TranscriptEntry> out;
    if (!history.is_array()) return out;
    int iteration = 0; // the model's requests since the last prompt

    for (const auto& msg : history) {
        if (!msg.is_object()) continue;
        const std::string role = msg.value("role", std::string());
        const bool user = role == "user";
        const bool model = role == "assistant" || role == "model";
        if (!user && !model) continue; // openai's "tool" and "system" turns

        if (model) {
            ++iteration;
            const std::size_t from = out.size();
            std::vector<TranscriptEntry> pieces;
            model_pieces(msg, work, iteration, pieces);
            for (auto& e : pieces) {
                if (e.who != Who::Tool) {
                    // models open their answer with a line end or two after
                    // the chain of thought; the screen did not show them
                    e.text.erase(0, std::min(e.text.find_first_not_of("\r\n"), e.text.size()));
                    trim_end(e.text);
                    e.iteration = 0;
                    if (e.text.empty()) continue;
                }
                // The texts around tool calls not shown are one answer.
                if (e.who == Who::Model && !out.empty() && out.back().who == Who::Model) {
                    out.back().text += "\n\n" + e.text;
                } else if (e.who == Who::Reasoning && out.size() > from &&
                           out.back().who == Who::Reasoning) {
                    out.back().text += "\n" + e.text;
                } else {
                    out.push_back(std::move(e));
                }
            }
            continue;
        }

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

        trim_end(text);

        if (other) continue; // tool results and images, not a prompt
        iteration = 0;       // a prompt: its turn's requests count from 1
        if (text.rfind(kSummaryPrefix, 0) == 0) {
            out.push_back({Who::Summary, std::string()});
            continue;
        }
        // A compaction inside a turn (auto-compact) starts again from the
        // task, then the summary: the marker, and the task as the prompt.
        if (const std::string task = tapto::task_of_seed(text); task.size() != text.size()) {
            out.push_back({Who::Summary, std::string()});
            text = task;
        }
        text = without_folder_note(text);
        if (!text.empty()) out.push_back({Who::User, std::move(text)});
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
