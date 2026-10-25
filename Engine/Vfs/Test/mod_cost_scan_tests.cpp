// Checks bringing Mods/.reskate-mod-costs.json up to date (Engine/Vfs/mod_cost_scan.h) in a made-up
// game folder: sizes on disk, a mod no longer installed dropped, the item counts the last merge
// found, a stop asked for before it starts, and that without the game's layout a mod is given its
// size only and never taken for measured. Measuring textures and meshes needs the game's archives.
#include "Engine/Vfs/mod_cost_scan.h"
#include "Engine/Vfs/mod_item_sources.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
int failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

namespace fs = std::filesystem;
using namespace dingosdk::mods;

void write_bytes(const fs::path& file, std::size_t bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output << std::string(bytes, 'x');
}

Mod mod_at(const fs::path& mods, const std::string& name) {
    Mod mod;
    mod.name = name;
    mod.directory = mods / name;
    return mod;
}
} // namespace

int main() try {
    const auto root = fs::temp_directory_path() / "dingosdk_mod_cost_scan_tests";
    fs::remove_all(root);
    const auto mods = root / mods_folder;
    write_bytes(mods / "Team-Hat" / "manifest.json", 100);
    write_bytes(mods / "Team-Hat" / "Win32" / "items.toc", 3000);
    write_bytes(mods / "Team-Plugin" / "plugin" / "main.lua", 5000);
    fs::create_directories(mods / generated_folder);
    write_item_sources(mods / generated_folder / item_sources_file,
        {{"Team-Hat", "Hat", "Team", "1.0.0", "Team-Hat", {"own_hat_a", "own_hat_b"}}});

    const auto hat = mod_at(mods, "Team-Hat"), plugin = mod_at(mods, "Team-Plugin");
    {
        std::atomic<bool> stop{true};
        const auto none = refresh_costs(root, {&hat, &plugin}, 2, false, &stop);
        check(none.empty(), "a stop asked for before it starts measures nothing");
    }
    auto costs = refresh_costs(root, {&hat, &plugin}, 2);
    check(costs.size() == 2, "every mod asked for is there");
    check(costs.at("Team-Hat").disk_bytes == 3100 && costs.at("Team-Hat").files == 2, "a mod's size is its files'");
    check(costs.at("Team-Plugin").disk_bytes == 5000, "a plugin's too");
    check(costs.at("Team-Hat").items == 2 && costs.at("Team-Plugin").items == 0, "items as the last merge found them");
    check(costs.at("Team-Hat").fingerprint.empty(), "without the game's layout a mod is not taken for measured");
    check(read_costs(mods / costs_file) == costs, "what it found is in the file");

    // The plugin is uninstalled: it goes from the file.
    fs::remove_all(mods / "Team-Plugin");
    costs = refresh_costs(root, {&hat});
    check(costs.size() == 1 && costs.contains("Team-Hat"), "a mod no longer installed is dropped");
    check(read_costs(mods / costs_file).size() == 1, "from the file too");

    // A measurement the file already holds for the same files is kept as it is.
    Costs held;
    auto& kept = held["Team-Hat"];
    kept.fingerprint = mod_fingerprint(hat.directory);
    kept.texture_bytes = 123456;
    write_costs(mods / costs_file, held);
    costs = refresh_costs(root, {&hat});
    check(costs.at("Team-Hat").texture_bytes == 123456, "a mod measured for these files is not read again");
    check(costs.at("Team-Hat").items == 2, "but its items are as the last merge found them");

    write_bytes(mods / "Team-Hat" / "Win32" / "items.toc", 4000);
    costs = refresh_costs(root, {&hat});
    check(costs.at("Team-Hat").texture_bytes == 0 && costs.at("Team-Hat").disk_bytes == 4100, "a mod whose files changed is measured again");

    fs::remove_all(root);
    if (failures) {
        std::cerr << failures << " mod cost scan check(s) failed\n";
        return 1;
    }
    std::cout << "mod cost scan checks passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
