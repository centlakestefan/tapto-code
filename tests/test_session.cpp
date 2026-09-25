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

    fs::remove_all(kDir, ec);

    std::cout << (g_failures ? "FAILED " : "ok ") << (g_checks - g_failures) << "/" << g_checks
              << " checks\n";
    return g_failures ? 1 : 0;
}
