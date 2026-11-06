#include "peer/link.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <utility>

#include "logging/logger.hpp"
#include "peer/keys.hpp"
#include "peer/replay.hpp"

namespace gtg::peer {
namespace {

using link_detail::Command;

// A burst of datagrams is read this far before the thread looks at its other
// events; Winsock signals again if more are waiting.
constexpr int kReceiveBurst = 128;

template <typename T>
T Resolve(const HMODULE module, const char* name) noexcept {
    return module == nullptr ? nullptr : reinterpret_cast<T>(GetProcAddress(module, name));
}

// Winsock, from System32 at run time: nothing here may call a Winsock
// function directly, or the EXE would import ws2_32.dll. That includes
// htons and friends, hence the byte swaps below.
struct Winsock {
    HMODULE module{};
    decltype(&WSAStartup) startup{};
    decltype(&WSACleanup) cleanup{};
    decltype(&::socket) open{};
    decltype(&::bind) bind{};
    decltype(&::connect) connect{};
    decltype(&::closesocket) close{};
    decltype(&::sendto) send_to{};
    decltype(&::recvfrom) receive_from{};
    decltype(&::getsockname) local_name{};
    decltype(&::setsockopt) set_option{};
    decltype(&WSAGetLastError) last_error{};
    decltype(&WSAEventSelect) event_select{};
    decltype(&WSAEnumNetworkEvents) network_events{};
    decltype(&WSAIoctl) ioctl{};
    decltype(&::gethostname) host_name{};
    decltype(&::getaddrinfo) address_info{};
    decltype(&::freeaddrinfo) free_address_info{};

    bool Load() noexcept {
        module = LoadLibraryExW(L"ws2_32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        startup = Resolve<decltype(startup)>(module, "WSAStartup");
        cleanup = Resolve<decltype(cleanup)>(module, "WSACleanup");
        open = Resolve<decltype(open)>(module, "socket");
        bind = Resolve<decltype(bind)>(module, "bind");
        connect = Resolve<decltype(connect)>(module, "connect");
        close = Resolve<decltype(close)>(module, "closesocket");
        send_to = Resolve<decltype(send_to)>(module, "sendto");
        receive_from = Resolve<decltype(receive_from)>(module, "recvfrom");
        local_name = Resolve<decltype(local_name)>(module, "getsockname");
        set_option = Resolve<decltype(set_option)>(module, "setsockopt");
        last_error = Resolve<decltype(last_error)>(module, "WSAGetLastError");
        event_select = Resolve<decltype(event_select)>(module, "WSAEventSelect");
        network_events = Resolve<decltype(network_events)>(module, "WSAEnumNetworkEvents");
        ioctl = Resolve<decltype(ioctl)>(module, "WSAIoctl");
        host_name = Resolve<decltype(host_name)>(module, "gethostname");
        address_info = Resolve<decltype(address_info)>(module, "getaddrinfo");
        free_address_info = Resolve<decltype(free_address_info)>(module, "freeaddrinfo");
        return startup && cleanup && open && bind && connect && close && send_to && receive_from &&
               local_name && set_option && last_error && event_select && network_events && ioctl &&
               host_name && address_info && free_address_info;
    }

    void Unload() noexcept {
        if (module != nullptr) FreeLibrary(module);
        module = nullptr;
    }
};

std::uint16_t Swap(const std::uint16_t value) noexcept {
    return static_cast<std::uint16_t>((value >> 8) | (value << 8));
}

sockaddr_in ToSockaddr(const Endpoint& endpoint) noexcept {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = Swap(endpoint.port);
    std::memcpy(&address.sin_addr, endpoint.address.data(), 4);
    return address;
}

Endpoint FromSockaddr(const sockaddr_in& address) noexcept {
    Endpoint endpoint;
    std::memcpy(endpoint.address.data(), &address.sin_addr, 4);
    endpoint.port = Swap(address.sin_port);
    return endpoint;
}

std::uint64_t Now() noexcept {
    return GetTickCount64();
}

// The wall clock, Unix milliseconds: what a session's age is judged by.
std::int64_t WallMs() noexcept {
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    const std::uint64_t ticks =
        (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    return static_cast<std::int64_t>(ticks / 10'000) - 11'644'473'600'000LL;
}

bool Private(const std::array<std::uint8_t, 4>& a) noexcept {
    return a[0] == 10 || (a[0] == 172 && a[1] >= 16 && a[1] <= 31) || (a[0] == 192 && a[1] == 168);
}

// The address other machines reach this one at: the one on the interface of
// the default route. Connecting a UDP socket sends nothing; it only picks the
// route. Without a default route (an isolated LAN), the first private address
// of this machine's name.
std::optional<std::array<std::uint8_t, 4>> LocalAddress(const Winsock& ws) noexcept {
    const SOCKET probe = ws.open(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe != INVALID_SOCKET) {
        const sockaddr_in remote = ToSockaddr({{8, 8, 8, 8}, 53});
        sockaddr_in local{};
        int size = sizeof(local);
        const bool routed =
            ws.connect(probe, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote)) == 0 &&
            ws.local_name(probe, reinterpret_cast<sockaddr*>(&local), &size) == 0;
        ws.close(probe);
        if (routed) {
            const Endpoint found = FromSockaddr(local);
            if (found.address[0] != 0) return found.address;
        }
    }
    char name[256]{};
    if (ws.host_name(name, sizeof(name)) != 0) return std::nullopt;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* list = nullptr;
    if (ws.address_info(name, nullptr, &hints, &list) != 0) return std::nullopt;
    std::optional<std::array<std::uint8_t, 4>> chosen;
    for (const addrinfo* item = list; item != nullptr; item = item->ai_next) {
        if (item->ai_family != AF_INET || item->ai_addrlen < sizeof(sockaddr_in)) continue;
        const Endpoint found = FromSockaddr(*reinterpret_cast<const sockaddr_in*>(item->ai_addr));
        if (found.address[0] == 127 || found.address[0] == 0) continue;
        if (!chosen || (Private(found.address) && !Private(*chosen))) chosen = found.address;
    }
    ws.free_address_info(list);
    return chosen;
}

std::wstring Describe(const Endpoint& endpoint) {
    return std::format(L"{}.{}.{}.{}:{}", endpoint.address[0], endpoint.address[1],
                       endpoint.address[2], endpoint.address[3], endpoint.port);
}

}  // namespace

struct Link::State {
    struct Peer {
        Pair pair;
        Heard heard;
        std::optional<HostState> state;
        ReplayGuard guard;
        std::optional<Key> receive_key;   // for the session the guard accepted
        std::optional<Key> send_key;      // for this machine's session
        std::optional<std::int64_t> clock_skew_ms;
#ifdef GTG_FEATURE_UPDATE
        std::optional<update::hosts::StatusBody> update_status;
        std::optional<update::hosts::TriggerBody> update_trigger;
        std::uint64_t update_trigger_ms{};
        // A trigger the reader asked for, and the rounds it still goes out with.
        std::optional<update::hosts::TriggerBody> trigger_out;
        int trigger_rounds{};
#endif
    };

    explicit State(Link& owner) : link(owner) {}

    State(const State&) = delete;
    State& operator=(const State&) = delete;

    ~State() {
        CloseSocket();
        if (socket_event != nullptr) CloseHandle(socket_event);
        if (winsock_started) ws.cleanup();
        ws.Unload();
    }

    Link& link;
    Crypto crypto;
    Winsock ws;
    bool winsock_started{};
    SOCKET socket{INVALID_SOCKET};
    HANDLE socket_event{};
    bool listening{};
    std::uint16_t bound_port{};
    // After the fixed port turned out to be taken, when to try it again.
    std::uint64_t retry_fixed_ms{};
    LinkStatus status{LinkStatus::Starting};
    Self self;
    std::vector<Peer> peers;

    std::optional<Invitation> invitation;
    std::string invitation_code;
    bool no_address{};
    std::optional<Joining> joining;
    std::optional<JoinView> join_view;
    std::uint64_t join_started_ms{};
    std::optional<std::uint64_t> join_sent_ms;

    std::array<std::uint8_t, kSaltBytes> session{};
    std::uint64_t sequence{};
    bool suspended{};
    bool muted{};
    Admission admission;
    std::optional<std::uint64_t> last_send_ms;
    std::optional<HostState> local;
#ifdef GTG_FEATURE_UPDATE
    std::optional<update::hosts::StatusBody> update_local;
#endif

    Peer* Find(const std::uint64_t peer) noexcept {
        for (Peer& candidate : peers) {
            if (candidate.pair.peer == peer) return &candidate;
        }
        return nullptr;
    }

    void Save(const Pair& pair) const {
        if (link.options_.save) link.options_.save(pair);
    }

    [[nodiscard]] bool WantsFixed(const std::uint64_t now) const noexcept {
        if (invitation && invitation->StatusAt(now) == Invitation::Status::Live) return true;
        return std::any_of(peers.begin(), peers.end(),
                           [](const Peer& peer) { return peer.pair.issued; });
    }

    void CloseSocket() noexcept {
        if (socket == INVALID_SOCKET) return;
        (void)ws.event_select(socket, socket_event, 0);
        ws.close(socket);
        socket = INVALID_SOCKET;
        bound_port = 0;
        listening = false;
    }

    // `fallback`: an ephemeral port because the fixed one is taken, which the
    // status keeps saying until the fixed port binds or is no longer wanted.
    void Bind(const bool fixed, const std::uint64_t now, const bool fallback = false) {
        CloseSocket();
        socket = ws.open(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket == INVALID_SOCKET) {
            status = LinkStatus::NetworkMissing;
            return;
        }
        // A datagram to a host that is not listening comes back as an ICMP
        // error, which Windows otherwise reports on the next receive.
        BOOL report = FALSE;
        DWORD returned = 0;
        (void)ws.ioctl(socket, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned,
                       nullptr, nullptr);
        if (fixed) {
            // No other program may bind the same port and read what is sent here.
            BOOL exclusive = TRUE;
            (void)ws.set_option(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                                reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        }
        const auto address = link.options_.bind_address.value_or(std::array<std::uint8_t, 4>{});
        const sockaddr_in local_address =
            ToSockaddr({address, fixed ? link.options_.port : static_cast<std::uint16_t>(0)});
        if (ws.bind(socket, reinterpret_cast<const sockaddr*>(&local_address),
                    sizeof(local_address)) != 0) {
            const int error = ws.last_error();
            ws.close(socket);
            socket = INVALID_SOCKET;
            if (fixed) {
                (void)logging::TryWarning(std::format(
                    L"hosts: port {} could not be bound (error {}); sending from another port",
                    link.options_.port, error));
                status = LinkStatus::PortInUse;
                retry_fixed_ms = now + 30'000;
                Bind(false, now, true);
                return;
            }
            status = LinkStatus::NetworkMissing;
            return;
        }
        (void)ws.event_select(socket, socket_event, FD_READ);
        sockaddr_in bound{};
        int size = sizeof(bound);
        if (ws.local_name(socket, reinterpret_cast<sockaddr*>(&bound), &size) == 0) {
            bound_port = FromSockaddr(bound).port;
        }
        listening = fixed;
        if (!fallback) status = LinkStatus::Running;
    }

    void SendTo(const Endpoint& endpoint, const std::span<const std::uint8_t> datagram) const {
        if (socket == INVALID_SOCKET) return;
        const sockaddr_in to = ToSockaddr(endpoint);
        (void)ws.send_to(socket, reinterpret_cast<const char*>(datagram.data()),
                         static_cast<int>(datagram.size()), 0,
                         reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }

    void NewSession() {
        if (!crypto.Random(session)) session.fill(0);
        sequence = 0;
        for (Peer& peer : peers)
            peer.send_key.reset();
    }

    [[nodiscard]] std::optional<Key> SendKey(Peer& peer) {
        if (!peer.send_key) {
            peer.send_key = DirectionKey(crypto, peer.pair.key, session, self.id, peer.pair.peer);
        }
        return peer.send_key;
    }

    void SendStates(const std::uint64_t now) {
        last_send_ms = now;
        {
            std::lock_guard lock(link.mailbox_mutex_);
            if (link.mailbox_) local = link.mailbox_;
#ifdef GTG_FEATURE_UPDATE
            if (link.update_mailbox_) update_local = link.update_mailbox_;
#endif
        }
        if (suspended || muted || !local) return;
        HostState out = *local;
        out.name = self.name;
        for (Peer& peer : peers) {
            if (!peer.pair.endpoint) continue;
            const auto key = SendKey(peer);
            if (!key) continue;
            const SealedHeader header{Type::State, self.id, session, ++sequence, WallMs()};
            if (const auto datagram = SealState(crypto, *key, header, out)) {
                SendTo(*peer.pair.endpoint, *datagram);
            }
#ifdef GTG_FEATURE_UPDATE
            SendUpdate(peer, *key);
#endif
        }
    }

#ifdef GTG_FEATURE_UPDATE
    // After each state: this machine's update status, and a trigger the reader
    // asked for, in its first rounds.
    void SendUpdate(Peer& peer, const Key& key) {
        if (update_local) {
            const SealedHeader header{Type::UpdateStatus, self.id, session, ++sequence, WallMs()};
            if (const auto datagram =
                    SealUpdate(crypto, key, header, Type::UpdateStatus, *update_local)) {
                SendTo(*peer.pair.endpoint, *datagram);
            }
        }
        if (peer.trigger_out && peer.trigger_rounds > 0) {
            --peer.trigger_rounds;
            const SealedHeader header{Type::UpdateTrigger, self.id, session, ++sequence, WallMs()};
            if (const auto datagram =
                    SealUpdate(crypto, key, header, Type::UpdateTrigger, *peer.trigger_out)) {
                SendTo(*peer.pair.endpoint, *datagram);
            }
            if (peer.trigger_rounds == 0) peer.trigger_out.reset();
        }
    }
#endif

    void SendGoodbye(Peer& peer) {
        if (!peer.pair.endpoint) return;
        const auto key = SendKey(peer);
        if (!key) return;
        const SealedHeader header{Type::Goodbye, self.id, session, ++sequence, WallMs()};
        if (const auto datagram = SealGoodbye(crypto, *key, header)) {
            SendTo(*peer.pair.endpoint, *datagram);
        }
    }

    void SendGoodbyes() {
        for (Peer& peer : peers)
            SendGoodbye(peer);
    }

    void AddPair(const NewPair& paired, const bool issued, const Endpoint& endpoint) {
        Peer* peer = Find(paired.peer);
        if (peer == nullptr) {
            if (peers.size() >= kMaxPairs) return;
            peers.emplace_back();
            peer = &peers.back();
        } else {
            // Paired again: the old key and everything heard under it go.
            *peer = {};
        }
        peer->pair = {paired.peer, paired.name, paired.key, issued, endpoint};
        Save(peer->pair);
        (void)logging::TryInfo(std::format(L"hosts: paired with \"{}\" at {} ({})", paired.name,
                                           Describe(endpoint), issued ? L"our code" : L"its code"));
    }

    void OnPairRequest(const std::span<const std::uint8_t> datagram, const Endpoint& source,
                       const std::uint64_t now) {
        if (!invitation) return;
        const auto request = ReadPairRequest(datagram);
        if (!request) return;
        if (peers.size() >= kMaxPairs && Find(request->joiner) == nullptr) return;
        const auto answer = invitation->Handle(crypto, self, *request, datagram, now);
        if (answer.reply) SendTo(source, *answer.reply);
        if (answer.paired) AddPair(*answer.paired, true, source);
    }

    void OnPairAccept(const std::span<const std::uint8_t> datagram, const Endpoint& source) {
        if (!joining || !join_view || join_view->status != JoinStatus::Asking) return;
        // The answer comes from where the request went, or it is not one.
        if (!(source == join_view->target)) return;
        const auto accept = ReadPairAccept(datagram);
        if (!accept) return;
        const auto result = joining->Handle(crypto, self, *accept, datagram);
        switch (result.outcome) {
            case Joining::Outcome::Ignored:
                return;
            case Joining::Outcome::Paired:
                if (peers.size() >= kMaxPairs && Find(result.paired->peer) == nullptr) {
                    join_view->status = JoinStatus::Full;
                    break;
                }
                AddPair(*result.paired, false, source);
                join_view->status = JoinStatus::Paired;
                join_view->issuer = result.paired->name;
                break;
            case Joining::Outcome::Used:
                join_view->status = JoinStatus::Used;
                break;
            case Joining::Outcome::Expired:
                join_view->status = JoinStatus::Expired;
                break;
            case Joining::Outcome::Self:
                join_view->status = JoinStatus::Own;
                break;
        }
        joining.reset();
    }

    void OnSealed(const std::span<const std::uint8_t> datagram, const Endpoint& source,
                  const std::uint64_t now) {
        const auto header = ReadSealedHeader(datagram);
        if (!header) return;
        Peer* peer = Find(header->sender);
        if (peer == nullptr) return;
        const std::int64_t wall = WallMs();
        const Verdict verdict = peer->guard.Classify(*header, wall);
        std::optional<Key> key;
        switch (verdict) {
            case Verdict::Current:
                key = peer->receive_key;
                break;
            case Verdict::NewSession:
                key = DirectionKey(crypto, peer->pair.key, header->salt, header->sender, self.id);
                break;
            case Verdict::OutOfWindow: {
                // Refused either way; if it is genuine, the dialog can say the
                // two clocks disagree instead of only that nothing arrives.
                const auto probe =
                    DirectionKey(crypto, peer->pair.key, header->salt, header->sender, self.id);
                if (probe && OpenSealed(crypto, *probe, *header, datagram).authentic) {
                    peer->clock_skew_ms = header->sent_ms - wall;
                }
                return;
            }
            case Verdict::Replayed:
            case Verdict::Older:
                return;
        }
        if (!key) return;
        auto opened = OpenSealed(crypto, *key, *header, datagram);
        if (!opened.authentic) return;
        // Authentic but not a state this version reads: nothing of it is used.
        if (header->type == Type::State && !opened.state) return;
        peer->guard.Accept(*header);
        if (verdict == Verdict::NewSession) peer->receive_key = key;
        peer->clock_skew_ms.reset();
        bool changed = false;
        if (!peer->pair.endpoint || !(*peer->pair.endpoint == source)) {
            peer->pair.endpoint = source;
            changed = true;
        }
        if (header->type == Type::State) {
            peer->heard.state_ms = now;
            if (!opened.state->name.empty() && opened.state->name != peer->pair.name) {
                peer->pair.name = opened.state->name;
                changed = true;
            }
            peer->state = std::move(opened.state);
#ifdef GTG_FEATURE_UPDATE
        } else if (header->type == Type::UpdateStatus) {
            peer->update_status = opened.update_status;
        } else if (header->type == Type::UpdateTrigger) {
            peer->update_trigger = opened.update_trigger;
            peer->update_trigger_ms = now;
#endif
        } else {
            peer->heard.goodbye_ms = now;
        }
        if (changed) Save(peer->pair);
    }

    void Dispatch(const std::span<const std::uint8_t> datagram, const Endpoint& source,
                  const std::uint64_t now) {
        const auto prefix = ReadPrefix(datagram);
        if (!prefix) return;
        switch (prefix->type) {
            case Type::PairRequest:
                OnPairRequest(datagram, source, now);
                break;
            case Type::PairAccept:
                OnPairAccept(datagram, source);
                break;
            case Type::State:
            case Type::Goodbye:
#ifdef GTG_FEATURE_UPDATE
            case Type::UpdateStatus:
            case Type::UpdateTrigger:
#endif
                OnSealed(datagram, source, now);
                break;
        }
    }

    void Drain(const std::uint64_t now) {
        if (socket == INVALID_SOCKET) return;
        WSANETWORKEVENTS events{};
        (void)ws.network_events(socket, socket_event, &events);
        std::array<std::uint8_t, kMaxDatagramBytes> buffer{};
        for (int i = 0; i < kReceiveBurst; ++i) {
            sockaddr_in from{};
            int from_size = sizeof(from);
            const int received = ws.receive_from(socket, reinterpret_cast<char*>(buffer.data()),
                                                 static_cast<int>(buffer.size()), 0,
                                                 reinterpret_cast<sockaddr*>(&from), &from_size);
            if (received == SOCKET_ERROR) {
                const int error = ws.last_error();
                // Larger than any datagram of ours, or an ICMP report: drop it
                // and keep reading.
                if (error == WSAEMSGSIZE || error == WSAECONNRESET) continue;
                break;
            }
            if (from.sin_family != AF_INET || from_size < static_cast<int>(sizeof(from))) continue;
            const Endpoint source = FromSockaddr(from);
            if (!admission.Admit(source, now)) continue;
            Dispatch(
                std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(received)),
                source, now);
        }
    }

    void TickJoin(const std::uint64_t now) {
        if (!joining || !join_view || join_view->status != JoinStatus::Asking) return;
        if (now - join_started_ms >= kJoinTimeoutMs) {
            join_view->status = JoinStatus::NoAnswer;
            joining.reset();
            return;
        }
        if (!join_sent_ms || now - *join_sent_ms >= kSendIntervalMs) {
            SendTo(join_view->target, joining->Request());
            join_sent_ms = now;
        }
    }

    void Apply(Command& command, const std::uint64_t now) {
        switch (command.kind) {
            case Command::Kind::CreateCode: {
                no_address = false;
                if (peers.size() >= kMaxPairs) return;
                const auto address =
                    link.options_.bind_address ? link.options_.bind_address : LocalAddress(ws);
                if (!address) {
                    no_address = true;
                    invitation.reset();
                    return;
                }
                invitation = Invitation::Create(crypto, *address, link.options_.port, now);
                invitation_code = invitation ? FormatCode(invitation->Code()) : std::string();
                if (invitation && !listening) Bind(true, now);
                break;
            }
            case Command::Kind::CancelCode:
                invitation.reset();
                invitation_code.clear();
                break;
            case Command::Kind::Join: {
                const Endpoint target{command.code.address, command.code.port};
                joining.reset();
                join_view = JoinView{target, JoinStatus::Asking, {}};
                if (invitation && invitation->Code().secret == command.code.secret) {
                    join_view->status = JoinStatus::Own;
                    return;
                }
                if (peers.size() >= kMaxPairs) {
                    join_view->status = JoinStatus::Full;
                    return;
                }
                joining = Joining::Start(crypto, self, command.code);
                if (!joining) {
                    join_view->status = JoinStatus::NoAnswer;
                    return;
                }
                join_started_ms = now;
                join_sent_ms.reset();
                TickJoin(now);
                break;
            }
            case Command::Kind::CancelJoin:
                joining.reset();
                join_view.reset();
                break;
            case Command::Kind::Remove: {
                const auto found = std::find_if(peers.begin(), peers.end(), [&](const Peer& peer) {
                    return peer.pair.peer == command.peer;
                });
                if (found == peers.end()) return;
                // A goodbye first, so the other side shows it offline rather
                // than lost.
                SendGoodbye(*found);
                (void)logging::TryInfo(std::format(L"hosts: removed \"{}\"", found->pair.name));
                peers.erase(found);
                if (link.options_.forget) link.options_.forget(command.peer);
                break;
            }
            case Command::Kind::Reorder: {
                std::vector<Peer> ordered;
                ordered.reserve(peers.size());
                for (const std::uint64_t id : command.order) {
                    const auto found = std::find_if(peers.begin(), peers.end(), [&](const Peer& p) {
                        return p.pair.peer == id;
                    });
                    if (found != peers.end()) {
                        ordered.push_back(std::move(*found));
                        peers.erase(found);
                    }
                }
                for (Peer& rest : peers)
                    ordered.push_back(std::move(rest));
                peers = std::move(ordered);
                break;
            }
            case Command::Kind::Rename:
                self.name = CleanName(command.name);
                break;
            case Command::Kind::Suspend:
                if (!suspended) {
                    SendGoodbyes();
                    suspended = true;
                }
                break;
            case Command::Kind::Resume:
                if (suspended) {
                    suspended = false;
                    NewSession();
                    last_send_ms.reset();
                }
                break;
            case Command::Kind::Mute:
                muted = true;
                break;
#ifdef GTG_FEATURE_UPDATE
            case Command::Kind::Trigger: {
                const auto found = std::find_if(peers.begin(), peers.end(), [&](const Peer& peer) {
                    return peer.pair.peer == command.peer;
                });
                if (found == peers.end()) return;
                found->trigger_out = command.trigger;
                found->trigger_rounds = kTriggerRounds;
                break;
            }
#endif
        }
    }

    void Publish(const std::uint64_t now) {
        Table table;
        table.status = status;
        table.port = bound_port;
        table.listening = listening;
        table.no_address = no_address;
        table.hosts.reserve(peers.size());
        for (const Peer& peer : peers) {
            HostRow row;
            row.peer = peer.pair.peer;
            row.name = peer.pair.name;
            row.heard = peer.heard;
            row.state = peer.state;
            row.endpoint = peer.pair.endpoint;
            row.issued = peer.pair.issued;
            row.clock_skew_ms = peer.clock_skew_ms;
#ifdef GTG_FEATURE_UPDATE
            row.update_status = peer.update_status;
            row.update_trigger = peer.update_trigger;
            row.update_trigger_ms = peer.update_trigger_ms;
#endif
            table.hosts.push_back(std::move(row));
        }
        if (invitation) {
            table.invitation = InvitationView{invitation_code, invitation->ExpiresMs(),
                                              invitation->StatusAt(now), invitation->JoinedBy()};
        }
        table.joining = join_view;
        std::lock_guard lock(link.table_mutex_);
        link.table_ = std::move(table);
    }
};

bool Link::Start(Options options) {
    if (thread_.joinable()) return true;
    if (options.pairs.size() > kMaxPairs) options.pairs.resize(kMaxPairs);
    options_ = std::move(options);
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    command_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (stop_ == nullptr || command_ == nullptr) {
        Stop();
        return false;
    }
    {
        std::lock_guard lock(table_mutex_);
        table_ = {};
    }
    try {
        thread_ = std::thread(&Link::Run, this);
    } catch (...) {
        Stop();
        return false;
    }
    return true;
}

void Link::Stop() noexcept {
    if (thread_.joinable()) {
        SetEvent(stop_);
        thread_.join();
    }
    if (stop_ != nullptr) CloseHandle(stop_);
    if (command_ != nullptr) CloseHandle(command_);
    stop_ = nullptr;
    command_ = nullptr;
    std::lock_guard lock(commands_mutex_);
    commands_.clear();
}

void Link::Publish(const HostState& state) {
    std::unique_lock lock(mailbox_mutex_, std::try_to_lock);
    if (lock.owns_lock()) mailbox_ = state;
}

std::optional<Table> Link::TryRead() const {
    std::unique_lock lock(table_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return std::nullopt;
    return table_;
}

void Link::Post(Command command) {
    {
        std::lock_guard lock(commands_mutex_);
        commands_.push_back(std::move(command));
    }
    if (command_ != nullptr) SetEvent(command_);
}

void Link::CreateCode() {
    Post({Command::Kind::CreateCode});
}

void Link::CancelCode() {
    Post({Command::Kind::CancelCode});
}

void Link::Join(const PairingCode& code) {
    Command command{Command::Kind::Join};
    command.code = code;
    Post(std::move(command));
}

void Link::CancelJoin() {
    Post({Command::Kind::CancelJoin});
}

void Link::Remove(const std::uint64_t peer) {
    Command command{Command::Kind::Remove};
    command.peer = peer;
    Post(std::move(command));
}

void Link::Reorder(std::vector<std::uint64_t> order) {
    Command command{Command::Kind::Reorder};
    command.order = std::move(order);
    Post(std::move(command));
}

void Link::Rename(std::wstring name) {
    Command command{Command::Kind::Rename};
    command.name = std::move(name);
    Post(std::move(command));
}

void Link::Suspend() {
    Post({Command::Kind::Suspend});
}

void Link::Resume() {
    Post({Command::Kind::Resume});
}

void Link::Mute() {
    Post({Command::Kind::Mute});
}

#ifdef GTG_FEATURE_UPDATE
void Link::PublishUpdate(const update::hosts::StatusBody& status) {
    std::unique_lock lock(mailbox_mutex_, std::try_to_lock);
    if (lock.owns_lock()) update_mailbox_ = status;
}

void Link::Trigger(const std::uint64_t peer, const update::hosts::TriggerBody& trigger) {
    Command command{Command::Kind::Trigger};
    command.peer = peer;
    command.trigger = trigger;
    Post(std::move(command));
}
#endif

void Link::Run() noexcept {
    (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    try {
        State state(*this);
        Serve(state);
    } catch (...) {
        std::lock_guard lock(table_mutex_);
        table_.status = LinkStatus::Stopped;
    }
    (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
}

void Link::Serve(State& state) {
    state.self = options_.self;
    state.self.name = CleanName(state.self.name);
    for (const Pair& pair : options_.pairs) {
        State::Peer peer;
        peer.pair = pair;
        state.peers.push_back(std::move(peer));
    }
    const auto idle = [&](const LinkStatus status) {
        state.status = status;
        state.Publish(Now());
        (void)WaitForSingleObject(stop_, INFINITE);
    };
    if (!state.crypto.Load()) {
        (void)logging::TryWarning(L"hosts: CNG or DPAPI lacks a function; Hosts is unavailable");
        idle(LinkStatus::CryptoMissing);
        return;
    }
    WSADATA data{};
    if (!state.ws.Load() || state.ws.startup(MAKEWORD(2, 2), &data) != 0) {
        (void)logging::TryWarning(L"hosts: Winsock could not be started; Hosts is unavailable");
        idle(LinkStatus::NetworkMissing);
        return;
    }
    state.winsock_started = true;
    state.socket_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (state.socket_event == nullptr) {
        idle(LinkStatus::NetworkMissing);
        return;
    }

    state.NewSession();
    std::uint64_t now = Now();
    state.Bind(state.WantsFixed(now), now);
    state.Publish(now);
    (void)logging::TryInfo(std::format(L"hosts: started; pairs={} port={}{}", state.peers.size(),
                                       state.bound_port, state.listening ? L" (fixed)" : L""));

    const HANDLE handles[3]{stop_, command_, state.socket_event};
    for (;;) {
        now = Now();
        const auto until = [now](const std::optional<std::uint64_t> last) -> std::uint64_t {
            if (!last) return 0;
            const std::uint64_t due = *last + kSendIntervalMs;
            return due > now ? due - now : 0;
        };
        std::uint64_t wait = until(state.last_send_ms);
        if (state.joining) wait = std::min(wait, until(state.join_sent_ms));
        const DWORD woke = WaitForMultipleObjects(3, handles, FALSE, static_cast<DWORD>(wait));
        if (woke == WAIT_OBJECT_0 || woke == WAIT_FAILED) break;

        now = Now();
        std::vector<Command> commands;
        {
            std::lock_guard lock(commands_mutex_);
            commands.swap(commands_);
        }
        for (Command& command : commands)
            state.Apply(command, now);
        // Every wake, not only the socket's: a command and a datagram arriving
        // together wake the thread once.
        state.Drain(now);
        if (!state.last_send_ms || now - *state.last_send_ms >= kSendIntervalMs) {
            state.SendStates(now);
        }
        state.TickJoin(now);
        const bool fixed = state.WantsFixed(now);
        if (fixed != state.listening && (!fixed || now >= state.retry_fixed_ms)) {
            state.Bind(fixed, now);
        }
        // Already on an ephemeral port, and the fixed one no longer wanted.
        if (!fixed && state.status == LinkStatus::PortInUse) state.status = LinkStatus::Running;
        state.Publish(now);
    }

    if (!state.suspended && !state.muted) state.SendGoodbyes();
    state.status = LinkStatus::Stopped;
    state.Publish(Now());
    (void)logging::TryInfo(L"hosts: stopped");
}

}  // namespace gtg::peer
