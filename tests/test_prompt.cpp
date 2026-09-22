// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB
//
// How the system prompt is composed from the config keys: system-prompt and
// system-prompt-file replace the built-in prompt, the -append keys add to it.
// The config format is one line per key, so a prompt longer than a line has to
// come from a file; these tests pin how that file is found and read.

#include "tapto/prompt.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using tapto::EffectiveEntry;
using tapto::Level;

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

const char* kDir = "tapto-prompt-tests";
const std::string kBuiltin = "BUILTIN";

void write_raw(const fs::path& p, const std::string& bytes) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::string compose(const std::vector<EffectiveEntry>& config, std::vector<std::string>& problems) {
    return tapto::compose_system_prompt(config, fs::path(kDir), kBuiltin, problems);
}

} // namespace

int main() {
    std::error_code ec;
    fs::remove_all(kDir, ec);
    fs::create_directories(kDir, ec);
    const std::string abs_dir = fs::absolute(kDir).string();

    // --- nothing configured: the built-in prompt ---------------------------
    {
        std::vector<std::string> problems;
        CHECK_EQ(compose({}, problems), kBuiltin);
        CHECK_TRUE(problems.empty());
    }

    // --- a multi-line file, with a BOM and CRLF endings --------------------
    // Relative to the working directory for the local scope, since the local
    // config file itself lives under the user's home, not in the project.
    {
        write_raw(fs::path(kDir) / "prompt.md", "\xEF\xBB\xBFLine one.\r\n\r\nLine two.\r\n\r\n");
        std::vector<std::string> problems;
        CHECK_EQ(compose({{"system-prompt-file", "prompt.md", Level::Local}}, problems),
                 "Line one.\n\nLine two.");
        CHECK_TRUE(problems.empty());
    }

    // --- file vs inline: the more specific scope wins, the file on a tie ----
    {
        std::vector<std::string> problems;
        CHECK_EQ(compose({{"system-prompt", "INLINE", Level::Local},
                          {"system-prompt-file", abs_dir + "/prompt.md", Level::Global}},
                         problems),
                 "INLINE");
        CHECK_EQ(compose({{"system-prompt", "INLINE", Level::Global},
                          {"system-prompt-file", abs_dir + "/prompt.md", Level::Global}},
                         problems),
                 "Line one.\n\nLine two.");
        CHECK_TRUE(problems.empty());
    }

    // --- append keys go after the base, inline first ------------------------
    {
        write_raw(fs::path(kDir) / "rules.md", "Use tabs.\n");
        std::vector<std::string> problems;
        CHECK_EQ(compose({{"system-prompt-append", "Be brief.", Level::Global},
                          {"system-prompt-append-file", "rules.md", Level::Local}},
                         problems),
                 kBuiltin + "\n\nBe brief.\n\nUse tabs.");
        CHECK_TRUE(problems.empty());
    }

    // --- a missing file falls back to the same scope's inline value ---------
    {
        std::vector<std::string> problems;
        CHECK_EQ(compose({{"system-prompt", "INLINE", Level::Local},
                          {"system-prompt-file", "missing.md", Level::Local},
                          {"system-prompt-append-file", "missing.md", Level::Local}},
                         problems),
                 "INLINE");
        CHECK_TRUE(problems.size() == 2);
        CHECK_TRUE(!problems.empty() && problems[0].find("cannot read") != std::string::npos);
    }

    // --- ...but not to an inline value the file overrode --------------------
    // A policy file that fails to load must not hand the prompt to the user's
    // own system-prompt.
    {
        std::vector<std::string> problems;
        CHECK_EQ(compose({{"system-prompt", "USER", Level::Global},
                          {"system-prompt-file", abs_dir + "/missing.md", Level::Policy}},
                         problems),
                 kBuiltin);
        CHECK_TRUE(problems.size() == 1);
    }

    // --- a policy prompt can't be added to from the user's own config -------
    // The admin can't lock the -append keys out by leaving them empty (an
    // empty policy value is dropped), so user appends are ignored instead;
    // the policy's own appends still apply.
    {
        std::vector<std::string> problems;
        CHECK_EQ(compose({{"system-prompt", "ORG", Level::Policy},
                          {"system-prompt-append", "USER", Level::Local}},
                         problems),
                 "ORG");
        CHECK_TRUE(problems.size() == 1 && problems[0].find("policy") != std::string::npos);
        problems.clear();
        CHECK_EQ(compose({{"system-prompt-file", abs_dir + "/prompt.md", Level::Policy},
                          {"system-prompt-append", "ORG RULES", Level::Policy}},
                         problems),
                 "Line one.\n\nLine two.\n\nORG RULES");
        CHECK_TRUE(problems.empty());
        // Without a policy prompt, a policy append adds to the user's prompt.
        CHECK_EQ(compose({{"system-prompt", "USER", Level::Local},
                          {"system-prompt-append", "ORG RULES", Level::Policy}},
                         problems),
                 "USER\n\nORG RULES");
    }

    // --- empty and binary files are not used --------------------------------
    {
        write_raw(fs::path(kDir) / "empty.md", "\r\n  \n");
        write_raw(fs::path(kDir) / "blob.bin", std::string("MZ\0\0PE", 6));
        std::vector<std::string> problems;
        CHECK_EQ(compose({{"system-prompt-file", "empty.md", Level::Local}}, problems), kBuiltin);
        CHECK_EQ(compose({{"system-prompt-file", "blob.bin", Level::Local}}, problems), kBuiltin);
        CHECK_TRUE(problems.size() == 2);
    }

    // --- a policy path must be absolute -------------------------------------
    // Relative to the working directory, a project could supply the prompt the
    // organization mandates.
    {
        std::vector<std::string> problems;
        CHECK_EQ(compose({{"system-prompt-file", "prompt.md", Level::Policy}}, problems), kBuiltin);
        CHECK_TRUE(problems.size() == 1 && problems[0].find("absolute") != std::string::npos);
        problems.clear();
        CHECK_EQ(compose({{"system-prompt-file", abs_dir + "/prompt.md", Level::Policy}}, problems),
                 "Line one.\n\nLine two.");
        CHECK_TRUE(problems.empty());
    }

    // --- an empty value counts as unset -------------------------------------
    {
        std::vector<std::string> problems;
        CHECK_EQ(compose({{"system-prompt", "INLINE", Level::Global},
                          {"system-prompt-file", "", Level::Local}},
                         problems),
                 "INLINE");
        CHECK_TRUE(problems.empty());
    }

    fs::remove_all(kDir, ec);

    std::cout << (g_failures ? "FAILED " : "ok ") << (g_checks - g_failures) << "/" << g_checks
              << " checks\n";
    return g_failures ? 1 : 0;
}
