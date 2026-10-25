#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// When each mod was last of use, so a player can find the ones they no longer use and switch them
// off: an item of it worn, a map of it loaded, its plugin doing something the player asked for, a
// song of it played, an object of it placed. Kept on this PC only, in Mods/.reskate-mod-usage.json,
// beside the other files the SDK keeps there; nothing of it is ever sent anywhere.
//
// The running game is the only writer: it notes uses as they happen and writes them out now and
// then (UsageLog). The launcher only reads it. Times are seconds since 1970 (UTC), given by the
// caller, so that what is "unused for 30 days" can be tested.
namespace dingosdk::mods {

inline constexpr char usage_file[] = ".reskate-mod-usage.json";

enum class UseKind { item, map, plugin, music, object };
inline constexpr UseKind use_kinds[] = {UseKind::item, UseKind::map, UseKind::plugin, UseKind::music, UseKind::object};
[[nodiscard]] std::string_view use_kind_name(UseKind kind) noexcept;   // "item", "map", ...
[[nodiscard]] std::optional<UseKind> use_kind_named(std::string_view name) noexcept;

struct Use {
    std::int64_t last{};        // the last time
    std::uint64_t count{};      // how many times, all told
    bool operator==(const Use&) const = default;
};

struct ModUsage {
    // When ReSkate first saw the mod enabled. A mod that was never used counts as unused from then,
    // so one just installed is not offered for switching off.
    std::int64_t first_seen{};
    std::map<UseKind, Use> uses;
    bool operator==(const ModUsage&) const = default;
};
using Usage = std::map<std::string, ModUsage, std::less<>>;   // by mod folder name

// The last time the mod was of any use, or nothing when it never was.
[[nodiscard]] std::optional<std::int64_t> last_used(const ModUsage& usage) noexcept;

// Of `mods` (folder names), those of no use for `days` days or more at `now`: last used that long
// ago, or never used and first seen that long ago. A mod `usage` does not know yet was not seen
// before, so it is never in it: tracking that has just begun offers nothing. In `mods`' order.
[[nodiscard]] std::vector<std::string> unused_for(const Usage& usage, const std::vector<std::string>& mods, int days,
                                                  std::int64_t now);
// Whole days since the mod was last of use (or first seen, never used), or nothing when unknown.
[[nodiscard]] std::optional<std::int64_t> idle_days(const Usage& usage, std::string_view mod, std::int64_t now) noexcept;

// What the game notes as it runs. Not thread-safe: the caller holds it on one thread or locks.
class UsageLog {
public:
    UsageLog() = default;
    explicit UsageLog(Usage usage) : usage_(std::move(usage)) {}

    // A mod enabled this run: first_seen is set the first time.
    void seen(std::string_view mod, std::int64_t now);
    // A use of the mod. The same use within `merge_seconds` of the last one counts once (an item
    // worn is looked at often; wearing it all afternoon is one use, not a thousand).
    void used(std::string_view mod, UseKind kind, std::int64_t now);
    static constexpr std::int64_t merge_seconds = 10 * 60;

    [[nodiscard]] const Usage& usage() const noexcept { return usage_; }
    // Whether anything changed since the last mark_written(): worth writing out.
    [[nodiscard]] bool changed() const noexcept { return changed_; }
    void mark_written() noexcept { changed_ = false; }

private:
    Usage usage_;
    bool changed_{};
};

// Empty when missing or unreadable; an entry of the wrong shape is dropped, the others kept.
// Never throws.
[[nodiscard]] Usage read_usage(const std::filesystem::path& file) noexcept;
// Replaces the file (written beside it, then renamed over it). Throws when it cannot be written.
void write_usage(const std::filesystem::path& file, const Usage& usage);

} // namespace dingosdk::mods
