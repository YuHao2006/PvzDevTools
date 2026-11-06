#pragma once
#include <string>
#include <vector>

namespace openpvz::reanim::atlas {
// Batch little-endian layout protocol; pixels never enter the compiler.
int RunPackAtlasLayout(const std::vector<std::string>& args);
}
