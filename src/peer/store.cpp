#include "peer/store.hpp"

#include <algorithm>
#include <cstring>

#include "logging/logger.hpp"
#include "settings/settings.hpp"

namespace gtg::peer {

PairStore::Loaded PairStore::LoadPairs() const {
    Loaded loaded;
    for (const settings::StoredHost& stored : settings::LoadHosts()) {
        if (loaded.pairs.size() >= kMaxPairs) break;
        const auto key = crypto_.Unprotect(stored.protected_key);
        if (!key || key->size() != kKeyBytes) {
            loaded.dropped.push_back(stored.name);
            std::wstring error;
            if (!settings::RemoveHost(stored.peer, error)) logging::Warning(error);
            continue;
        }
        Pair pair;
        pair.peer = stored.peer;
        pair.name = stored.name;
        std::memcpy(pair.key.data(), key->data(), kKeyBytes);
        pair.issued = stored.issued;
        if (stored.has_endpoint) pair.endpoint = Endpoint{stored.address, stored.port};
        loaded.pairs.push_back(std::move(pair));
    }
    return loaded;
}

bool PairStore::Save(const Pair& pair) const {
    const auto blob = crypto_.Protect(pair.key);
    if (!blob) return false;
    settings::StoredHost stored;
    stored.peer = pair.peer;
    stored.name = pair.name;
    stored.issued = pair.issued;
    if (pair.endpoint) {
        stored.has_endpoint = true;
        stored.address = pair.endpoint->address;
        stored.port = pair.endpoint->port;
    }
    stored.protected_key = *blob;
    std::wstring error;
    if (!settings::SaveHost(stored, error)) {
        (void)logging::TryWarning(error);
        return false;
    }
    return true;
}

bool PairStore::Forget(const std::uint64_t peer) const {
    std::wstring error;
    if (settings::RemoveHost(peer, error)) return true;
    (void)logging::TryWarning(error);
    return false;
}

bool PairStore::SaveOrder(const std::vector<std::uint64_t>& order) const {
    std::wstring error;
    if (settings::SaveHostsOrder(order, error)) return true;
    logging::Warning(error);
    return false;
}

std::uint64_t PairStore::EnsureSelfId() const {
    const auto preference = settings::LoadHostsPreference();
    if (preference.peer_id != 0) return preference.peer_id;
    std::uint64_t id = 0;
    while (id == 0) {
        if (!crypto_.Random({reinterpret_cast<std::uint8_t*>(&id), sizeof(id)})) return 0;
    }
    std::wstring error;
    if (!settings::SaveHostsPeerId(id, error)) {
        logging::Warning(error);
        return 0;
    }
    return id;
}

}  // namespace gtg::peer
