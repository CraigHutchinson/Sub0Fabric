#include "sub0fabric/pure/version.hpp"

#include <Version.hpp>

namespace sub0fabric {

std::string_view versionString() noexcept { return sub0fabric::version::version_full; }

}  // namespace sub0fabric
