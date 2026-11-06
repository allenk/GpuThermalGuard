#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "peer/crypto.hpp"
#include "peer/link.hpp"

// Where pairs are kept: HKCU, through the settings layer, each pair key under
// DPAPI for the current user. Used from the UI thread at start and from the
// link thread when a pair is made or changes; every call opens and closes its
// own registry keys, and DPAPI and CNG may be called from any thread.
namespace gtg::peer {

class PairStore {
public:
    // Loads the cryptography. False if it is unavailable, as for the link.
    [[nodiscard]] bool Load() noexcept { return crypto_.Load(); }

    struct Loaded {
        std::vector<Pair> pairs;   // in the reader's order
        // Pairs whose key could not be unprotected -- another user, another
        // machine, a damaged value. They are removed; their names are here so
        // the reader is told.
        std::vector<std::wstring> dropped;
    };

    [[nodiscard]] Loaded LoadPairs() const;

    // A new pair goes last in the reader's order; a known one keeps its place.
    [[nodiscard]] bool Save(const Pair& pair) const;

    [[nodiscard]] bool Forget(std::uint64_t peer) const;

    [[nodiscard]] bool SaveOrder(const std::vector<std::uint64_t>& order) const;

    // This GTG's id among its pairs: random, made once and kept. Zero if it
    // could not be made or kept.
    [[nodiscard]] std::uint64_t EnsureSelfId() const;

private:
    Crypto crypto_;
};

}  // namespace gtg::peer
