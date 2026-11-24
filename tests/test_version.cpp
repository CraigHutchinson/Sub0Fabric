#include "sub0fabric/pure/version.hpp"
#include "sub0platform/testing/check.hpp"

int main()
{
    CHECK(!sub0fabric::versionString().empty());
    return finish();
}
