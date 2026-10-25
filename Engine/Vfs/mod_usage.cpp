#include "mod_usage.h"
#include "mod_records_file.h"
#include "Engine/Core/Json/json.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace dingosdk::mods {
namespace {
constexpr int usage_schema = 1;
constexpr std::int64_t day_seconds = 24 * 60 * 60;

std::int64_t time_of(const Json& entry, std::string_view name) {
    if (!entry.contains(name)) return 0;
    const auto& value = entry.at(name);
    if (!value.is_number_integer()) throw std::runtime_error("not a time: " + std::string(name));
    return value.get<std::int64_t>();
}

// What a mod's idle time is counted from: its last use, else when it was first seen; 0 unknown.
std::int64_t idle_from(const ModUsage& usage) noexcept {
    if (const auto last = last_used(usage)) return *last;
    return usage.first_seen;
}
} // namespace

std::string_view use_kind_name(UseKind kind) noexcept {
    switch (kind) {
    case UseKind::item: return "item";
    case UseKind::map: return "map";
    case UseKind::plugin: return "plugin";
    case UseKind::music: return "music";
    case UseKind::object: return "object";
    }
    return "item";
}

std::optional<UseKind> use_kind_named(std::string_view name) noexcept {
    for (const auto kind : use_kinds)
        if (use_kind_name(kind) == name) return kind;
    return std::nullopt;
}

std::optional<std::int64_t> last_used(const ModUsage& usage) noexcept {
    std::optional<std::int64_t> last;
    for (const auto& [kind, use] : usage.uses)
        if (use.count && (!last || use.last > *last)) last = use.last;
    return last;
}

std::vector<std::string> unused_for(const Usage& usage, const std::vector<std::string>& mods, int days, std::int64_t now) {
    const auto span = std::int64_t{std::max(days, 0)} * day_seconds;
    std::vector<std::string> unused;
    for (const auto& mod : mods) {
        const auto found = usage.find(mod);
        if (found == usage.end()) continue;
        const auto from = idle_from(found->second);
        if (from && now - from >= span) unused.push_back(mod);
    }
    return unused;
}

std::optional<std::int64_t> idle_days(const Usage& usage, std::string_view mod, std::int64_t now) noexcept {
    const auto found = usage.find(mod);
    if (found == usage.end()) return std::nullopt;
    const auto from = idle_from(found->second);
    if (!from) return std::nullopt;
    return std::max<std::int64_t>(0, now - from) / day_seconds;
}

void UsageLog::seen(std::string_view mod, std::int64_t now) {
    auto& entry = usage_[std::string(mod)];
    if (entry.first_seen) return;
    entry.first_seen = now;
    changed_ = true;
}

void UsageLog::used(std::string_view mod, UseKind kind, std::int64_t now) {
    seen(mod, now);
    auto& use = usage_[std::string(mod)].uses[kind];
    // A clock put back far is a use, but not one in the future: the later time stands.
    const auto apart = now > use.last ? now - use.last : use.last - now;
    if (use.count && apart < merge_seconds) {
        if (now > use.last) {
            use.last = now;
            changed_ = true;
        }
        return;
    }
    use.last = std::max(use.last, now);
    ++use.count;
    changed_ = true;
}

Usage read_usage(const std::filesystem::path& file) noexcept {
    Usage usage;
    try {
        std::ifstream input(file, std::ios::binary);
        if (!input) return usage;
        const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        const auto root = Json::parse(text);
        if (!root.is_object() || root.value("schema", 0) != usage_schema || !root.contains("mods") ||
            !root.at("mods").is_object())
            return usage;
        for (const auto& [name, entry] : root.at("mods").items()) {
            try {
                if (!entry.is_object()) continue;
                ModUsage mod;
                mod.first_seen = time_of(entry, "first_seen");
                for (const auto kind : use_kinds) {
                    const auto key = use_kind_name(kind);
                    if (!entry.contains(key)) continue;
                    const auto& use = entry.at(key);
                    if (!use.is_object() || !use.contains("count") || !use.at("count").is_number_unsigned()) continue;
                    mod.uses[kind] = Use{time_of(use, "last"), use.at("count").get<std::uint64_t>()};
                }
                usage.emplace(name, std::move(mod));
            } catch (const std::exception&) {
                // This mod starts again from nothing; the others stand.
            }
        }
    } catch (...) {
        usage.clear();
    }
    return usage;
}

void write_usage(const std::filesystem::path& file, const Usage& usage) {
    auto mods = Json::object();
    for (const auto& [name, mod] : usage) {
        auto entry = Json::object();
        entry["first_seen"] = mod.first_seen;
        for (const auto& [kind, use] : mod.uses) {
            auto record = Json::object();
            record["last"] = use.last;
            record["count"] = use.count;
            entry[use_kind_name(kind)] = std::move(record);
        }
        mods[name] = std::move(entry);
    }
    auto root = Json::object();
    root["schema"] = usage_schema;
    root["mods"] = std::move(mods);
    detail::replace_file(file, root.dump(2), "mod usage");
}

} // namespace dingosdk::mods
