#pragma once
#include "Engine/Vfs/mod_usage.h"
#include <string_view>

// Notes, as the game runs, when each enabled mod is of use (Engine/Vfs/mod_usage.h): a map of it
// loaded, an item of it worn on the skater or the board, its plugin doing something the player
// asked for or sees. Kept in Mods/.reskate-mod-usage.json, on this PC only; the launcher reads it
// to offer switching off what has not been used for a while.
//
// Songs and placed objects are not told apart by mod yet, so a mod that brings them is never
// offered for being unused (Launcher/mod_cleanup.h watched_mod).
namespace dingosdk::mod_usage_tracker {
// A use of the mod with this folder name (a variant's "<mod>#<variant>" counts for its mod).
// Any thread; cheap.
void note(std::string_view mod, mods::UseKind kind) noexcept;
// Client thread, every tick: every 30 seconds, the enabled mods are noted as seen, the level loaded
// and what the skater wears as used; what changed is written out every five minutes, and as the
// game closes. Reads the file the first time.
void poll() noexcept;
} // namespace dingosdk::mod_usage_tracker
