#pragma once
#include <filesystem>

namespace runner::runtime {
// Guest-only convenience link. The target remains the existing session Output;
// this creates no Host mapping and no second results directory.
std::filesystem::path create_output_shortcut(const std::filesystem::path& output);
}
