// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB
//
// Trimming of the /compact summarization input.
//
// buildTrimmedHistoryForSummary() (header-only on AiBackend) must shrink every
// oversized tool-result payload to a short placeholder while leaving the small
// ones, the user text, the assistant text, and — crucially — the tool *calls*
// intact. It must also be a pure function of the history: the live history must
// not be mutated, and a history with nothing oversized must map back to an
// identical copy. Each of the three provider shapes (OpenAI role:"tool",
// Claude tool_result blocks, Gemini functionResponse parts) is driven through
// a canned history on a stub backend, so no provider, key, or network is needed.

#include "tapto/aibackend.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <iostream>
#include <optional>
#include <string>

using json = nlohmann::json;

namespace {

int g_checks = 0;
int g_failures = 0;

void check_eq(int line, const char* what, const std::string& actual, const std::string& expected) {
    ++g_checks;
    if (actual == expected) return;
    ++g_failures;
    std::cout << "FAIL (line " << line << ") " << what << "\n"
              << "  expected: " << expected << "\n"
              << "  actual:   " << actual << "\n";
}

void check_eq(int line, const char* what, long long actual, long long expected) {
    ++g_checks;
    if (actual == expected) return;
    ++g_failures;
    std::cout << "FAIL (line " << line << ") " << what << "\n"
              << "  expected: " << expected << "\n"
              << "  actual:   " << actual << "\n";
}

void check_true(int line, const char* what, bool cond) {
    ++g_checks;
    if (cond) return;
    ++g_failures;
    std::cout << "FAIL (line " << line << ") " << what << "\n";
}

#define CHECK_EQ(actual, expected) check_eq(__LINE__, #actual, (actual), (expected))
#define CHECK_TRUE(cond) check_true(__LINE__, #cond, (cond))

// Minimal AiBackend whose only job is to hand back a fixed history, so the
// header-only trimmer can be driven without any provider, key, or network.
// loadHistory()/getHistory() back the stored value; the rest are no-ops.
class FakeBackend : public AiBackend {
public:
    json m_hist;

    void setSystemPrompt(const std::string&) override {}
    const std::string& getSystemPrompt() const override { return m_sp; }
    void setModel(const std::string&) override {}
    void setHost(const std::string&) override {}
    void setApiKeyRef(const std::string&) override {}
    void setThinkingBudget(std::optional<int>) override {}
    std::string chat(Context&, const std::string&) override { return ""; }
    void start() override { m_hist = json::array(); }
    bool hasHistory() const override { return !m_hist.empty(); }
    void loadHistory(const json& h) override { m_hist = h; }
    json getHistory() const override { return m_hist; }
    std::size_t lastInputTokens() const override { return 0; }
    void beginWithSummary(const std::string&) override { m_hist = json::array(); }

private:
    std::string m_sp;
};

} // namespace

int main() {
    FakeBackend be;
    // Payloads just over and under AiBackend::kCompactTrimPayloadChars (2000).
    const std::string big(5000, 'a');
    const std::string small = "ok";
    const std::string expectBigPh = "[omitted tool output (" + std::to_string(5000) + " chars)]";

    // --- OpenAI: oversized tool result shrunk, small kept, calls intact ----
    {
        json tc_fn = {{"name", "str_replace_based_edit_tool"}, {"arguments", "{\"path\":\"src/main.cpp\"}"}};
        json tc = {{"id", "c1"}, {"function", tc_fn}};
        json tcs = json::array();
        tcs.push_back(tc);

        json hist = json::array();
        hist.push_back({{"role", "user"}, {"content", "read the main file"}});
        hist.push_back({{"role", "assistant"}, {"content", ""}, {"tool_calls", tcs}});
        hist.push_back({{"role", "tool"}, {"tool_call_id", "c1"}, {"content", big}});
        hist.push_back({{"role", "tool"}, {"tool_call_id", "c2"}, {"content", small}});
        hist.push_back({{"role", "assistant"}, {"content", "done, it was fine"}});
        be.loadHistory(hist);

        json t = be.buildTrimmedHistoryForSummary();

        CHECK_EQ(t[2]["content"].get<std::string>(), expectBigPh); // oversized result shrunk
        CHECK_EQ(t[3]["content"].get<std::string>(), small); // small result kept
        CHECK_EQ(t[1]["tool_calls"][0]["function"]["name"].get<std::string>(), "str_replace_based_edit_tool"); // call preserved
        CHECK_EQ(t[1]["tool_calls"][0]["function"]["arguments"].get<std::string>(), "{\"path\":\"src/main.cpp\"}"); // args preserved
        CHECK_EQ(t[4]["content"].get<std::string>(), "done, it was fine"); // assistant text preserved
        CHECK_EQ((int)t.size(), 5); // same number of messages
    }

    // --- Claude: tool_result blocks shrunk; tool_use call preserved --------
    {
        json tb = {{"type", "text"}, {"text", "reading now"}};
        json tu_input = {{"path", "src/main.cpp"}};
        json tu = {{"type", "tool_use"}, {"id", "u1"}, {"name", "str_replace_based_edit_tool"}, {"input", tu_input}};
        json ablocks = json::array();
        ablocks.push_back(tb);
        ablocks.push_back(tu);

        json tr_big = {{"type", "tool_result"}, {"tool_use_id", "u1"}, {"content", big}};
        json tr_small = {{"type", "tool_result"}, {"tool_use_id", "u2"}, {"content", small}};
        json ures = json::array();
        ures.push_back(tr_big);
        ures.push_back(tr_small);

        json ablocks2 = json::array();
        ablocks2.push_back({{"type", "text"}, {"text", "ok"}});

        json hist = json::array();
        hist.push_back({{"role", "user"}, {"content", "look at the file"}});
        hist.push_back({{"role", "assistant"}, {"content", ablocks}});
        hist.push_back({{"role", "user"}, {"content", ures}});
        hist.push_back({{"role", "assistant"}, {"content", ablocks2}});
        be.loadHistory(hist);

        json t = be.buildTrimmedHistoryForSummary();

        CHECK_EQ(t[2]["content"][0]["content"].get<std::string>(), expectBigPh); // oversized result shrunk
        CHECK_EQ(t[2]["content"][1]["content"].get<std::string>(), small); // small result kept
        CHECK_EQ(t[1]["content"][1]["type"].get<std::string>(), "tool_use");
        CHECK_EQ(t[1]["content"][1]["name"].get<std::string>(), "str_replace_based_edit_tool"); // call preserved
        CHECK_EQ(t[1]["content"][1]["input"]["path"].get<std::string>(), "src/main.cpp"); // call input preserved
        CHECK_EQ((int)t.size(), 4); // same number of messages
    }

    // --- Gemini: functionResponse shrunk; functionCall preserved -----------
    {
        json fc_args = {{"path", "src/main.cpp"}};
        json fc = {{"name", "str_replace_based_edit_tool"}, {"args", fc_args}};
        json fcpart = {{"functionCall", fc}};
        json mparts = json::array();
        mparts.push_back(fcpart);

        json resp_big = {{"content", big}};
        json fr_big = {{"name", "str_replace_based_edit_tool"}, {"response", resp_big}};
        json fr_big_part = {{"functionResponse", fr_big}};
        json resp_small = {{"content", small}};
        json fr_small = {{"name", "run_command"}, {"response", resp_small}};
        json fr_small_part = {{"functionResponse", fr_small}};
        json uparts = json::array();
        uparts.push_back(fr_big_part);
        uparts.push_back(fr_small_part);

        json mparts2 = json::array();
        mparts2.push_back({{"text", "done"}});

        json readpart = {{"text", "read it"}};
        json userparts = json::array();
        userparts.push_back(readpart);

        json hist = json::array();
        hist.push_back({{"role", "user"}, {"parts", userparts}});
        hist.push_back({{"role", "model"}, {"parts", mparts}});
        hist.push_back({{"role", "user"}, {"parts", uparts}});
        hist.push_back({{"role", "model"}, {"parts", mparts2}});
        be.loadHistory(hist);

        json t = be.buildTrimmedHistoryForSummary();

        CHECK_EQ(t[2]["parts"][0]["functionResponse"]["response"]["content"].get<std::string>(), expectBigPh); // oversized shrunk
        CHECK_EQ(t[2]["parts"][1]["functionResponse"]["response"]["content"].get<std::string>(), small); // small kept
        CHECK_EQ(t[1]["parts"][0]["functionCall"]["name"].get<std::string>(), "str_replace_based_edit_tool"); // call preserved
        CHECK_EQ(t[1]["parts"][0]["functionCall"]["args"]["path"].get<std::string>(), "src/main.cpp"); // call args preserved
        CHECK_EQ((int)t.size(), 4); // same number of messages
    }

    // --- pure: the live history is never mutated by the trimmer -----------
    {
        json hist = json::array();
        hist.push_back({{"role", "tool"}, {"tool_call_id", "c1"}, {"content", big}});
        be.loadHistory(hist);

        json before = be.getHistory();
        json t = be.buildTrimmedHistoryForSummary();

        CHECK_TRUE(be.getHistory() == before);   // live history untouched
        CHECK_EQ(t[0]["content"].get<std::string>(), expectBigPh); // and the copy was trimmed
    }

    // --- no-op: nothing oversized maps back to an identical copy ---------
    {
        json hist = json::array();
        hist.push_back({{"role", "user"}, {"content", "hi"}});
        hist.push_back({{"role", "assistant"}, {"content", "hello there"}});
        be.loadHistory(hist);

        CHECK_TRUE(be.buildTrimmedHistoryForSummary() == be.getHistory());
    }

    std::cout << (g_failures ? "FAILED " : "ok ") << (g_checks - g_failures) << "/" << g_checks
              << " checks\n";
    return g_failures ? 1 : 0;
}
