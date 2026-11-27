// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

// What a model server's Prometheus /metrics says about its load, and the few
// words the metrics plugin puts on the status line from it. Header-only and
// free of the plugin boundary, so tests/test_metrics.cpp reads it directly.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace metrics {

// vLLM's vllm:... and llama.cpp's llamacpp:..., summed over the server's models
// and engines. A value it does not report is negative.
struct ServerMetrics {
    double running = -1;   // requests being generated now
    double waiting = -1;   // requests queued behind them
    double kv_usage = -1;  // KV cache in use, 0..1 (the fullest engine)
    double generated = -1; // tokens generated since the server started
};

inline ServerMetrics parse_server_metrics(const std::string& text) {
    ServerMetrics m;
    // Add a sample to a running total that starts unknown (negative).
    auto add = [](double& total, double v) { total = (total < 0 ? 0 : total) + v; };
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        // name{labels} value [timestamp]; a label value may hold spaces and
        // braces, so the labels are skipped by their quotes.
        size_t i = 0;
        while (i < line.size() && line[i] != '{' && line[i] != ' ') ++i;
        const std::string name = line.substr(0, i);
        if (i < line.size() && line[i] == '{') {
            bool quoted = false;
            for (++i; i < line.size(); ++i) {
                if (line[i] == '\\' && quoted) { ++i; continue; }
                if (line[i] == '"') quoted = !quoted;
                else if (line[i] == '}' && !quoted) { ++i; break; }
            }
        }
        while (i < line.size() && line[i] == ' ') ++i;
        const size_t end = line.find(' ', i);
        const std::string number = line.substr(i, end == std::string::npos ? std::string::npos : end - i);
        char* stop = nullptr;
        const double v = std::strtod(number.c_str(), &stop);
        if (number.empty() || stop == number.c_str() || !std::isfinite(v)) continue;

        if (name == "vllm:num_requests_running" || name == "llamacpp:requests_processing") {
            add(m.running, v);
        } else if (name == "vllm:num_requests_waiting" || name == "llamacpp:requests_deferred") {
            add(m.waiting, v);
        } else if (name == "vllm:gpu_cache_usage_perc" || name == "vllm:kv_cache_usage_perc" ||
                   name == "llamacpp:kv_cache_usage_ratio") {
            m.kv_usage = (std::max)(m.kv_usage, v); // engines' caches are separate: the fullest
        } else if (name == "vllm:generation_tokens_total" || name == "llamacpp:tokens_predicted_total") {
            add(m.generated, v);
        }
    }
    return m;
}

// "27 tok/s · 1 user · 2 waiting · cache 63%", leaving out what is unknown
// (`tokens_per_s` < 0: no rate yet), and the queue when nothing waits.
inline std::string format_server_metrics(const ServerMetrics& m, double tokens_per_s) {
    std::vector<std::string> parts;
    auto count = [](double v) { return std::to_string(static_cast<long long>(std::llround(v))); };
    if (tokens_per_s >= 0) parts.push_back(count(tokens_per_s) + " tok/s");
    // The requests the server is generating for, each someone's: its users now.
    if (m.running >= 0)
        parts.push_back(count(m.running) + (std::llround(m.running) == 1 ? " user" : " users"));
    // A queue only when there is one: nothing waiting is the usual case.
    if (m.waiting >= 0.5) parts.push_back(count(m.waiting) + " waiting");
    if (m.kv_usage >= 0) parts.push_back("cache " + count(m.kv_usage * 100) + "%");
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : " \xC2\xB7 ") + p; // ·
    return out;
}

} // namespace metrics
