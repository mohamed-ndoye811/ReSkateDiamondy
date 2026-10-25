#pragma once

#include "mod_cost.h"
#include "mod_list.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <vector>

// Measuring what mods cost (mod_cost.h) from their own bundles: every texture and mesh a mod ships
// in its own archives, read the way the merge reads them, and its folder's size. Only what is the
// mod's own counts: a bundle it ships points at the game's copy of most of what it holds.
//
// Reading a texture's or a mesh's header takes the game's Oodle codec, so this runs on Windows, in
// the launcher, off the UI thread. A mod is measured once for its files (mod_fingerprint): opening
// the mod list again, or launching, reads nothing new.
namespace dingosdk::mods {

// One mod's cost, measured now (its `items` are not: they come from the merge). `data_root` is the
// game folder, holding Data and Mods. Never throws: what cannot be read is left out of the figures.
[[nodiscard]] ModCost measure_cost(const Mod& mod, const std::filesystem::path& data_root);

// Mods/.reskate-mod-costs.json brought up to date for `mods`, which should be every installed mod:
// one whose files changed since it was measured, or that never was, is measured, on up to `threads`
// threads beside the caller's (`background`: under normal priority); one no longer installed is
// dropped. Each mod's item count is taken from the last merge's Mods/.reskate/reskate-items.json.
// `stop` set ends it early, keeping what was measured. The file is written when anything changed.
// Returns the costs as they now are. Never throws; one at a time per Mods folder.
Costs refresh_costs(const std::filesystem::path& data_root, const std::vector<const Mod*>& mods, std::size_t threads = 0,
                    bool background = false, const std::atomic<bool>* stop = nullptr) noexcept;

} // namespace dingosdk::mods
