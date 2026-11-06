#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "peer/admission.hpp"
#include "peer/crypto.hpp"
#include "peer/hosts_view.hpp"
#include "peer/pairing.hpp"
#include "peer/protocol.hpp"

#ifdef GTG_FEATURE_UPDATE
#include "update/hosts_wire.hpp"
#endif

// The link between this GTG and its paired hosts, on a thread of its own.
//
// The thread exists only while Hosts is on. It owns the socket and the
// cryptography, sends this machine's state to every paired host once a
// second, and receives theirs. It holds no NVML session and shares no lock,
// queue or handle with protection, the Service or IPC; it reads no telemetry
// itself. The UI hands it the local state through a single-slot mailbox and
// reads the hosts back through TryRead; neither waits on the thread.
//
// A GTG binds the fixed port only once it has issued a code -- a live code,
// or a pair it issued. A GTG that only ever joined sends from an ephemeral
// port: Windows asks about a program the first time it binds a non-zero UDP
// port, so a joiner never sees the firewall prompt, and the issuer's
// datagrams reach it as replies to its own.
namespace gtg::peer {

// Below the range Windows hands out for ephemeral ports -- 49152 and up by
// default, 32768 and up where Hyper-V or WSL moved it -- and the blocks
// Hyper-V reserves inside it, so another program's outgoing socket does not
// hold it at the moment GTG wants it.
inline constexpr std::uint16_t kDefaultPort = 29'480;
inline constexpr std::uint64_t kSendIntervalMs = 1'000;
// A joiner asks once a second for this long before it gives up.
inline constexpr std::uint64_t kJoinTimeoutMs = 10'000;
#ifdef GTG_FEATURE_UPDATE
// An update trigger goes out with this many state rounds; the host acts on it
// once whichever copy arrives.
inline constexpr int kTriggerRounds = 5;
#endif

// A paired host as the link keeps it, and as it is stored.
struct Pair {
    std::uint64_t peer{};
    std::wstring name;   // as last heard
    Key key{};
    // This GTG issued the code: it keeps the fixed port bound for this pair.
    bool issued{};
    std::optional<Endpoint> endpoint;   // the last one a datagram authenticated
};

enum class LinkStatus {
    Starting,
    Running,
    CryptoMissing,   // bcrypt or crypt32 lacks an export
    NetworkMissing,  // ws2_32 lacks an export or would not start
    PortInUse,       // the fixed port is taken: another program, or another GTG here
    Stopped,
};

enum class JoinStatus { Asking, Paired, Used, Expired, NoAnswer, Own, Full };

struct InvitationView {
    std::string code;   // as shown and copied
    std::uint64_t expires_ms{};
    Invitation::Status status{Invitation::Status::Live};
    std::wstring joined_by;
};

struct JoinView {
    Endpoint target;
    JoinStatus status{JoinStatus::Asking};
    std::wstring issuer;   // once paired
};

// One paired host as the UI reads it. Times are this machine's monotonic
// clock (GetTickCount64), which the UI reads too.
struct HostRow {
    std::uint64_t peer{};
    std::wstring name;
    Heard heard;
    std::optional<HostState> state;
    std::optional<Endpoint> endpoint;
    // This GTG issued the code: the host reaches it on the fixed port, so a
    // host never heard from may mean the firewall said no.
    bool issued{};
    // A session that authenticated but was refused for its clock: how far the
    // host's clock is from this one's, so the dialog can say so.
    std::optional<std::int64_t> clock_skew_ms;
#ifdef GTG_FEATURE_UPDATE
    // The host's last update status and the last trigger it sent this GTG,
    // with when that arrived: authenticated, not read.
    std::optional<update::hosts::StatusBody> update_status;
    std::optional<update::hosts::TriggerBody> update_trigger;
    std::uint64_t update_trigger_ms{};
#endif
};

namespace link_detail {

struct Command {
    enum class Kind {
        CreateCode,
        CancelCode,
        Join,
        CancelJoin,
        Remove,
        Reorder,
        Rename,
        Suspend,
        Resume,
        Mute,
#ifdef GTG_FEATURE_UPDATE
        Trigger,
#endif
    };
    Kind kind{};
    PairingCode code;
    std::uint64_t peer{};
    std::vector<std::uint64_t> order;
    std::wstring name;
#ifdef GTG_FEATURE_UPDATE
    update::hosts::TriggerBody trigger{};
#endif
};

}  // namespace link_detail

struct Table {
    LinkStatus status{LinkStatus::Starting};
    std::uint16_t port{};     // bound now; 0 while not bound
    bool listening{};         // bound to the fixed port
    // A code was asked for, but this machine has no address another can reach.
    bool no_address{};
    std::vector<HostRow> hosts;   // in the reader's order
    std::optional<InvitationView> invitation;
    std::optional<JoinView> joining;
};

class Link {
public:
    struct Options {
        Self self;
        std::uint16_t port{kDefaultPort};
        std::vector<Pair> pairs;   // in the reader's order, at most kMaxPairs
        // Called on the link thread for a new pair and for a pair whose stored
        // part (name, endpoint) changed, so it can be saved.
        std::function<void(const Pair&)> save;
        // Called on the link thread when a pair is removed.
        std::function<void(std::uint64_t peer)> forget;
        // Where to bind and what address a code names. Unset: any address,
        // and the default route's address in codes. Tests use loopback.
        std::optional<std::array<std::uint8_t, 4>> bind_address;
    };

    Link() = default;

    ~Link() { Stop(); }

    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    [[nodiscard]] bool Start(Options options);
    // Says goodbye to every host and joins the thread.
    void Stop() noexcept;

    [[nodiscard]] bool Running() const noexcept { return thread_.joinable(); }

    // The local state to send from now on. Dropped if the thread holds the
    // slot at this instant; the next second's call replaces it anyway.
    void Publish(const HostState& state);

    // The last published table, or nothing if it is being published at this
    // instant. Never waits.
    [[nodiscard]] std::optional<Table> TryRead() const;

    // Requests carried out on the thread, in order.
    void CreateCode();
    void CancelCode();
    void Join(const PairingCode& code);
    void CancelJoin();
    void Remove(std::uint64_t peer);
    void Reorder(std::vector<std::uint64_t> order);
    void Rename(std::wstring name);
    // Sleep: say goodbye now and send nothing until Resume, which starts a
    // new session.
    void Suspend();
    void Resume();
    // Stop sending, without a goodbye: what a crash or a pulled cable looks
    // like from the other side. For the tests and the fake hosts tool; Hosts
    // itself never mutes.
    void Mute();

#ifdef GTG_FEATURE_UPDATE
    // This machine's update status, sent after its state from now on, through
    // the same kind of mailbox.
    void PublishUpdate(const update::hosts::StatusBody& status);
    // Asks one paired host to update: the trigger goes out with its next
    // state rounds.
    void Trigger(std::uint64_t peer, const update::hosts::TriggerBody& trigger);
#endif

private:
    struct State;

    void Post(link_detail::Command command);
    void Run() noexcept;
    void Serve(State& state);

    Options options_;
    std::thread thread_;
    void* stop_{};      // manual-reset event
    void* command_{};   // auto-reset event

    std::mutex commands_mutex_;
    std::vector<link_detail::Command> commands_;

    mutable std::mutex mailbox_mutex_;
    std::optional<HostState> mailbox_;
#ifdef GTG_FEATURE_UPDATE
    std::optional<update::hosts::StatusBody> update_mailbox_;
#endif

    mutable std::mutex table_mutex_;
    Table table_;
};

}  // namespace gtg::peer
