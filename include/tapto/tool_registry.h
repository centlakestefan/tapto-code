// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

class Context;

// A tool executor receives the run context and the model-supplied JSON input
// and returns a string result that is fed back to the model.
using ToolExecutorFn = std::function<std::string(Context&, const nlohmann::json&)>;

// Declarative description of a tool the model may call.
struct ToolSpec {
    std::string name;
    std::string description;
    nlohmann::json parameters = nlohmann::json::object(); // JSON Schema for the args
    ToolExecutorFn executor;

    // When non-empty, this tool maps to a Claude server-side built-in tool of
    // this type (e.g. "text_editor_20250728"). For ToolFormat::Claude it is
    // declared as {type, name} with no schema (Claude knows the schema); other
    // providers fall back to the explicit `parameters` schema below.
    std::string claude_builtin_type;
};

// Wire formats for tool/function declarations across providers.
enum class ToolFormat { Claude, OpenAI, Gemini, Generic };

// Render a ToolSpec into the JSON shape a given provider expects. (The OpenAI
// caller wraps this in {"type":"function","function":{...}} itself.)
inline nlohmann::json tool_definition_to_json(const ToolSpec& spec, ToolFormat format) {
    using nlohmann::json;

    // Claude built-in tools are declared by type + name only; the schema is
    // built into the model and must not be sent.
    if (format == ToolFormat::Claude && !spec.claude_builtin_type.empty()) {
        return json{
            {"type", spec.claude_builtin_type},
            {"name", spec.name},
        };
    }

    switch (format) {
        case ToolFormat::Claude:
        case ToolFormat::Generic:
            return json{
                {"name", spec.name},
                {"description", spec.description},
                {"input_schema", spec.parameters},
            };
        case ToolFormat::OpenAI:
        case ToolFormat::Gemini:
            return json{
                {"name", spec.name},
                {"description", spec.description},
                {"parameters", spec.parameters},
            };
    }
    return json::object();
}

// Error string returned to the model when it calls an unknown tool. Templated
// on the registry type so it works with the clients' internal registry maps.
template <class Registry>
inline std::string formatUnknownToolError(const std::string& tool_name, const Registry& registry) {
    std::string msg = "ERROR: Unknown tool '" + tool_name + "'. Available tools:";
    if (registry.empty()) {
        msg += " (none)";
    } else {
        for (const auto& entry : registry) {
            msg += " " + entry.first;
        }
    }
    return msg;
}

// Human-friendly label for a tool invocation, shown on the status line while
// the tool is running and committed to the scroll buffer afterwards.
//
// str_replace_based_edit_tool:
//   view (directory)          -> "List dir/"
//   view (file, range)        -> "View src/foo.cpp:10-50"
//   view (file)               -> "View src/foo.cpp"
//   create                    -> "Create src/foo.cpp"
//   str_replace               -> "Edit src/foo.cpp"
//   insert                    -> "Insert src/foo.cpp"
//
// find_files:
//   with search_string        -> "Search *.cpp ~\"query\""
//   without                   -> "Find *.cpp"
//
// list_commands               -> "List commands"
//
// run_command:
//   with args                 -> "Run build arg1 arg2"
//   without args              -> "Run build"
//
// Unknown tool                -> raw tool name
inline std::string getToolDisplayName(const std::string& tool_name, const nlohmann::json& input) {

    if (tool_name == "str_replace_based_edit_tool" && input.is_object()) {
        std::string cmd  = input.contains("command") && input["command"].is_string()
                           ? input["command"].get<std::string>() : "";
        std::string path = input.contains("path") && input["path"].is_string()
                           ? input["path"].get<std::string>() : "";

        if (cmd == "view") {
            // Append a trailing slash hint for directories (best-effort: check
            // whether the last character already is one, otherwise just label it).
            std::string label = "View " + path;
            // view_range: show line numbers when present
            if (input.contains("view_range")) {
                const auto& vr = input["view_range"];
                // Accept both a JSON array and a string-encoded array.
                auto try_range = [&](const nlohmann::json& r) -> std::string {
                    if (r.is_array() && r.size() == 2 &&
                        r[0].is_number_integer() && r[1].is_number_integer()) {
                        int end = r[1].get<int>();
                        return ":" + std::to_string(r[0].get<int>()) +
                               "-" + (end < 0 ? "EOF" : std::to_string(end));
                    }
                    return "";
                };
                if (vr.is_string()) {
                    try {
                        label += try_range(nlohmann::json::parse(vr.get<std::string>()));
                    } catch (...) {}
                } else {
                    label += try_range(vr);
                }
            }
            return label;
        }
        if (cmd == "create")     return "Create "  + path;
        if (cmd == "str_replace") return "Edit "   + path;
        if (cmd == "insert")     return "Insert "  + path;
        // Unknown sub-command: fall back to path only.
        return path.empty() ? tool_name : path;
    }

    if (tool_name == "find_files" && input.is_object()) {
        std::string pattern = input.contains("filename") && input["filename"].is_string()
                              ? input["filename"].get<std::string>() : "*";
        bool has_query = input.contains("search_string") &&
                         input["search_string"].is_string() &&
                         !input["search_string"].get<std::string>().empty();
        if (has_query)
            return "Search " + pattern + " ~\"" + input["search_string"].get<std::string>() + "\"";
        return "Find " + pattern;
    }

    if (tool_name == "list_commands") {
        return "List commands";
    }

    if (tool_name == "run_command" && input.is_object()) {
        std::string name = input.contains("name") && input["name"].is_string()
                           ? input["name"].get<std::string>() : "?";
        std::string label = "Run " + name;
        if (input.contains("args") && input["args"].is_array()) {
            for (const auto& a : input["args"]) {
                if (a.is_string()) label += " " + a.get<std::string>();
            }
        } else if (input.contains("path") && input["path"].is_string()) {
            label += " " + input["path"].get<std::string>();
        }
        return label;
    }

    // Unknown / future tool: return the raw name so nothing is lost.
    return tool_name;
}
