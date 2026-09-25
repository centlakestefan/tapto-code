// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tapto {

// The conversation as it stood after the last completed turn, kept so a chat
// ended by accident (Ctrl-C, a closed window) can be picked up again with
// --resume or /resume.
//
// The history is the provider's own wire format, which differs per dialect,
// so it can only be handed back to a client of the same dialect. The system
// prompt and tools are not stored: they are rebuilt from config as usual, and
// the granted folders are stored so the tools that depend on them come back.
struct SavedSession {
    std::string provider;     // provider name, as chosen with --provider or config
    std::string dialect;      // claude | openai | gemini: what the history is shaped for
    std::string model;
    std::string saved_at;     // local time, "YYYY-MM-DD HH:MM"; filled in by save_session
    struct Grant {
        std::string path;
        bool writable = false;
    };
    std::vector<Grant> folders; // /add-folder grants, the working directory not included
    // A prompt that was sent but whose turn never finished. The history does
    // not contain it, so it is shown on resume for the user to send again.
    std::string pending_prompt;
    nlohmann::json history = nlohmann::json::array();
};

// Where the working directory's session is kept: beside its local config,
// under ~/.tapto/projects/<encoded-cwd>/, never in the project itself.
std::filesystem::path session_path();

// Write `s` to `path`, replacing what was there. The file is written beside
// the target and renamed over it, so a crash mid-write leaves the previous
// session intact. Returns an empty string on success, else what went wrong.
std::string save_session(const std::filesystem::path& path, SavedSession s);

// Read a saved session. nullopt when there is none; a file that exists but
// cannot be used sets `error` and also returns nullopt.
std::optional<SavedSession> load_session(const std::filesystem::path& path, std::string& error);

} // namespace tapto
