#pragma once

#include <cstdint>

#include "update/digest.hpp"
#include "update/manifest.hpp"

// What a source's release is to the running build, and what GTG may then
// offer. Pure, so every edge can be pinned by a test.
namespace gtg::update {

// The first version with the updater. Nothing older is ever installed, forced
// or not: a machine taken below it could never update again.
inline constexpr Version kFloor{1, 2, 0};

struct Running {
    Version version;
    Digest build{};
    // When the running EXE was linked (its PE time stamp), in UTC ms. Two
    // builds of one version are told apart by it: the source's is newer only
    // when it was published after this one was linked.
    std::int64_t linked_unix_ms{};
};

enum class Compare {
    SameBuild,
    Newer,
    Older,
};

struct Decision {
    Compare compare{Compare::SameBuild};
    bool below_floor{};
};

[[nodiscard]] constexpr Decision Decide(const Running& running, const Manifest& manifest,
                                        const ManifestVariant& variant) noexcept {
    Decision decision;
    decision.below_floor = manifest.version < kFloor;
    if (variant.build == running.build) {
        decision.compare = Compare::SameBuild;
    } else if (manifest.version != running.version) {
        decision.compare = manifest.version > running.version ? Compare::Newer : Compare::Older;
    } else {
        decision.compare =
            manifest.published_unix_ms > running.linked_unix_ms ? Compare::Newer : Compare::Older;
    }
    return decision;
}

// An update is offered only for a newer release; a reinstall, which skips just
// that comparison, for any release this machine's variant has. Neither below
// the floor.
[[nodiscard]] constexpr bool OffersUpdate(const Decision decision) noexcept {
    return decision.compare == Compare::Newer && !decision.below_floor;
}

[[nodiscard]] constexpr bool OffersReinstall(const Decision decision) noexcept {
    return !decision.below_floor;
}

}  // namespace gtg::update
