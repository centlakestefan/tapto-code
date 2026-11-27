// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB
//
// The metrics plugin's reading of a model server's Prometheus /metrics
// (plugins/metrics/server_metrics.h) and the words it puts on the status line:
// vLLM's and llama.cpp's text, the samples summed over their label sets
// (models, engines), the KV cache the fullest one, comments, timestamps and
// other metrics passed over; what is missing stays unknown, and shows nothing.

#include "server_metrics.h"

#include <cstddef>
#include <iostream>
#include <string>

namespace {

int g_checks = 0;
int g_failures = 0;

void check_true(int line, const char* what, bool cond) {
    ++g_checks;
    if (cond) return;
    ++g_failures;
    std::cout << "FAIL (line " << line << ") " << what << "\n";
}

void check_eq(int line, const char* what, const std::string& actual, const std::string& expected) {
    ++g_checks;
    if (actual == expected) return;
    ++g_failures;
    std::cout << "FAIL (line " << line << ") " << what << "\n"
              << "  expected: " << expected << "\n"
              << "  actual:   " << actual << "\n";
}

#define CHECK_TRUE(cond) check_true(__LINE__, #cond, (cond))
#define CHECK_EQ(actual, expected) check_eq(__LINE__, #actual, (actual), (expected))

} // namespace

int main() {
    using metrics::format_server_metrics;
    using metrics::parse_server_metrics;

    // vLLM, two engines: running and the generated tokens summed, the cache
    // the fuller one; a label value with spaces and braces in its quotes, a
    // timestamp after a value, a CRLF line end, and other metrics.
    const std::string vllm =
        "# HELP vllm:num_requests_running Number of requests in model execution batches.\n"
        "# TYPE vllm:num_requests_running gauge\n"
        "vllm:num_requests_running{engine=\"0\",model_name=\"qwen\"} 1.0\n"
        "vllm:num_requests_running{engine=\"1\",model_name=\"qwen\"} 2.0\r\n"
        "vllm:num_requests_waiting{engine=\"0\",model_name=\"a {b} c\"} 3.0 1731400000000\n"
        "vllm:kv_cache_usage_perc{engine=\"0\"} 0.25\n"
        "vllm:kv_cache_usage_perc{engine=\"1\"} 0.634\n"
        "vllm:generation_tokens_total{engine=\"0\"} 1000.0\n"
        "vllm:generation_tokens_total{engine=\"1\"} 234.0\n"
        "vllm:prompt_tokens_total{engine=\"0\"} 99999.0\n"
        "process_cpu_seconds_total 12.5\n";
    const auto m = parse_server_metrics(vllm);
    CHECK_TRUE(m.running == 3);
    CHECK_TRUE(m.waiting == 3);
    CHECK_TRUE(m.kv_usage > 0.63 && m.kv_usage < 0.64);
    CHECK_TRUE(m.generated == 1234);
    CHECK_EQ(format_server_metrics(m, 26.7),
             "27 tok/s \xC2\xB7 3 users \xC2\xB7 3 waiting \xC2\xB7 cache 63%");
    CHECK_EQ(format_server_metrics(m, -1),
             "3 users \xC2\xB7 3 waiting \xC2\xB7 cache 63%"); // no rate yet

    // llama.cpp; one user, and nothing waiting: no queue shown.
    const auto l = parse_server_metrics(
        "llamacpp:requests_processing 1\n"
        "llamacpp:requests_deferred 0\n"
        "llamacpp:kv_cache_usage_ratio 0.5\n"
        "llamacpp:tokens_predicted_total 42\n");
    CHECK_EQ(format_server_metrics(l, 0), "0 tok/s \xC2\xB7 1 user \xC2\xB7 cache 50%");
    CHECK_TRUE(l.generated == 42);

    // A gateway's page, an error page, or nothing: all unknown, nothing shown.
    const auto none = parse_server_metrics("litellm_requests_total 5\n<html>Not Found</html>\n");
    CHECK_TRUE(none.running < 0 && none.waiting < 0 && none.kv_usage < 0 && none.generated < 0);
    CHECK_EQ(format_server_metrics(none, -1), "");
    CHECK_EQ(format_server_metrics(parse_server_metrics(""), -1), "");

    std::cout << (g_failures ? "FAILED " : "ok ") << (g_checks - g_failures) << "/" << g_checks
              << " checks\n";
    return g_failures ? 1 : 0;
}
