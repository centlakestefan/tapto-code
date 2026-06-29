// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/claude.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include "tapto/log.h"

using json = nlohmann::json;

namespace {

/// <summary>
/// Replace any invalid UTF-8 byte sequences with U+FFFD. Tool results are
/// supposed to be valid UTF-8, but a stray non-UTF-8 byte would make the next
/// json::dump() of the conversation history throw and abort the turn. This is a
/// safety net only — the offending tool should be fixed at its source.
/// </summary>
std::string sanitizeUtf8(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    const size_t n = in.size();
    auto is_cont = [&](size_t k) {
        return k < n && (static_cast<unsigned char>(in[k]) & 0xC0) == 0x80;
    };

    size_t i = 0;
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        if (c < 0x80) {
            out.push_back(in[i]);
            i += 1;
        } else if ((c & 0xE0) == 0xC0 && c >= 0xC2 && is_cont(i + 1)) {
            out.append(in, i, 2);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && is_cont(i + 1) && is_cont(i + 2)) {
            out.append(in, i, 3);
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && c <= 0xF4 && is_cont(i + 1) && is_cont(i + 2) && is_cont(i + 3)) {
            out.append(in, i, 4);
            i += 4;
        } else {
            out += "\xEF\xBF\xBD"; // U+FFFD
            i += 1;
        }
    }
    return out;
}

std::string sanitizeToolResult(const std::string& result, const std::string& tool_name) {
    std::string clean = sanitizeUtf8(result);
    if (clean.size() != result.size()) {
        mclog("WARN: tool '" + tool_name + "' returned non-UTF-8 bytes; sanitizing with U+FFFD\n");
    }
    return clean;
}

} // namespace

ClaudeClient::ClaudeClient(const AiConfig* config, const std::string& url, const std::string& model, const std::string& apiKey)
    : m_config(config), m_model(model), m_apiKeyRef(apiKey) {
    m_url = url;

    mclog("ClaudeClient initialized with url=" + m_url + "\n");
}

/// <summary>Initializes the persistent client with proper timeout and keep-alive settings.</summary>
void ClaudeClient::init_api_client() {
    mclog("Initializing API client connection to " + m_url + "\n");

    m_api_client = std::make_unique<httplib::Client>(m_url);
    m_api_client->enable_server_certificate_verification(true);

    m_api_client->set_connection_timeout(CONNECTION_TIMEOUT_SECONDS);
    m_api_client->set_read_timeout(READ_TIMEOUT_SECONDS);
    m_api_client->set_keep_alive(true);

    mclog("API client initialized with keep-alive enabled\n");
}

/// <summary>Returns the configured Claude API key.</summary>
std::string ClaudeClient::getApiKey() {
    if (!m_apiKeyRef.empty()) {
        return m_apiKeyRef;
    }
    throw std::runtime_error("Claude API key is not set");
}

/// <summary>Makes API call to Claude with retry logic and caching optimization.</summary>
json ClaudeClient::call_claude(const std::string& user_message, const json& tools, const json& conversation_history) {
    // Build messages array
    json messages = conversation_history;
    if (!user_message.empty()) {
        messages.push_back({
            {"role", "user"},
            {"content", user_message}
            });
    }

    // OPTIMIZATION: Add cache control to conversation history
    // Cache up to the second-to-last message (everything before the new user message)
    if (messages.size() >= 2) {
        size_t cache_index = messages.size() - 2;
        auto& cache_msg = messages[cache_index];

        // Handle both string and array content
        if (cache_msg["content"].is_string()) {
            // Convert to array format with cache control
            std::string content_text = cache_msg["content"];
            cache_msg["content"] = json::array({
                {
                    {"type", "text"},
                    {"text", content_text},
                    {"cache_control", {{"type", "ephemeral"}}}
                }
                });
        }
        else if (cache_msg["content"].is_array() && !cache_msg["content"].empty()) {
            // Add cache control to last block in array
            cache_msg["content"].back()["cache_control"] = { {"type", "ephemeral"} };
        }
    }

    // Build request body
    json request_body = {
        {"model", m_model},
        {"max_tokens", m_config->maxOutputTokens()},
        {"messages", messages}
    };

    // Provider-level extended-thinking override. Claude has no explicit
    // "thinking off" — omitting the field is already off — so we only emit
    // the field for budget > 0.
    if (m_thinkingBudget.has_value() && m_thinkingBudget.value() > 0) {
        request_body["thinking"] = {
            {"type", "enabled"},
            {"budget_tokens", m_thinkingBudget.value()}
        };
    }

    // System prompt with caching (only when one is set).
    if (!m_systemPrompt.empty()) {
        request_body["system"] = json::array({
            {
                {"type", "text"},
                {"text", m_systemPrompt},
                {"cache_control", {{"type", "ephemeral"}}}
            }
            });
    }

    // OPTIMIZATION: Cache tool definitions (they don't change). Only sent when
    // tools are actually present.
    if (!tools.empty()) {
        json cached_tools = tools;
        cached_tools.back()["cache_control"] = { {"type", "ephemeral"} };
        request_body["tools"] = cached_tools;
    }

    std::string api_key = getApiKey();

    httplib::Headers headers = {
        {"x-api-key", api_key},
        {"anthropic-version", "2023-06-01"},
        {"content-type", "application/json"}
    };

    int connection_retry_count = 0;
    int rate_limit_retries = 0;
    int server_error_retries = 0;

    for (;;) {
        auto res = m_api_client->Post("/v1/messages", headers, request_body.dump(), "application/json");

        // Handle connection errors
        if (!res) {
            auto error = res.error();
            std::string error_msg = httplib::to_string(error);

            std::ostringstream log_msg;
            log_msg << "Connection failed (attempt " << (connection_retry_count + 1)
                   << "/" << MAX_CONNECTION_RETRIES << "): " << error_msg << "\n";
            mclog(log_msg.str());

            // Check if we should retry
            if (connection_retry_count >= MAX_CONNECTION_RETRIES) {
                mclog("Exceeded maximum connection retries\n");
                std::ostringstream exception_msg;
                exception_msg << "Connection failed after " << MAX_CONNECTION_RETRIES
                            << " retries: " << error_msg;
                throw std::runtime_error(exception_msg.str());
            }

            connection_retry_count++;

            // Reinitialize connection on connection failure
            mclog("Reinitializing API client connection...\n");
            init_api_client();

            // Exponential backoff for connection retries: 2, 4, 8, 16, 32 seconds
            // (shift clamped so it can't overflow if the retry limit is raised)
            int wait_seconds = std::min(CONNECTION_BACKOFF_BASE * (1 << std::min(connection_retry_count - 1, 16)), CONNECTION_BACKOFF_MAX);

            std::ostringstream retry_msg;
            retry_msg << "Retrying in " << wait_seconds << " seconds...\n";
            mclog(retry_msg.str());

            std::this_thread::sleep_for(std::chrono::seconds(wait_seconds));
            continue;
        }

        // Reset connection retry counter on successful connection
        connection_retry_count = 0;

        // Handle HTTP status codes
        int status = res->status;

        // Success
        if (status == 200) {
            return json::parse(res->body);
        }

        // Rate limiting (429)
        if (status == 429) {
            if (rate_limit_retries >= MAX_RATE_LIMIT_RETRIES) {
                std::ostringstream err;
                err << "Exceeded maximum rate limit retries (" << MAX_RATE_LIMIT_RETRIES << ")\n";
                mclog(err.str());
                throw std::runtime_error("Exceeded maximum rate limit retries");
            }
            rate_limit_retries++;

            int wait_seconds = 0;

            // Try to parse retry-after header with error handling
            if (res->has_header("retry-after")) {
                try {
                    wait_seconds = std::stoi(res->get_header_value("retry-after"));
                }
                catch (const std::exception& e) {
                    std::ostringstream err;
                    err << "Failed to parse retry-after header: " << e.what() << "\n";
                    mclog(err.str());
                    wait_seconds = 0;
                }
            }

            // Use exponential backoff if no valid retry-after header.
            if (wait_seconds <= 0) {
                wait_seconds = RATE_LIMIT_BACKOFF_BASE * (1 << std::min(rate_limit_retries - 1, 16));
            }
            // Cap whatever we got (header or backoff) so a hostile/buggy
            // retry-after can't make us sleep indefinitely.
            wait_seconds = std::min(wait_seconds, RATE_LIMIT_BACKOFF_MAX);

            std::ostringstream log_msg;
            log_msg << "Rate limited (attempt " << rate_limit_retries << "/" << MAX_RATE_LIMIT_RETRIES
                   << "). Waiting " << wait_seconds << " seconds before retry\n";
            mclog(log_msg.str());

            std::this_thread::sleep_for(std::chrono::seconds(wait_seconds));
            continue;
        }

        // Client errors (4xx) - generally not retryable
        if (status >= 400 && status < 500) {
            std::string error_detail = res->body.length() > 500 ? res->body.substr(0, 500) + "..." : res->body;
            std::ostringstream log_msg;
            log_msg << "Client error " << status << ": " << error_detail << "\n";
            mclog(log_msg.str());

            std::ostringstream exception_msg;
            switch (status) {
            case 400:
                exception_msg << "Bad Request (400): Invalid request format or parameters - " << error_detail;
                break;
            case 401:
                exception_msg << "Unauthorized (401): Invalid or missing API key - " << error_detail;
                break;
            case 403:
                exception_msg << "Forbidden (403): Access denied - " << error_detail;
                break;
            case 404:
                exception_msg << "Not Found (404): Endpoint not found - " << error_detail;
                break;
            default:
                exception_msg << "Client error (" << status << "): " << error_detail;
                break;
            }
            throw std::runtime_error(exception_msg.str());
        }

        // Server errors (5xx) - can be retried
        if (status >= 500 && status < 600) {
            if (server_error_retries >= MAX_SERVER_ERROR_RETRIES) {
                std::string error_detail = res->body.length() > 500 ? res->body.substr(0, 500) + "..." : res->body;
                std::ostringstream err;
                err << "Exceeded maximum server error retries (" << MAX_SERVER_ERROR_RETRIES << ")\n";
                mclog(err.str());

                std::ostringstream exception_msg;
                exception_msg << "Server error (" << status << ") after "
                            << MAX_SERVER_ERROR_RETRIES << " retries: " << error_detail;
                throw std::runtime_error(exception_msg.str());
            }
            server_error_retries++;

            int wait_seconds = 0;

            // For 503 Service Unavailable, respect retry-after header
            if (status == 503 && res->has_header("retry-after")) {
                try {
                    wait_seconds = std::stoi(res->get_header_value("retry-after"));
                }
                catch (const std::exception& e) {
                    std::ostringstream err;
                    err << "Failed to parse retry-after header: " << e.what() << "\n";
                    mclog(err.str());
                    wait_seconds = 0;
                }
            }

            // Use exponential backoff if no retry-after header.
            if (wait_seconds <= 0) {
                wait_seconds = SERVER_ERROR_BACKOFF_BASE * (1 << std::min(server_error_retries - 1, 16));
            }
            // Cap whatever we got (header or backoff) against a hostile retry-after.
            wait_seconds = std::min(wait_seconds, SERVER_ERROR_BACKOFF_MAX);

            std::string error_msg;
            switch (status) {
            case 500: error_msg = "Internal Server Error"; break;
            case 502: error_msg = "Bad Gateway"; break;
            case 503: error_msg = "Service Unavailable"; break;
            case 504: error_msg = "Gateway Timeout"; break;
            default: error_msg = "Server Error"; break;
            }

            std::ostringstream log_msg;
            log_msg << error_msg << " (" << status << ") - attempt "
                   << server_error_retries << "/" << MAX_SERVER_ERROR_RETRIES
                   << ". Retrying in " << wait_seconds << " seconds\n";
            mclog(log_msg.str());

            std::this_thread::sleep_for(std::chrono::seconds(wait_seconds));
            continue;
        }

        // Unknown status code
        std::string error_detail = res->body.length() > 500 ? res->body.substr(0, 500) + "..." : res->body;
        mclog("Unexpected HTTP status " + std::to_string(status) + ": " + error_detail + "\n");
        throw std::runtime_error("Unexpected HTTP status " + std::to_string(status) + ": " + error_detail);
    }
}

/// <summary>Sends one user message and returns Claude's text reply, running any
/// tools the model calls along the way.</summary>
std::string ClaudeClient::chat(Context& context, const std::string& user_message) {

    m_tool_registry.clear();
    m_tools.clear();

    // Wrap all tools to bind the context
    for (const auto& tool : context.tools) {
        auto executor = tool.executor;
        m_tool_registry[tool.name] = [executor, &context](const json& input) {
            return executor(context, input);
        };
        m_tools.push_back(tool);
    }

    mclog("Start " + user_message + " ===\n");

    // Same-line progress feedback: "Thinking..." while waiting on the API and
    // "[tool] ..." while a tool actually runs.
    bool printed_status = false;
    auto set_status = [&](std::string s) {
        constexpr size_t W = 78;
        if (s.size() > W) s = s.substr(0, W - 3) + "...";
        if (!printed_status) std::cout << "\x1b[?25l"; // hide cursor during the wait
        std::cout << "\r" << s << "\x1b[K" << std::flush; // erase to end of line (no padding)
        printed_status = true;
    };

    auto has_tool_use = [](const json& resp) {
        if (!resp.contains("content")) return false;
        for (const auto& b : resp["content"]) {
            if (b.value("type", std::string()) == "tool_use") return true;
        }
        return false;
    };

    // Use Claude-native tool schemas when talking to Anthropic, generic otherwise.
    ToolFormat fmt = (m_url.find("anthropic.com") != std::string::npos) ? ToolFormat::Claude : ToolFormat::Generic;
    json tools = json::array();
    for (const auto& tool_spec : m_tools) {
        tools.push_back(tool_definition_to_json(tool_spec, fmt));
    }

    int iteration = 0;
    const int max_iterations = m_config->maxToolIterations();

    if (!m_api_client) init_api_client();

    // Add the user message to persistent history.
    if (!user_message.empty()) {
        m_conversation_history.push_back({
            {"role", "user"},
            {"content", user_message}
            });
    }

    set_status("Thinking...");
    json response = call_claude("", tools, m_conversation_history);
    if (!response.contains("content") || !response.contains("stop_reason")) {
        throw std::runtime_error("Invalid Claude response: " + response.dump());
    }
    m_conversation_history.push_back({
        {"role", "assistant"},
        {"content", response["content"]}
        });

    // Tool loop: keep going while the model's latest message contains a tool_use
    // block. Driving on presence (rather than stop_reason) means a turn cut off
    // by max_tokens that still carries a tool_use is handled, not orphaned.
    while (has_tool_use(response) && iteration < max_iterations) {
        iteration++;
        mclog("=== Iteration " + std::to_string(iteration) + " ===\n");

        json tool_results = json::array();

        for (const auto& block : response["content"]) {
            if (block["type"] == "text") {
                mclog("assistant says: " + block["text"].get<std::string>() + "\n");
            }
            else if (block["type"] == "tool_use") {
                std::string tool_name = block["name"];
                std::string tool_id = block["id"];
                json tool_input = block["input"];

                set_status("[tool] " + getToolDisplayName(tool_name, tool_input));
                mclog("Executing tool: " + tool_name + "\n");
                mclog("Input: " + tool_input.dump(2) + "\n");

                std::string result;
                auto it = m_tool_registry.find(tool_name);
                if (it != m_tool_registry.end()) {
                    try {
                        result = it->second(tool_input);
                    }
                    catch (const std::exception& e) {
                        result = "ERROR: Tool execution failed: " + std::string(e.what());
                        mclog("Tool execution exception: " + std::string(e.what()) + "\n");
                    }
                }
                else {
                    result = formatUnknownToolError(tool_name, m_tool_registry);
                    mclog("Unknown tool requested: " + tool_name + "\n");
                }

                tool_results.push_back({
                    {"type", "tool_result"},
                    {"tool_use_id", tool_id},
                    {"content", sanitizeToolResult(result, tool_name)}
                    });
            }
        }

        // Send tool results back as a user turn.
        m_conversation_history.push_back({
            {"role", "user"},
            {"content", tool_results}
            });

        set_status("Thinking...");
        response = call_claude("", tools, m_conversation_history);
        if (!response.contains("content") || !response.contains("stop_reason")) {
            throw std::runtime_error("Invalid Claude response: " + response.dump());
        }
        m_conversation_history.push_back({
            {"role", "assistant"},
            {"content", response["content"]}
            });
    }

    if (iteration >= max_iterations) {
        mclog("Warning: Reached maximum iterations\n");
    }

    // If we stopped with an unanswered tool_use (iteration cap), answer it with a
    // synthetic result so the conversation stays valid on the next turn — Claude
    // requires a tool_result after every tool_use.
    if (has_tool_use(response)) {
        json tool_results = json::array();
        for (const auto& block : response["content"]) {
            if (block.value("type", std::string()) == "tool_use") {
                tool_results.push_back({
                    {"type", "tool_result"},
                    {"tool_use_id", block.value("id", std::string())},
                    {"content", "Tool not run: iteration limit reached."}
                });
            }
        }
        m_conversation_history.push_back({{"role", "user"}, {"content", tool_results}});
    }

    mclog("=== Final Response === " + response.value("stop_reason", std::string("unknown")) + "\n");

    // Clear the progress line and restore the cursor before the reply prints.
    if (printed_status) std::cout << "\r\x1b[K\x1b[?25h" << std::flush;

    // Collect the assistant's final text reply.
    std::string reply;
    for (const auto& block : response["content"]) {
        if (block.value("type", std::string()) == "text") {
            reply += block.value("text", std::string());
        }
    }
    if (response.value("stop_reason", std::string()) == "max_tokens") {
        reply += "\n[truncated: hit max output tokens - raise max-output-tokens]";
    }
    return reply;
}

/// <summary>Starts a new conversation by clearing the conversation history.</summary>
void ClaudeClient::start() {
    m_conversation_history = json::array();
    mclog("Started new conversation (history cleared)\n");
}

/// <summary>Checks if conversation history exists.</summary>
bool ClaudeClient::hasHistory() const {
    return !m_conversation_history.empty();
}

/// <summary>Loads conversation history from stored format.</summary>
void ClaudeClient::loadHistory(const json& history) {
    if (!history.is_array()) {
        mclog("ERROR: Invalid history format - expected array\n");
        return;
    }

    m_conversation_history = history;
    mclog("Loaded conversation history with " + std::to_string(history.size()) + " messages\n");
}

/// <summary>Gets the current conversation history.</summary>
json ClaudeClient::getHistory() const {
    return m_conversation_history;
}

/// <summary>Sets the model for Claude client.</summary>
void ClaudeClient::setModel(const std::string& model) {
    m_model = model;
    mclog("ClaudeClient model updated to: " + model + "\n");
}

/// <summary>Sets the host URL for Claude client and resets the API client.</summary>
void ClaudeClient::setHost(const std::string& host) {
    m_url = host;
    m_api_client.reset();  // Reset client to force re-initialization with new URL
    mclog("ClaudeClient host updated to: " + host + " (API client reset)\n");
}

/// <summary>Sets the API key for Claude client.</summary>
void ClaudeClient::setApiKeyRef(const std::string& apiKey) {
    m_apiKeyRef = apiKey;
    mclog("ClaudeClient API key updated\n");
}
