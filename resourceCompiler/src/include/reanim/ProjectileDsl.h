#pragma once

#include <string>
#include <vector>

namespace openpvz::reanim::projectile {

// Offline frontend only: emits source paths, never runtime image symbol IDs.
int RunCompileProjectile(const std::vector<std::string>& args);

}
