#pragma once

#include "Engine/Resource/texture.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// What each mod costs the game, so a player can see which mods are heavy: what it takes on disk
// and, above all, what its textures and meshes can take in video memory. The merge measures it
// from the mod's own bundles and keeps it in Mods/.reskate-mod-costs.json while the mod's files
// are as they were (mod_fingerprint, mod_list.h). The file is beside .reskate-excluded.json, not in
// the generated folder a full merge deletes, so an unchanged mod is never read again for it. The
// launcher sorts and filters its list by it; the `mods cost` console command prints it.
//
// It is what a mod *can* take, not a reading: what is in video memory at once depends on the map
// and on what is worn, and the game streams the largest mips of many textures. At run time the
// game's memory cannot be told apart by mod (every mod is in one merged patch), so this is the
// figure to compare mods by.
namespace dingosdk::mods {

inline constexpr char costs_file[] = ".reskate-mod-costs.json";

// A texture wider or taller than this is more than a cosmetic needs on screen.
inline constexpr std::uint16_t oversized_side = 2048;
// A texture this large or larger on a side with a single mip is drawn far away at full size.
inline constexpr std::uint16_t unmipped_side = 256;

struct ModCost {
    std::string fingerprint;             // mod_fingerprint() of the files measured
    std::uint64_t disk_bytes{}, files{};
    std::uint64_t textures{};            // texture resources the mod ships itself (not the game's)
    std::uint64_t texture_bytes{};       // all their mips: their video memory once loaded in full
    std::uint64_t streamed_textures{};   // of them, those that stream: each holds one of the game's
                                         // texture handles while loaded (Extension/World/texture_handle_limit.h)
    std::uint64_t oversized_textures{};  // wider or taller than oversized_side
    std::uint64_t unmipped_textures{};   // unmipped_side or more with one mip
    std::uint64_t mesh_bytes{};          // every level of detail of the meshes it ships
    std::uint64_t items{};               // cosmetic items it adds or changes (mod_item_sources.h)
    std::uint64_t read_ms{};             // how long measuring it took: a hint of what it adds to a merge
    bool operator==(const ModCost&) const = default;
};
using Costs = std::map<std::string, ModCost, std::less<>>;   // by mod folder name

// Counts one texture the mod ships.
void add_texture(ModCost& cost, const frostbite::TextureHeader& texture) noexcept;
// The video memory the mod can take: its textures and its meshes.
[[nodiscard]] std::uint64_t video_bytes(const ModCost& cost) noexcept;

// How heavy a mod is for a card with `card_memory` bytes of its own: heavy from 5% of it, medium
// from 1%, light below. 0 (not known) is taken as 8 GB, the cards the 2.1.0 notes found short.
enum class Weight { light, medium, heavy };
[[nodiscard]] Weight weight(const ModCost& cost, std::uint64_t card_memory) noexcept;
[[nodiscard]] std::string_view weight_name(Weight weight) noexcept;   // "light", "medium", "heavy"

// The mods in `costs`, heaviest first: by video_bytes, then disk_bytes, then name.
[[nodiscard]] std::vector<std::string> heaviest_first(const Costs& costs);

// The cost measured for `mod` with these files, or null when it was measured for other files or
// never: the merge measures it again then.
[[nodiscard]] const ModCost* known_cost(const Costs& costs, std::string_view mod, std::string_view fingerprint) noexcept;

// Mods/.reskate-mod-costs.json. Empty when the file is missing or unreadable; an entry of the
// wrong shape is dropped, the others kept. Never throws.
[[nodiscard]] Costs read_costs(const std::filesystem::path& file) noexcept;
// Replaces the file (written beside it, then renamed over it). Throws when it cannot be written.
void write_costs(const std::filesystem::path& file, const Costs& costs);

// "1.40 GB", "820 MB", "12 KB": as the launcher shows sizes (gui_mods_browse.cpp), in 1024s; at
// least "1 KB" for anything, "0 KB" for nothing.
[[nodiscard]] std::string byte_text(std::uint64_t bytes);
// One line for the console and the log:
// "1.40 GB of video memory (812 textures, 40 over 2048 px, 3 without mips; 200 MB of meshes), 3.20 GB on disk, 12 items".
[[nodiscard]] std::string describe_cost(const ModCost& cost);

} // namespace dingosdk::mods
