// The refusal reason: preserved, made safe for one quoted field, and bounded.
#include "startup_failure.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {
int failures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++failures;
    }
}
}  // namespace

int main() {
    using namespace gtg::overlay::integration;

    auto reason = StartupFailureReason("matching system module absent in named game");
    Check(std::string_view(reason.data()) == "matching system module absent in named game",
          "the refusal's own sentence survives");

    reason = StartupFailureReason("first\r\nsecond\tthird");
    Check(std::string_view(reason.data()) == "first  second third",
          "a reason is one line, whatever it arrived as");

    reason = StartupFailureReason("path \"C:\\game\" refused");
    Check(std::string_view(reason.data()) == "path  C: game  refused",
          "nothing in a reason can end its quoted field or escape it");

    reason = StartupFailureReason(std::string(kStartupFailureCapacity * 2, 'x'));
    Check(std::string_view(reason.data()).size() == kStartupFailureCapacity - 1,
          "a long reason is truncated, not refused");
    Check(reason.back() == '\0', "always terminated");

    reason = StartupFailureReason("");
    Check(std::string_view(reason.data()) == "unknown attach failure",
          "a failure with nothing to say still names itself");

    if (failures == 0) std::puts("PASS startup failure reason");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
