// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB
//
// Line-ending behaviour of the text editor tool.
//
// The model only ever sees the CR-stripped rendering `view` produces, so it
// composes edits in LF terms. A file checked out on Windows holds CRLF, and a
// file with a long history can hold both. These tests drive the real tool the
// model calls, through builtin_tools(), and assert on the exact bytes on disk —
// which is the only place the difference shows up.

#include "tapto/context.h"
#include "tapto/tool_registry.h"
#include "tapto/tools.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_failures = 0;

// Render CR and LF visibly, so a failure report shows which endings are where.
std::string visible(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\r') out += "\\r";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

void check_eq(int line, const char* what, const std::string& actual, const std::string& expected) {
    ++g_checks;
    if (actual == expected) return;
    ++g_failures;
    std::cout << "FAIL (line " << line << ") " << what << "\n"
              << "  expected: " << visible(expected) << "\n"
              << "  actual:   " << visible(actual) << "\n";
}

void check_true(int line, const char* what, bool cond) {
    ++g_checks;
    if (cond) return;
    ++g_failures;
    std::cout << "FAIL (line " << line << ") " << what << "\n";
}

#define CHECK_EQ(actual, expected) check_eq(__LINE__, #actual, (actual), (expected))
#define CHECK_TRUE(cond) check_true(__LINE__, #cond, (cond))

// The tool resolves paths against the sandbox root, which is the process's
// working directory, so tests work in a subdirectory of wherever they run.
const char* kDir = "tapto-edit-tests";

std::string test_path(const std::string& name) {
    return std::string(kDir) + "/" + name;
}

void write_raw(const std::string& rel, const std::string& bytes) {
    std::ofstream out(rel, std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::string read_raw(const std::string& rel) {
    std::ifstream in(rel, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

ToolExecutorFn find_editor(Context& ctx) {
    for (const auto& t : ctx.tools) {
        if (t.name == "str_replace_based_edit_tool") return t.executor;
    }
    return nullptr;
}

ToolExecutorFn find_run_command(Context& ctx) {
    for (const auto& t : ctx.tools) {
        if (t.name == "run_command") return t.executor;
    }
    return nullptr;
}

} // namespace

int main() {
    Context ctx;
    ctx.tools = tapto::builtin_tools();
    ToolExecutorFn edit = find_editor(ctx);
    if (!edit) {
        std::cout << "FAIL: str_replace_based_edit_tool is not registered\n";
        return 1;
    }

    std::error_code ec;
    fs::remove_all(kDir, ec);
    fs::create_directories(kDir, ec);

    const std::string file = test_path("sample.txt");

    // --- str_replace, multi-line, on a CRLF file ---------------------------
    // The bug this suite exists for: the model sends "beta\ngamma" because that
    // is what `view` showed it, and a byte-exact search never finds it.
    {
        write_raw(file, "alpha\r\nbeta\r\ngamma\r\n");
        std::string r = edit(ctx, json{{"command", "str_replace"},
                                       {"path", file},
                                       {"old_str", "beta\ngamma"},
                                       {"new_str", "BETA\nGAMMA"}});
        CHECK_EQ(r, "OK");
        CHECK_EQ(read_raw(file), "alpha\r\nBETA\r\nGAMMA\r\n");
    }

    // --- str_replace, multi-line, on an LF file ----------------------------
    {
        write_raw(file, "alpha\nbeta\ngamma\n");
        std::string r = edit(ctx, json{{"command", "str_replace"},
                                       {"path", file},
                                       {"old_str", "beta\ngamma"},
                                       {"new_str", "BETA\nGAMMA"}});
        CHECK_EQ(r, "OK");
        CHECK_EQ(read_raw(file), "alpha\nBETA\nGAMMA\n");
    }

    // --- a CRLF needle against an LF file ----------------------------------
    {
        write_raw(file, "alpha\nbeta\ngamma\n");
        std::string r = edit(ctx, json{{"command", "str_replace"},
                                       {"path", file},
                                       {"old_str", "beta\r\ngamma"},
                                       {"new_str", "BETA\r\nGAMMA"}});
        CHECK_EQ(r, "OK");
        CHECK_EQ(read_raw(file), "alpha\nBETA\nGAMMA\n");
    }

    // --- a mixed file keeps the endings the edit didn't touch ---------------
    {
        write_raw(file, "one\r\ntwo\nthree\r\nfour\n");
        std::string r = edit(ctx, json{{"command", "str_replace"},
                                       {"path", file},
                                       {"old_str", "two\nthree"},
                                       {"new_str", "TWO\nTHREE"}});
        CHECK_EQ(r, "OK");
        // The span between "two" and "three" was LF in the original, so the
        // replacement is LF there too — the local convention is preserved, not
        // the file's majority. Lines one and four keep their own endings, which
        // is the property that matters: nothing outside the edit is rewritten.
        CHECK_EQ(read_raw(file), "one\r\nTWO\nTHREE\r\nfour\n");
    }

    // --- one line becoming several, in a CRLF file -------------------------
    // The matched span has no ending of its own, so the file's decides.
    {
        write_raw(file, "alpha\r\nbeta\r\ngamma\r\n");
        std::string r = edit(ctx, json{{"command", "str_replace"},
                                       {"path", file},
                                       {"old_str", "beta"},
                                       {"new_str", "one\ntwo\nthree"}});
        CHECK_EQ(r, "OK");
        CHECK_EQ(read_raw(file), "alpha\r\none\r\ntwo\r\nthree\r\ngamma\r\n");
    }

    // --- ambiguity is refused across differing endings ---------------------
    // Both occurrences render identically in `view`, so the model cannot have
    // meant one of them in particular — even though only the second is a
    // byte-exact match for what it sent.
    {
        write_raw(file, "x\r\ny\r\nzzz\r\nx\ny\r\n");
        std::string r = edit(ctx, json{{"command", "str_replace"},
                                       {"path", file},
                                       {"old_str", "x\ny"},
                                       {"new_str", "Q"}});
        CHECK_TRUE(r.rfind("ERROR: Multiple occurrences found (2)", 0) == 0);
        CHECK_EQ(read_raw(file), "x\r\ny\r\nzzz\r\nx\ny\r\n"); // unchanged
    }

    // --- genuinely absent text is still an error ---------------------------
    {
        write_raw(file, "alpha\r\nbeta\r\n");
        std::string r = edit(ctx, json{{"command", "str_replace"},
                                       {"path", file},
                                       {"old_str", "nowhere\nto be seen"},
                                       {"new_str", "x"}});
        CHECK_TRUE(r.rfind("ERROR: String not found", 0) == 0);
        CHECK_EQ(read_raw(file), "alpha\r\nbeta\r\n"); // unchanged
    }

    // --- insert keeps the file's endings -----------------------------------
    {
        write_raw(file, "alpha\r\nbeta\r\ngamma\r\n");
        std::string r = edit(ctx, json{{"command", "insert"},
                                       {"path", file},
                                       {"insert_line", 1},
                                       {"new_str", "INSERTED"}});
        CHECK_TRUE(r.rfind("Insertion successful", 0) == 0);
        CHECK_EQ(read_raw(file), "alpha\r\nINSERTED\r\nbeta\r\ngamma\r\n");
    }

    // --- insert at the beginning -------------------------------------------
    {
        write_raw(file, "alpha\r\nbeta\r\n");
        edit(ctx, json{{"command", "insert"},
                       {"path", file},
                       {"insert_line", 0},
                       {"new_str", "FIRST"}});
        CHECK_EQ(read_raw(file), "FIRST\r\nalpha\r\nbeta\r\n");
    }

    // --- insert after the last line ----------------------------------------
    {
        write_raw(file, "alpha\r\nbeta\r\ngamma\r\n");
        edit(ctx, json{{"command", "insert"},
                       {"path", file},
                       {"insert_line", 3},
                       {"new_str", "APPENDED"}});
        CHECK_EQ(read_raw(file), "alpha\r\nbeta\r\ngamma\r\nAPPENDED\r\n");
    }

    // --- insert at the phantom line split_lines yields for a trailing newline
    // Used to produce a blank line and drop the final newline.
    {
        write_raw(file, "alpha\r\nbeta\r\ngamma\r\n");
        edit(ctx, json{{"command", "insert"},
                       {"path", file},
                       {"insert_line", 4},
                       {"new_str", "APPENDED"}});
        CHECK_EQ(read_raw(file), "alpha\r\nbeta\r\ngamma\r\nAPPENDED\r\n");
    }

    // --- a file with no trailing newline doesn't gain one -------------------
    {
        write_raw(file, "alpha\r\nbeta");
        edit(ctx, json{{"command", "insert"},
                       {"path", file},
                       {"insert_line", 2},
                       {"new_str", "APPENDED"}});
        CHECK_EQ(read_raw(file), "alpha\r\nbeta\r\nAPPENDED");
    }

    // --- a multi-line insert follows the file's endings too -----------------
    {
        write_raw(file, "alpha\r\nbeta\r\n");
        edit(ctx, json{{"command", "insert"},
                       {"path", file},
                       {"insert_line", 1},
                       {"new_str", "one\ntwo"}});
        CHECK_EQ(read_raw(file), "alpha\r\none\r\ntwo\r\nbeta\r\n");
    }

    // --- insert into a mixed file leaves other lines alone ------------------
    {
        write_raw(file, "one\r\ntwo\nthree\r\n");
        edit(ctx, json{{"command", "insert"},
                       {"path", file},
                       {"insert_line", 2},
                       {"new_str", "NEW"}});
        // CRLF dominates, so the inserted line uses it; "two" keeps its LF.
        CHECK_EQ(read_raw(file), "one\r\ntwo\nNEW\r\nthree\r\n");
    }

    // --- an LF file stays LF ------------------------------------------------
    {
        write_raw(file, "alpha\nbeta\n");
        edit(ctx, json{{"command", "insert"},
                       {"path", file},
                       {"insert_line", 1},
                       {"new_str", "INSERTED"}});
        CHECK_EQ(read_raw(file), "alpha\nINSERTED\nbeta\n");
    }

    // --- view still hides CR from the model ---------------------------------
    {
        write_raw(file, "alpha\r\nbeta\r\n");
        std::string r = edit(ctx, json{{"command", "view"}, {"path", file}});
        CHECK_EQ(r, "1|alpha\n2|beta\n3|\n");
    }

    // --- .git is read-only to the model ------------------------------------
    // A writable .git/config (core.fsmonitor, core.hooksPath) or .git/hooks
    // would turn any allow-listed git command into code execution.
    {
        const std::string gitdir = test_path(".git");
        const std::string cfg = test_path(".git/config");
        fs::create_directories(gitdir, ec);
        write_raw(cfg, "[core]\n");

        std::string r = edit(ctx, json{{"command", "create"},
                                       {"path", test_path(".git/hooks/pre-commit")},
                                       {"file_text", "#!/bin/sh\n"}});
        CHECK_TRUE(r.rfind("ERROR:", 0) == 0 && r.find(".git") != std::string::npos);
        CHECK_TRUE(!fs::exists(test_path(".git/hooks/pre-commit")));

        r = edit(ctx, json{{"command", "str_replace"},
                           {"path", cfg},
                           {"old_str", "[core]"},
                           {"new_str", "[core]\n\tfsmonitor = evil"}});
        CHECK_TRUE(r.rfind("ERROR:", 0) == 0);
        CHECK_EQ(read_raw(cfg), "[core]\n");

        r = edit(ctx, json{{"command", "insert"},
                           {"path", cfg},
                           {"insert_line", 1},
                           {"new_str", "\tfsmonitor = evil"}});
        CHECK_TRUE(r.rfind("ERROR:", 0) == 0);
        CHECK_EQ(read_raw(cfg), "[core]\n");

        // Reading it is still fine, and a file merely *named* like .git isn't caught.
        r = edit(ctx, json{{"command", "view"}, {"path", cfg}});
        CHECK_EQ(r, "1|[core]\n2|\n");
        r = edit(ctx, json{{"command", "create"},
                           {"path", test_path(".gitignore")},
                           {"file_text", "build/\n"}});
        CHECK_EQ(r, "OK");
    }

    // --- run_command `path` fallback ---------------------------------------
    // The model sometimes sends `{"name":"ls","path":"driver"}` instead of
    // `{"name":"ls","args":["driver"]}` (conflating with the text editor). The
    // handler should treat the stray `path` field as the sole argument.
    {
        ToolExecutorFn run = find_run_command(ctx);
        CHECK_TRUE(run != nullptr);

        // Make a test subdirectory with a known file.
        fs::create_directories(test_path("driver"), ec);
        write_raw(test_path("driver/fake.c"), "void lmdb_open(){}\n");

        // path as a top-level field → should work like args: ["driver"]
        std::string r1 = run(ctx, json{{"name", "ls"}, {"path", test_path("driver")}});
        CHECK_TRUE(r1.find("fake.c") != std::string::npos);

        // args still works the same way
        std::string r2 = run(ctx, json{{"name", "ls"}, {"args", {test_path("driver")}}});
        CHECK_TRUE(r2.find("fake.c") != std::string::npos);

        // When both are present, args wins (path is ignored)
        write_raw(test_path("otherfile.txt"), "x\n");
        std::string r3 = run(ctx, json{{"name", "ls"},
                                       {"path", test_path("driver")},
                                       {"args", {test_path("driver")}}});
        CHECK_TRUE(r3.find("fake.c") != std::string::npos);

        fs::remove_all(kDir, ec);
        fs::create_directories(kDir, ec);
    }

    fs::remove_all(kDir, ec);

    std::cout << (g_failures ? "FAILED " : "ok ") << (g_checks - g_failures) << "/" << g_checks
              << " checks\n";
    return g_failures ? 1 : 0;
}
