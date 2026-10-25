// Checks what a mod's cost says (Engine/Vfs/mod_cost.h): how one texture counts, how heavy a mod is
// for a card, the order heaviest first, that a cost holds only for the files it was measured on,
// and that Mods/.reskate-mod-costs.json reads back as written and survives a damaged entry.
#include "Engine/Vfs/mod_cost.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace {
int failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

namespace fs = std::filesystem;
using namespace dingosdk::mods;
using dingosdk::frostbite::TextureHeader;

// A square BC-like texture with a full chain of mips, each a quarter of the one before.
TextureHeader texture(std::uint16_t side, std::uint8_t mips, std::uint32_t top_bytes, std::uint16_t flags = 0) {
    TextureHeader header;
    header.width = header.height = side;
    header.mip_count = mips;
    header.flags = flags;
    for (std::uint8_t mip = 0; mip < mips && mip < header.mip_sizes.size(); ++mip)
        header.mip_sizes[mip] = std::max<std::uint32_t>(16, top_bytes >> (2 * mip));
    return header;
}

void write_text(const fs::path& file, const std::string& text) {
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output << text;
}
} // namespace

int main() try {
    // One texture: all its mips count, and what is unusual about it is noted.
    {
        ModCost cost;
        add_texture(cost, texture(4096, 13, 16u << 20, 0x1));
        check(cost.textures == 1, "a texture is counted");
        check(cost.texture_bytes > (16u << 20) && cost.texture_bytes < (22u << 20), "every mip counts, not only the largest");
        check(cost.streamed_textures == 1, "a texture flagged to stream is counted as one");
        check(cost.oversized_textures == 1, "4096 px is over 2048 px");
        check(cost.unmipped_textures == 0, "a texture with mips is not unmipped");

        add_texture(cost, texture(1024, 1, 1u << 20));
        check(cost.textures == 2 && cost.unmipped_textures == 1, "1024 px with one mip is unmipped");
        check(cost.oversized_textures == 1, "1024 px is not oversized");
        check(cost.streamed_textures == 1, "a texture without the flag does not stream");

        add_texture(cost, texture(128, 1, 64u << 10));
        check(cost.unmipped_textures == 1, "a small texture needs no mips");

        add_texture(cost, texture(2048, 12, 4u << 20));
        check(cost.oversized_textures == 1, "exactly 2048 px is not over it");
    }
    // A header with no mip sizes still counts its pixels.
    {
        ModCost cost;
        TextureHeader header;
        header.width = header.height = 512;
        header.mip_count = 10;
        header.chunk_size = 300000;
        add_texture(cost, header);
        check(cost.texture_bytes == 300000, "the chunk's size stands in for missing mip sizes");
    }
    // More mips than the header has room for are not read past it.
    {
        ModCost cost;
        auto header = texture(256, 15, 1u << 16);
        header.mip_count = 40;
        add_texture(cost, header);
        std::uint64_t expected{};
        for (const auto size : header.mip_sizes) expected += size;
        check(cost.texture_bytes == expected, "a mip count past the table stops at its end");
    }

    // How heavy a mod is depends on the card.
    {
        ModCost small, mid, big;
        small.texture_bytes = 20ull << 20;            // 20 MB
        mid.texture_bytes = 60ull << 20;              // 60 MB + 40 MB of meshes = 100 MB
        mid.mesh_bytes = 40ull << 20;
        big.texture_bytes = 500ull << 20;             // 500 MB
        const auto eight = 8ull << 30, twenty_four = 24ull << 30;
        check(video_bytes(mid) == (100ull << 20), "video memory is textures and meshes");
        check(weight(small, eight) == Weight::light, "20 MB is light for an 8 GB card");
        check(weight(mid, eight) == Weight::medium, "100 MB is medium for an 8 GB card");
        check(weight(big, eight) == Weight::heavy, "500 MB is heavy for an 8 GB card");
        check(weight(big, twenty_four) == Weight::medium, "500 MB is only medium for a 24 GB card");
        check(weight(big, 0) == weight(big, eight), "an unknown card is taken as 8 GB");
        check(weight_name(Weight::heavy) == "heavy" && weight_name(Weight::light) == "light", "weights have names");
    }

    // Heaviest first: video memory, then disk, then name.
    {
        Costs costs;
        costs["b-map"].mesh_bytes = 900;
        costs["a-hat"].texture_bytes = 100;
        costs["c-plugin"].disk_bytes = 50;
        costs["d-plugin"].disk_bytes = 50;
        costs["e-deck"].texture_bytes = 100;
        costs["e-deck"].disk_bytes = 10;
        const auto order = heaviest_first(costs);
        check(order == std::vector<std::string>{"b-map", "e-deck", "a-hat", "c-plugin", "d-plugin"},
            "ordered by video memory, then disk, then name");
    }

    // At once: everything that is not a map, and only the heaviest map.
    {
        Costs costs;
        costs["LosSantos"].texture_bytes = 11000;
        costs["Paradise"].texture_bytes = 5000;
        costs["Paradise"].mesh_bytes = 1000;
        costs["Hats"].texture_bytes = 300;
        costs["Deck"].texture_bytes = 200;
        costs["Plugin"].disk_bytes = 10;
        const std::set<std::string, std::less<>> maps{"LosSantos", "Paradise", "Not-Installed-Map"};
        const auto peak = peak_video(costs, maps);
        check(peak.everywhere == 500, "cosmetics and the rest all count");
        check(peak.maps == 17000, "every map, for the record");
        check(peak.heaviest_map == 11000 && peak.map == "LosSantos", "the heaviest map is the one that counts");
        check(peak.at_once() == 11500, "at once: the rest plus one map, not every map");
        const auto none = peak_video(costs, {});
        check(none.at_once() == 17500 && none.map.empty() && none.maps == 0, "with no maps, everything counts");
        Costs empty_map;
        empty_map["Tiny-Map"];
        check(peak_video(empty_map, {"Tiny-Map"}).map == "Tiny-Map", "a map with nothing measured is still named");
    }

    // A cost holds only for the files it was measured on.
    {
        Costs costs;
        costs["mod"].fingerprint = "12-00000000deadbeef";
        check(known_cost(costs, "mod", "12-00000000deadbeef") == &costs["mod"], "the same files: known");
        check(!known_cost(costs, "mod", "13-00000000deadbeef"), "the mod changed: measured again");
        check(!known_cost(costs, "other", "12-00000000deadbeef"), "a mod never measured is not known");
        costs["blank"];
        check(!known_cost(costs, "blank", ""), "an empty fingerprint never matches");
    }

    // The file reads back as written, and a damaged entry costs only itself.
    {
        const auto folder = fs::temp_directory_path() / "dingosdk_mod_cost_tests";
        fs::remove_all(folder);
        fs::create_directories(folder);
        const auto file = folder / costs_file;

        check(read_costs(file).empty(), "no file: no costs");

        Costs costs;
        auto& map = costs["Team-Big_Map"];
        map.fingerprint = "4100-0123456789abcdef";
        map.disk_bytes = 5ull << 30;   // past 32 bits
        map.files = 4100;
        map.mesh_bytes = 1234567890123ull;
        map.read_ms = 8000;
        auto& hat = costs["Someone-Hat"];
        hat.fingerprint = "9-fedcba9876543210";
        add_texture(hat, texture(4096, 13, 16u << 20, 0x1));
        hat.items = 3;
        write_costs(file, costs);
        check(read_costs(file) == costs, "costs read back as written");
        check(!fs::exists(fs::path(file) += ".partial"), "nothing is left beside the file");

        write_costs(file, {});
        check(read_costs(file).empty(), "an empty list replaces the old one");

        write_text(file, R"({"schema":1,"mods":{
            "good": {"fingerprint":"1-a","disk_bytes":10,"textures":2},
            "negative": {"fingerprint":"1-b","disk_bytes":-5},
            "fraction": {"fingerprint":"1-c","texture_bytes":1.5},
            "nameless": {"disk_bytes":5},
            "list": [1, 2]
        }})");
        const auto damaged = read_costs(file);
        check(damaged.size() == 1 && damaged.contains("good"), "only the entry of the right shape is kept");
        check(damaged.at("good").disk_bytes == 10 && damaged.at("good").textures == 2 && damaged.at("good").mesh_bytes == 0,
            "a field it does not name reads as nothing");

        write_text(file, R"({"schema":2,"mods":{"good":{"fingerprint":"1-a"}}})");
        check(read_costs(file).empty(), "a later schema is not guessed at");
        write_text(file, "{ not json");
        check(read_costs(file).empty(), "a broken file is no costs");
        fs::remove_all(folder);
    }

    // The words a player reads.
    {
        check(byte_text(0) == "0 KB", "nothing is 0 KB");
        check(byte_text(10) == "1 KB", "anything is at least 1 KB");
        check(byte_text(820ull << 20) == "820 MB", "megabytes are whole");
        check(byte_text(1536ull << 20) == "1.50 GB", "gigabytes have two places");

        ModCost cost;
        cost.texture_bytes = 1ull << 30;
        cost.mesh_bytes = 200ull << 20;
        cost.textures = 812;
        cost.oversized_textures = 40;
        cost.unmipped_textures = 3;
        cost.disk_bytes = 3ull << 30;
        cost.items = 12;
        check(describe_cost(cost) ==
                  "1.20 GB of video memory (812 textures, 40 over 2048 px, 3 without mips; 200 MB of meshes), 3.00 GB on disk, 12 items",
            "the whole line: " + describe_cost(cost));
        ModCost plugin;
        plugin.disk_bytes = 40ull << 10;
        check(describe_cost(plugin) == "0 KB of video memory, 40 KB on disk", "a plugin's line: " + describe_cost(plugin));
        ModCost one;
        one.textures = 1;
        one.texture_bytes = 2ull << 20;
        one.items = 1;
        check(describe_cost(one) == "2 MB of video memory (1 texture), 0 KB on disk, 1 item", "one of each is singular: " + describe_cost(one));
    }

    if (failures) {
        std::cerr << failures << " mod cost check(s) failed\n";
        return 1;
    }
    std::cout << "mod cost checks passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
