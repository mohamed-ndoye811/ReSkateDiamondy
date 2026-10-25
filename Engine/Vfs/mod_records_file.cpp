#include "mod_records_file.h"

#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace dingosdk::mods::detail {

void replace_file(const std::filesystem::path& file, std::string_view text, std::string_view what) {
    auto partial = file;
    partial += ".partial";
    {
        std::ofstream output(partial, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output.flush()) throw std::runtime_error("The " + std::string(what) + " could not be written to " + partial.string());
    }
    std::error_code error;
    std::filesystem::rename(partial, file, error);
    if (error) {
        std::filesystem::remove(partial, error);
        throw std::runtime_error("The " + std::string(what) + " could not be saved as " + file.string());
    }
}

} // namespace dingosdk::mods::detail
