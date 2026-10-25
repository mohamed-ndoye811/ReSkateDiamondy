#pragma once

#include <filesystem>
#include <string_view>

// The files the SDK keeps about mods beside them (mod_cost.h, mod_usage.h) are replaced whole:
// written beside the file first, then renamed over it, so a game or launcher that stops halfway
// leaves the last good file rather than half of a new one.
namespace dingosdk::mods::detail {
// Throws a readable message naming `what` when the file cannot be written.
void replace_file(const std::filesystem::path& file, std::string_view text, std::string_view what);
}
