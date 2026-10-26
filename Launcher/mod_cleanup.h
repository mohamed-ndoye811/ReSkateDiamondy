#pragma once

#include "Engine/Vfs/mod_cost.h"
#include "Engine/Vfs/mod_list.h"
#include "Engine/Vfs/mod_usage.h"
#include "Launcher/mod_profiles.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Switching many mods off at once, in a way that can be taken back: the mods unused for some days
// (Engine/Vfs/mod_usage.h), or the heaviest until the rest fit a video memory budget
// (Engine/Vfs/mod_cost.h). A plan says what would go and what stays and why; nothing changes
// until the player has seen it. Before a plan changes mods.json, the mods as they were are saved as
// a profile of their own (a snapshot), so loading that profile puts everything back. A light
// profile is a new profile beside the others, never the one in use rewritten.
//
// Every change is one change of mods.json, not one per mod: each change of the set of mods
// empties the game's shader cache (mod_manager.h game_data_signature), and its shaders are then
// built again while the player skates.
namespace dingosdk::launcher_cleanup {

// What each mod needs, by folder name, as its manifest.json lists them (mod_manager.h
// listed_dependencies). Compared ignoring ASCII case.
using Dependencies = std::map<std::string, std::vector<std::string>, std::less<>>;

struct Plan {
    std::vector<std::string> switch_off;                    // folder names, in load order
    std::map<std::string, std::string, std::less<>> kept;   // would have gone, but stay: why
    [[nodiscard]] bool empty() const noexcept { return switch_off.empty(); }
};

// Whether the game can see the mod used (Extension/Assets/mod_usage_tracker.h): it brings maps, a
// plugin or cosmetic items (`cost`'s items, null when not measured), and no songs or objects, whose
// use is not told apart by mod. A mod that only changes the game's data is never seen used either.
[[nodiscard]] bool watched_mod(const mods::Mod& mod, const mods::ModCost* cost) noexcept;
using Names = std::set<std::string, std::less<>>;

// The enabled mods of no use for `days` days at `now` (mod_usage.h unused_for). Only those in
// `watched` (watched_mod) can go: one whose use cannot be seen stays, as do the mods the player
// keeps (`keep`) and a mod an enabled mod that stays needs.
[[nodiscard]] Plan unused_plan(const mods::ModList& list, const mods::Usage& usage, int days, std::int64_t now,
                               const Dependencies& dependencies, const Names& watched, const Names& keep = {});

// The enabled mods heaviest first (mod_cost.h video_bytes) switched off until what stays can take
// `budget` bytes of video memory or less at once (mod_cost.h peak_video: `maps` are the mods that
// bring maps, and only one map loads at a time). A mod not measured yet stays (it is not known to be
// heavy), as do the mods the player keeps, one a mod that stays needs, and a mod with no video
// memory to give back. `costs` are taken as they are: the caller leaves out the ones measured for
// other files.
[[nodiscard]] Plan light_plan(const mods::ModList& list, const mods::Costs& costs, const Names& maps, std::uint64_t budget,
                              const Dependencies& dependencies, const Names& keep = {});

// What mods.json holds once `plan` is made: every entry in its place, those it names switched off.
[[nodiscard]] std::vector<mods::ModEntry> switched_off(const mods::ModList& list, const Plan& plan);

// Snapshots are the profiles named with this prefix, then when they were taken.
inline constexpr std::string_view snapshot_prefix = "Before cleanup ";
inline constexpr std::size_t kept_snapshots = 3;
[[nodiscard]] bool is_snapshot(std::string_view profile_name) noexcept;

// Saves the mods as they are now (launcher_profiles::capture) as the snapshot "Before cleanup
// <when>", `when` as the caller writes the time ("2026-10-25 18h40"). The oldest snapshots past
// kept_snapshots go; no other profile is touched and the profile in use stays the one in use.
// False, with nothing changed, when there is no room for one more profile: the plan must not be
// made then, as it could not be taken back. The snapshot's name is in `name`.
[[nodiscard]] bool add_snapshot(launcher_profiles::Profiles& profiles, const mods::ModList& list, std::string_view when,
                                std::string& name);

// A new profile: the mods as they are now with `plan` made, named "<base> (light)" (or with a number
// when that is taken). Not added to `profiles`, which only says which names are taken.
[[nodiscard]] launcher_profiles::Profile light_profile(const launcher_profiles::Profiles& profiles, const mods::ModList& list,
                                                       const Plan& plan, std::string_view base);

} // namespace dingosdk::launcher_cleanup
