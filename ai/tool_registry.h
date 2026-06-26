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

// Human-friendly label for a tool invocation: the tool name plus the most
// useful argument(s) so progress feedback shows what is actually happening
// (e.g. "str_replace_based_edit_tool str_replace src/foo.cpp").
inline std::string getToolDisplayName(const std::string& tool_name, const nlohmann::json& input) {
    std::string label = tool_name;
    if (input.is_object()) {
        if (input.contains("command") && input["command"].is_string()) {
            label += " " + input["command"].get<std::string>();
        }
        if (input.contains("path") && input["path"].is_string()) {
            label += " " + input["path"].get<std::string>();
        }
        if (input.contains("filename") && input["filename"].is_string()) {
            label += " " + input["filename"].get<std::string>();
        }
        // run_command takes its target as "name" (and there is no "command" key
        // to collide with on that tool), plus optional positional "args".
        if (!input.contains("command") && input.contains("name") && input["name"].is_string()) {
            label += " " + input["name"].get<std::string>();
        }
        if (input.contains("args") && input["args"].is_array()) {
            for (const auto& a : input["args"]) {
                if (a.is_string()) label += " " + a.get<std::string>();
            }
        }
        if (input.contains("search_string") && input["search_string"].is_string()) {
            const std::string& q = input["search_string"].get_ref<const std::string&>();
            if (!q.empty()) label += " ~\"" + q + "\"";
        }
    }
    return label;
}
