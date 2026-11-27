// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

// The metrics plugin: the model server's load on the host's status line while
// the model works -- "27 tok/s · 1 user · cache 63%" -- from the server's own
// Prometheus /metrics (vLLM, llama.cpp).
//
// Loaded with /add-plugin metrics, or `plugin.metrics = ro`. Its one setting:
//
//   metrics-url   the server's metrics, e.g. http://gpu-box:8000/metrics
//
// The provider's URL is often a gateway (LiteLLM) that has no such numbers,
// so the server's metrics have a URL of their own. The plugin has no tools.
//
// The host asks for the text about once a second, on a thread of its own
// (ITaptoPluginStatus), and the answer must come at once, so the fetching is
// done here on a thread of the plugin's: Status hands back what was last
// fetched. The thread fetches every two seconds while the host is asking,
// every ten after a fetch failed, and not at all once the host has stopped
// asking, between turns.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "server_metrics.h"
#include "tapto/plugin.h"

using json = nlohmann::json;
using scom::SRESULT;
using tapto::TaptoString;

namespace {

using Clock = std::chrono::steady_clock;

SRESULT hand_out(std::string_view s, char*& out) {
    TaptoString t = TaptoString::copy(s);
    if (!t) return scom::ERR_NO_MEMORY;
    out = t.detach();
    return scom::OK;
}

void release_host() {
    if (tapto::ITaptoHost*& h = tapto::plugin_host()) {
        h->Release();
        h = nullptr;
    }
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

class MetricsPlugin final : public scom::Base,
                            public tapto::ITaptoPlugin,
                            public tapto::ITaptoPluginUnload,
                            public tapto::ITaptoPluginStatus {
public:
    SCOM_IMPLEMENT_BASE_EX(MetricsPlugin, tapto::ITaptoPlugin)
    SCOM_BEGIN_INTERFACE_MAP(MetricsPlugin)
        SCOM_INTERFACE(tapto::ITaptoPlugin)
        SCOM_INTERFACE(tapto::ITaptoPluginUnload)
        SCOM_INTERFACE(tapto::ITaptoPluginStatus)
    SCOM_END_INTERFACE_MAP()

    SRESULT SCOM_CALL Describe(char*& json_out) override {
        const json d = {
            {"name", "metrics"},
            {"version", "1.0.0"},
            {"config_keys", {"metrics-url"}},
            {"tools", json::array()},
        };
        return hand_out(d.dump(), json_out);
    }

    SRESULT SCOM_CALL Configure(const char* json_config) override {
        const json c = json::parse(json_config ? json_config : "{}", nullptr, false);
        std::string url;
        if (c.is_object() && c.contains("metrics-url") && c["metrics-url"].is_string())
            url = trim(c["metrics-url"].get<std::string>());
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_url = url;
            m_text.clear();
        }
        if (!m_thread.joinable()) m_thread = std::thread([this] { run(); });
        m_wake.notify_all();
        return scom::OK;
    }

    SRESULT SCOM_CALL CallTool(const char* name, const char*, char*& result_out) override {
        return hand_out(std::string("ERROR: the metrics plugin has no tool ") + (name ? name : ""),
                        result_out);
    }

    SRESULT SCOM_CALL Shutdown() override {
        stop();
        release_host();
        return scom::OK;
    }

    // What was last fetched; the fetch thread is told the host is asking.
    // `json_context` is not needed: the server is the one metrics-url names.
    SRESULT SCOM_CALL Status(const char*, char*& text_out) override {
        m_asked.store(Clock::now().time_since_epoch().count());
        m_wake.notify_all();
        std::string text;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            text = m_text;
        }
        return hand_out(text, text_out);
    }

private:
    ~MetricsPlugin() {
        stop();
        release_host();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_wake.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

    void run() {
        std::string url;                       // what `http` was made for
        std::unique_ptr<httplib::Client> http;
        std::string path = "/metrics";
        double last_generated = -1;            // for the rate: the last fetch's count
        Clock::time_point last_at;
        Clock::time_point next_fetch;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait_for(lock, std::chrono::milliseconds(250));
                if (m_stop) return;
                if (m_url != url) {
                    // A new URL: a new client, and no rate across the change.
                    url = m_url;
                    http.reset();
                    last_generated = -1;
                    next_fetch = Clock::now();
                }
            }
            const auto now = Clock::now();
            // The host asks while the model works; between turns, no fetches.
            const Clock::time_point asked{Clock::duration(m_asked.load())};
            if (url.empty() || now - asked > std::chrono::seconds(5)) {
                last_generated = -1;
                continue;
            }
            if (now < next_fetch) continue;

            if (!http) {
                // "http://box:8000/metrics": the client takes the scheme, host and
                // port, the request the path.
                const size_t scheme = url.find("://");
                const size_t slash = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
                const std::string base = slash == std::string::npos ? url : url.substr(0, slash);
                path = slash == std::string::npos ? "/metrics" : url.substr(slash);
                http = std::make_unique<httplib::Client>(base);
                if (base.rfind("https://", 0) == 0) http->enable_server_certificate_verification(true);
                http->set_connection_timeout(2, 0);
                http->set_read_timeout(2, 0);
            }
            metrics::ServerMetrics m; // all unknown when the fetch fails
            const auto res = http->Get(path);
            const bool ok = res && res->status == 200;
            if (ok) m = metrics::parse_server_metrics(res->body);
            next_fetch = Clock::now() + std::chrono::seconds(ok ? 2 : 10);
            // The rate is the server's: every request it generates for.
            double rate = -1;
            if (m.generated >= 0 && last_generated >= 0 && m.generated >= last_generated) {
                const double dt = std::chrono::duration<double>(now - last_at).count();
                if (dt > 0) rate = (m.generated - last_generated) / dt;
            }
            last_generated = m.generated;
            last_at = now;
            const std::string text = metrics::format_server_metrics(m, rate);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_text = text;
        }
    }

    std::mutex m_mutex;              // m_url, m_text, m_stop
    std::condition_variable m_wake;
    std::string m_url;
    std::string m_text;
    bool m_stop = false;
    std::atomic<Clock::rep> m_asked{0}; // when Status was last called
    std::thread m_thread;
    tapto::PluginObjectCount m_count;
};

} // namespace

TAPTO_PLUGIN_EXPORT SRESULT SCOM_CALL TaptoGetPlugin(tapto::ITaptoHost* host,
                                                     const scom::GUID& riid, void** ppv) {
    if (!host || !ppv) return scom::ERR_BAD_ARGUMENT;
    *ppv = nullptr;
    if (!tapto::plugin_host()) {
        host->AddRef();
        tapto::plugin_host() = host;
    }
    auto* p = new MetricsPlugin;
    const SRESULT r = p->QueryInterface(riid, ppv);
    p->Release();
    return r;
}

TAPTO_PLUGIN_EXPORT SRESULT SCOM_CALL TaptoCanUnloadNow() {
    return tapto::plugin_objects() == 0 ? scom::OK : scom::ST_FALSE;
}
