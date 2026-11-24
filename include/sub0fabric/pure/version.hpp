#pragma once
/// @file version.hpp
/// The library's version, from `git describe` at configure time (Version.cmake).

#include <string_view>

namespace sub0fabric {

/// The full version: `<tag>` on a tag, `<tag>-<n>-g<sha>` after it, `-dirty` when the tree has changes; `0.0.0...` before the first v<major>.<minor> tag.
[[nodiscard]] std::string_view versionString() noexcept;

}  // namespace sub0fabric
