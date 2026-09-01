// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB
//
// Behaviour of the Config store, focused on the regression that motivated the
// /cot work: saving a config used to rebuild the file from its key/value table,
// so any comment lines the user had added by hand were silently dropped.
//
// These tests drive Config directly (load / set / unset / save) and assert on
// the exact bytes on disk, which is where preservation shows up.

#include "tapto/config.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using tapto::Config;

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

const char* kDir = "tapto-config-tests";

void write_raw(const fs::path& p, const std::string& bytes) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::string read_raw(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    const fs::path file = std::string(kDir) + "/config";
    { std::error_code ec; fs::remove_all(kDir, ec); fs::create_directories(kDir, ec); }

    // --- a plain set preserves surrounding comments and blank lines ---------
    {
        write_raw(file,
                  "# tapto-code\n"
                  "# my comment about the provider\n\n"
                  "provider = claude\n"
                  "claude-api-key = sk-abc\n");
        Config cfg = Config::load(file);
        cfg.set("print-cot", "false");
        cfg.save(file);
        CHECK_EQ(read_raw(file),
                 "# tapto-code\n"
                 "# my comment about the provider\n\n"
                 "provider = claude\n"
                 "claude-api-key = sk-abc\n"
                 "print-cot = false\n");
    }

    // --- updating an existing key rewrites only that line -------------------
    {
        write_raw(file,
                  "# tapto-code\n"
                  "provider = claude\n"
                  "print-cot = true\n"
                  "# note: keep this\n"
                  "claude-api-key = sk-abc\n");
        Config cfg = Config::load(file);
        CHECK_EQ(*cfg.get("print-cot"), "true");
        cfg.set("print-cot", "false");
        cfg.save(file);
        CHECK_EQ(read_raw(file),
                 "# tapto-code\n"
                 "provider = claude\n"
                 "print-cot = false\n"
                 "# note: keep this\n"
                 "claude-api-key = sk-abc\n");
    }

    // --- unset removes the key and everything else stays put ----------------
    {
        write_raw(file,
                  "# tapto-code\n"
                  "provider = claude\n"
                  "\n"
                  "print-cot = true\n"
                  "claude-api-key = sk-abc\n");
        Config cfg = Config::load(file);
        CHECK_TRUE(cfg.unset("print-cot"));
        cfg.save(file);
        CHECK_EQ(read_raw(file),
                 "# tapto-code\n"
                 "provider = claude\n"
                 "\n"
                 "claude-api-key = sk-abc\n");
    }

    // --- unset of an absent key is a no-op (file unchanged) -----------------
    {
        const std::string original = "# tapto-code\nprovider = claude\n";
        write_raw(file, original);
        Config cfg = Config::load(file);
        CHECK_TRUE(!cfg.unset("no-such-key"));
        cfg.save(file);
        CHECK_EQ(read_raw(file), original);
    }

    // --- values with spaces and ';' are preserved verbatim ------------------
    {
        write_raw(file, "# tapto-code\nsystem-prompt = be brief; stay on task\n");
        Config cfg = Config::load(file);
        CHECK_EQ(*cfg.get("system-prompt"), "be brief; stay on task");
        cfg.save(file);
        CHECK_EQ(read_raw(file), "# tapto-code\nsystem-prompt = be brief; stay on task\n");
    }

    // --- a file with only comments keeps them over an append ----------------
    {
        write_raw(file, "# just notes\n# nothing else here\n\n");
        Config cfg = Config::load(file);
        cfg.set("provider", "gemini");
        cfg.save(file);
        CHECK_EQ(read_raw(file), "# just notes\n# nothing else here\n\nprovider = gemini\n");
    }

    // --- a missing file yields an empty config and a recognizable new file ---
    {
        std::error_code ec; fs::remove(file, ec);
        Config cfg = Config::load(file);
        CHECK_TRUE(!cfg.get("provider").has_value());
        cfg.set("provider", "claude");
        cfg.save(file);
        CHECK_EQ(read_raw(file), "# tapto-code\nprovider = claude\n");
    }

    // --- entries() skips comments/blanks and lets last-value win ------------
    {
        write_raw(file,
                  "# tapto-code\n"
                  "a = 1\n"
                  "\n"
                  "a = 2\n"
                  "; semicolon comment\n");
        Config cfg = Config::load(file);
        CHECK_EQ(*cfg.get("a"), "2");
        auto es = cfg.entries();
        int count = 0; std::string a_val;
        for (const auto& e : es) { if (e.first == "a") { ++count; a_val = e.second; } }
        CHECK_EQ(std::to_string(count), "1");
        CHECK_EQ(a_val, "2");
    }

    // --- load/save is idempotent (a save with no edits reproduces the file) -
    {
        const std::string original =
            "# tapto-code\n"
            "# generated by install\n\n"
            "provider = claude\n"
            "print-cot = true\n";
        write_raw(file, original);
        Config cfg = Config::load(file);
        cfg.save(file);
        CHECK_EQ(read_raw(file), original);
    }

    std::error_code ec;
    fs::remove_all(kDir, ec);

    std::cout << (g_failures ? "FAILED " : "ok ") << (g_checks - g_failures) << "/" << g_checks
              << " checks\n";
    return g_failures ? 1 : 0;
}
