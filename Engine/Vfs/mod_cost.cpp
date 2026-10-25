#include "mod_cost.h"
#include "Engine/Core/Json/json.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

namespace dingosdk::mods {
namespace {
namespace fs = std::filesystem;

// The flag the game's own streamed textures carry (as mod_merge_textures.cpp reads it).
constexpr std::uint16_t texture_streams = 0x1;
constexpr int costs_schema = 1;

// The fields of an entry, in the order they are written; every one an unsigned number.
struct Field {
    const char* name;
    std::uint64_t ModCost::*member;
};
constexpr Field fields[] = {
    {"disk_bytes", &ModCost::disk_bytes},
    {"files", &ModCost::files},
    {"textures", &ModCost::textures},
    {"texture_bytes", &ModCost::texture_bytes},
    {"streamed_textures", &ModCost::streamed_textures},
    {"oversized_textures", &ModCost::oversized_textures},
    {"unmipped_textures", &ModCost::unmipped_textures},
    {"mesh_bytes", &ModCost::mesh_bytes},
    {"items", &ModCost::items},
    {"read_ms", &ModCost::read_ms},
};

// A number field as it was written. A negative or fractional number is a file someone else wrote.
std::uint64_t count_of(const Json& entry, const char* name) {
    if (!entry.contains(name)) return 0;   // a field added later reads as nothing in an older file
    const auto& value = entry.at(name);
    if (!value.is_number_unsigned()) throw std::runtime_error(std::string("not a count: ") + name);
    return value.get<std::uint64_t>();
}
} // namespace

void add_texture(ModCost& cost, const frostbite::TextureHeader& texture) noexcept {
    std::uint64_t bytes{};
    const auto mips = std::min<std::size_t>(texture.mip_count, texture.mip_sizes.size());
    for (std::size_t mip = 0; mip < mips; ++mip) bytes += texture.mip_sizes[mip];
    // A header that gives no mip sizes still says how large its pixels are.
    if (!bytes) bytes = texture.chunk_size;
    ++cost.textures;
    cost.texture_bytes += bytes;
    if (texture.flags & texture_streams) ++cost.streamed_textures;
    const auto side = std::max(texture.width, texture.height);
    if (side > oversized_side) ++cost.oversized_textures;
    if (texture.mip_count <= 1 && side >= unmipped_side) ++cost.unmipped_textures;
}

std::uint64_t video_bytes(const ModCost& cost) noexcept { return cost.texture_bytes + cost.mesh_bytes; }

Weight weight(const ModCost& cost, std::uint64_t card_memory) noexcept {
    if (!card_memory) card_memory = 8ull << 30;
    const auto bytes = video_bytes(cost);
    if (bytes >= card_memory / 20) return Weight::heavy;
    if (bytes >= card_memory / 100) return Weight::medium;
    return Weight::light;
}

std::string_view weight_name(Weight weight) noexcept {
    switch (weight) {
    case Weight::heavy: return "heavy";
    case Weight::medium: return "medium";
    case Weight::light: break;
    }
    return "light";
}

std::vector<std::string> heaviest_first(const Costs& costs) {
    std::vector<const Costs::value_type*> order;
    order.reserve(costs.size());
    for (const auto& entry : costs) order.push_back(&entry);
    std::ranges::sort(order, [](const auto* a, const auto* b) {
        const auto videoA = video_bytes(a->second), videoB = video_bytes(b->second);
        if (videoA != videoB) return videoA > videoB;
        if (a->second.disk_bytes != b->second.disk_bytes) return a->second.disk_bytes > b->second.disk_bytes;
        return a->first < b->first;
    });
    std::vector<std::string> names;
    names.reserve(order.size());
    for (const auto* entry : order) names.push_back(entry->first);
    return names;
}

const ModCost* known_cost(const Costs& costs, std::string_view mod, std::string_view fingerprint) noexcept {
    const auto found = costs.find(mod);
    if (found == costs.end() || fingerprint.empty() || found->second.fingerprint != fingerprint) return nullptr;
    return &found->second;
}

Costs read_costs(const fs::path& file) noexcept {
    Costs costs;
    try {
        std::ifstream input(file, std::ios::binary);
        if (!input) return costs;
        const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        const auto root = Json::parse(text);
        if (!root.is_object() || root.value("schema", 0) != costs_schema || !root.contains("mods") ||
            !root.at("mods").is_object())
            return costs;
        for (const auto& [name, entry] : root.at("mods").items()) {
            try {
                if (!entry.is_object() || !entry.contains("fingerprint") || !entry.at("fingerprint").is_string()) continue;
                ModCost cost;
                cost.fingerprint = entry.at("fingerprint").string();
                for (const auto& field : fields) cost.*field.member = count_of(entry, field.name);
                costs.emplace(name, std::move(cost));
            } catch (const std::exception&) {
                // This mod is measured again; the others stand.
            }
        }
    } catch (...) {
        costs.clear();
    }
    return costs;
}

void write_costs(const fs::path& file, const Costs& costs) {
    auto mods = Json::object();
    for (const auto& [name, cost] : costs) {
        auto entry = Json::object();
        entry["fingerprint"] = cost.fingerprint;
        for (const auto& field : fields) entry[field.name] = cost.*field.member;
        mods[name] = std::move(entry);
    }
    auto root = Json::object();
    root["schema"] = costs_schema;
    root["mods"] = std::move(mods);
    const auto text = root.dump(2);

    // Written whole beside it first, so a launch that stops halfway leaves the last good file.
    auto partial = file;
    partial += ".partial";
    {
        std::ofstream output(partial, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output.flush()) throw std::runtime_error("The mod costs could not be written to " + partial.string());
    }
    std::error_code error;
    fs::rename(partial, file, error);
    if (error) {
        fs::remove(partial, error);
        throw std::runtime_error("The mod costs could not be saved as " + file.string());
    }
}

std::string byte_text(std::uint64_t bytes) {
    if (!bytes) return "0 KB";
    if (bytes >= 1024ull * 1024 * 1024) return std::format("{:.2f} GB", static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
    if (bytes >= 1024ull * 1024) return std::format("{:.0f} MB", static_cast<double>(bytes) / (1024.0 * 1024));
    return std::format("{} KB", std::max<std::uint64_t>(1, bytes / 1024));
}

std::string describe_cost(const ModCost& cost) {
    std::string text = byte_text(video_bytes(cost)) + " of video memory";
    std::vector<std::string> parts;
    if (cost.textures) {
        auto textures = std::format("{} texture{}", cost.textures, cost.textures == 1 ? "" : "s");
        if (cost.oversized_textures) textures += std::format(", {} over {} px", cost.oversized_textures, oversized_side);
        if (cost.unmipped_textures) textures += std::format(", {} without mips", cost.unmipped_textures);
        parts.push_back(std::move(textures));
    }
    if (cost.mesh_bytes) parts.push_back(byte_text(cost.mesh_bytes) + " of meshes");
    if (!parts.empty()) {
        text += " (";
        for (std::size_t at = 0; at < parts.size(); ++at) text += (at ? "; " : "") + parts[at];
        text += ')';
    }
    text += ", " + byte_text(cost.disk_bytes) + " on disk";
    if (cost.items) text += std::format(", {} item{}", cost.items, cost.items == 1 ? "" : "s");
    return text;
}

} // namespace dingosdk::mods
