// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB
//
// The saved conversation behind --resume and /resume. A save must read back
// as it was written, including history the provider shaped and text that is
// not valid UTF-8; a file that is damaged, foreign or from another layout
// version must be refused with a reason rather than loaded half-way.

#include "tapto/session.h"

#include "tapto/paths.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using nlohmann::json;
using tapto::SavedSession;

namespace {

int g_checks = 0;
int g_failures = 0;

void check_eq(int line, const char* what, const std::string& actual, const std::string& expected) {
    ++g_checks;
    if (actual == expected) return;
    ++g_failures;
    std::cout << "FAIL (line " << line << ") " << what << "\n"
              << "  expected:\n" << expected << "\n"
              << "  actual:\n" << actual << "\n";
}

void check_true(int line, const char* what, bool cond) {
    ++g_checks;
    if (cond) return;
    ++g_failures;
    std::cout << "FAIL (line " << line << ") " << what << "\n";
}

#define CHECK_EQ(actual, expected) check_eq(__LINE__, #actual, (actual), (expected))
#define CHECK_TRUE(cond) check_true(__LINE__, #cond, (cond))

const char* kDir = "tapto-session-tests";

void write_raw(const fs::path& p, const std::string& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << bytes;
}

} // namespace

int main() {
    std::error_code ec;
    fs::remove_all(kDir, ec);
    // The directory is created by the save itself, as it is under ~/.tapto.
    const fs::path file = fs::path(kDir) / "project" / "session.json";

    // --- nothing saved -----------------------------------------------------
    {
        std::string err;
        CHECK_TRUE(!tapto::load_session(file, err));
        CHECK_EQ(err, "");
    }

    // --- round trip --------------------------------------------------------
    {
        SavedSession s;
        s.provider = "qwen36";
        s.dialect = "openai";
        s.model = "Qwen3-VL-30B";
        s.folders.push_back({"C:/proj/libfoo", false});
        s.folders.push_back({"C:/proj/libbar", true});
        s.plugins.push_back({"image", false});
        s.plugins.push_back({"jira", true});
        s.pending_prompt = "fix the build\nplease";
        s.history = json::array({
            {{"role", "user"}, {"content", "hello \xE2\x80\x94 world"}},
            {{"role", "assistant"}, {"content", "hi"},
             {"tool_calls", json::array({{{"id", "c1"}, {"type", "function"}}})}},
        });
        CHECK_EQ(tapto::save_session(file, s), "");
        CHECK_TRUE(!fs::exists(fs::path(file).concat(".tmp")));

        std::string err;
        auto back = tapto::load_session(file, err);
        CHECK_EQ(err, "");
        CHECK_TRUE(back.has_value());
        if (back) {
            CHECK_EQ(back->provider, "qwen36");
            CHECK_EQ(back->dialect, "openai");
            CHECK_EQ(back->model, "Qwen3-VL-30B");
            CHECK_EQ(back->pending_prompt, "fix the build\nplease");
            CHECK_EQ(back->history.dump(), s.history.dump());
            CHECK_TRUE(back->folders.size() == 2);
            CHECK_TRUE(back->plugins.size() == 2 && back->plugins[0].name == "image" &&
                       !back->plugins[0].writable && back->plugins[1].name == "jira" &&
                       back->plugins[1].writable);
            if (back->folders.size() == 2) {
                CHECK_EQ(back->folders[1].path, "C:/proj/libbar");
                CHECK_TRUE(!back->folders[0].writable);
                CHECK_TRUE(back->folders[1].writable);
            }
            CHECK_EQ(std::to_string(back->saved_at.size()), "16"); // YYYY-MM-DD HH:MM
        }
    }

    // --- a second save replaces the first; no pending prompt is none --------
    {
        SavedSession s;
        s.dialect = "claude";
        s.history = json::array({{{"role", "user"}, {"content", "second"}}});
        CHECK_EQ(tapto::save_session(file, s), "");
        std::string err;
        auto back = tapto::load_session(file, err);
        CHECK_TRUE(back.has_value());
        if (back) {
            CHECK_EQ(back->dialect, "claude");
            CHECK_EQ(back->pending_prompt, "");
            CHECK_TRUE(back->folders.empty());
            CHECK_EQ(back->history.dump(), s.history.dump());
        }
    }

    // --- bytes that are not UTF-8 are replaced, not a failed save ------------
    {
        SavedSession s;
        s.dialect = "claude";
        s.history = json::array({{{"role", "user"}, {"content", std::string("bad \xFF byte")}}});
        CHECK_EQ(tapto::save_session(file, s), "");
        std::string err;
        auto back = tapto::load_session(file, err);
        CHECK_TRUE(back.has_value());
        if (back) {
            CHECK_EQ(back->history[0]["content"].get<std::string>(), "bad \xEF\xBF\xBD byte");
        }
    }

    // --- refused files ------------------------------------------------------
    {
        std::string err;
        write_raw(file, "{\"version\": 1, \"history\": [");
        CHECK_TRUE(!tapto::load_session(file, err));
        CHECK_TRUE(err.find("is not a saved session") != std::string::npos);

        write_raw(file, "{\"version\": 99, \"history\": []}");
        CHECK_TRUE(!tapto::load_session(file, err));
        CHECK_TRUE(err.find("different version") != std::string::npos);

        write_raw(file, "{\"version\": 1, \"history\": {}}");
        CHECK_TRUE(!tapto::load_session(file, err));
        CHECK_TRUE(err.find("holds no conversation") != std::string::npos);

        write_raw(file, "{\"version\": 1, \"provider\": 7, \"history\": []}");
        CHECK_TRUE(!tapto::load_session(file, err));
        CHECK_TRUE(err.find("is not a saved session") != std::string::npos);
    }

    // --- it lives beside the local config, not in the project ----------------
    CHECK_EQ(tapto::session_path().parent_path().string(),
             tapto::config_path(tapto::Level::Local).parent_path().string());
    CHECK_EQ(tapto::session_path().filename().string(), "session.json");

    // --- the transcript /resume shows, from each dialect's history -----------
    // The prompts and the model's text, in order; the model's texts around
    // its tool calls joined; tool turns, images and thinking left out; the
    // /compact seed a marker of its own.
    {
        using Who = tapto::TranscriptEntry::Who;
        auto render = [](const std::vector<tapto::TranscriptEntry>& t) {
            std::string out;
            for (const auto& e : t) {
                out += e.who == Who::User ? "U:" : e.who == Who::Model ? "M:" : "S:";
                out += e.text + "|";
            }
            return out;
        };
        const std::string seed =
            "(The conversation prior to this point has been summarized for context):\nwe did X";

        json claude = json::parse(R"([
            {"role":"user","content":"fix the build"},
            {"role":"assistant","content":[
                {"type":"thinking","thinking":"hmm","signature":"s"},
                {"type":"text","text":"Looking at it."},
                {"type":"tool_use","id":"t1","name":"run_command","input":{}}]},
            {"role":"user","content":[
                {"type":"tool_result","tool_use_id":"t1","content":"ok"}]},
            {"role":"assistant","content":[{"type":"text","text":"Fixed.\n\n"}]},
            {"role":"user","content":[{"type":"text","text":"thanks","cache_control":{"type":"ephemeral"}}]}
        ])");
        claude.insert(claude.begin(), json{{"role", "user"}, {"content", seed}});
        CHECK_EQ(render(tapto::session_transcript(claude)),
                 "S:|U:fix the build|M:Looking at it.\n\nFixed.|U:thanks|");

        const json openai = json::parse(R"([
            {"role":"system","content":"be brief"},
            {"role":"user","content":"show the logo"},
            {"role":"assistant","content":null,"reasoning_content":"think",
             "tool_calls":[{"id":"c1","type":"function","function":{"name":"view","arguments":"{}"}}]},
            {"role":"tool","tool_call_id":"c1","content":"done"},
            {"role":"user","content":[{"type":"text","text":"logo.png:"},
                {"type":"image_url","image_url":{"url":"data:image/png;base64,AA=="}}]},
            {"role":"assistant","content":"It is blue."}
        ])");
        CHECK_EQ(render(tapto::session_transcript(openai)), "U:show the logo|M:It is blue.|");

        const json gemini = json::parse(R"([
            {"role":"user","parts":[{"text":"list files"}]},
            {"role":"model","parts":[{"text":"pondering","thought":true},
                {"functionCall":{"name":"find_files","args":{}}}]},
            {"role":"user","parts":[{"functionResponse":{"name":"find_files","response":{"content":"a.cpp"}}}]},
            {"role":"model","parts":[{"text":"There is "},{"text":"a.cpp."}]}
        ])");
        CHECK_EQ(render(tapto::session_transcript(gemini)), "U:list files|M:There is a.cpp.|");

        // A folder note sent ahead of a prompt is the model's, not the
        // user's: the transcript (and so Up's recall) shows the prompt alone.
        const std::string sent = tapto::with_folder_note("Granted:\n  lib  (C:/lib)", "read lib/a.h");
        CHECK_TRUE(sent.find("C:/lib") != std::string::npos);
        CHECK_EQ(tapto::without_folder_note(sent), "read lib/a.h");
        CHECK_EQ(tapto::without_folder_note("<folder-access-changed> by hand"),
                 "<folder-access-changed> by hand");
        json noted = json::array();
        noted.push_back({{"role", "user"}, {"content", sent}});
        noted.push_back({{"role", "assistant"}, {"content", "Read it."}});
        CHECK_EQ(render(tapto::session_transcript(noted)), "U:read lib/a.h|M:Read it.|");
        const auto recalled = tapto::recall_history({}, noted, "", 10);
        CHECK_EQ(recalled.empty() ? std::string() : recalled.front(), "read lib/a.h");

        CHECK_EQ(render(tapto::session_transcript(json::object())), "");
        CHECK_EQ(render(tapto::session_transcript(json::parse(R"([1,"x",{"role":"user"}])"))), "");

        // --- what Up recalls after a resume ----------------------------------
        // This run's typing, then the conversation's prompts, then the
        // unanswered one; a repeat keeps its newest place; the newest `cap`.
        auto join = [](const std::vector<std::string>& v) {
            std::string out;
            for (const auto& s : v) out += s + "|";
            return out;
        };
        CHECK_EQ(join(tapto::recall_history({}, claude, "and the tests", 100)),
                 "fix the build|thanks|and the tests|");
        CHECK_EQ(join(tapto::recall_history({"thanks", "/resume"}, claude, "", 100)),
                 "/resume|fix the build|thanks|");
        CHECK_EQ(join(tapto::recall_history({"a", ""}, gemini, "a", 100)), "list files|a|");
        CHECK_EQ(join(tapto::recall_history({"a", "b"}, claude, "c", 2)), "thanks|c|");
        CHECK_EQ(join(tapto::recall_history({}, json::array(), "", 100)), "");
    }

    fs::remove_all(kDir, ec);

    std::cout << (g_failures ? "FAILED " : "ok ") << (g_checks - g_failures) << "/" << g_checks
              << " checks\n";
    return g_failures ? 1 : 0;
}
