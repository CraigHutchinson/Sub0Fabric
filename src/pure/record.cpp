#include "sub0fabric/pure/record.hpp"

namespace sub0fabric {

KindRef splitKind(std::string_view kind) noexcept
{
    const std::size_t at = kind.find('@');
    if (at == std::string_view::npos) return {kind, {}};
    return {kind.substr(0, at), kind.substr(at + 1)};
}

}  // namespace sub0fabric
