#pragma once
#include "load_protocol.hpp"
#include "draw_admission.hpp"
#include "chain_handoff.hpp"
#include <ostream>
#include <type_traits>

namespace gtg::research {
constexpr DWORD kDrawDiagnosticMagic = 0x52475447;
constexpr DWORD kDrawDiagnosticVersion = 4;

struct alignas(8) HandoffSnapshot {
    volatile LONG state{};
    HandoffDecision decision{};
};

enum DrawMismatchBits : DWORD { kChainChanged = 1, kDeviceChanged = 2, kThreadChanged = 4 };

struct alignas(8) DrawMismatchSnapshot {
    volatile LONG state{};
    DWORD mask{};
    DrawIdentity actual{};
    std::uint64_t output_window{};
    DWORD width{}, height{};
    HRESULT desc_result{E_PENDING}, fullscreen_result{E_PENDING};
    BOOL fullscreen{};
};

// Two immutable snapshots; state2 publishes a complete group. Not a pixel oracle.
struct alignas(8) DrawDiagnostics {
    DWORD magic{}, size{}, pid{}, version{kDrawDiagnosticVersion};
    volatile LONG identity_state{}, draw_state{};
    std::uint64_t chain{}, device{}, output_window{};
    DWORD thread{}, width{}, height{}, format{}, swap_effect{};
    HRESULT desc_result{}, on12_result{};
    LONG predicate_present{}, predicate_value{}, lists{}, vertices{}, commands{}, visible_clips{};
    float clip_left{}, clip_top{}, clip_right{}, clip_bottom{};
    volatile LONG busy{}, mismatch{}, invalid{};
    HRESULT fullscreen_result{E_PENDING};
    BOOL fullscreen{};
    volatile LONG mismatch_chain{}, mismatch_device{}, mismatch_thread{};
    DrawMismatchSnapshot first_mismatch;
    alignas(8) volatile LONG64 generation{};
    volatile LONG handoff_result{};
    HandoffSnapshot first_commit, first_latch;
};

inline void RecordHandoffDecision(DrawDiagnostics& data, const HandoffDecision& decision) noexcept {
    auto& snapshot = decision.result == HandoffResult::kDraw ? data.first_commit : data.first_latch;
    if (InterlockedCompareExchange(&snapshot.state, 1, 0) != 0) return;
    snapshot.decision = decision;
    InterlockedExchange(&snapshot.state, 2);
}

template <typename Inspect>
inline void RecordDrawMismatch(DrawDiagnostics& data, DrawIdentity expected, DrawIdentity actual,
                               Inspect&& inspect) noexcept {
    static_assert(std::is_nothrow_invocable_v<Inspect&, DrawMismatchSnapshot&>);
    const DWORD mask = (expected.chain != actual.chain ? kChainChanged : 0U) |
                       (expected.device != actual.device ? kDeviceChanged : 0U) |
                       (expected.thread != actual.thread ? kThreadChanged : 0U);
    if (!mask) return;
    if (mask & kChainChanged) InterlockedIncrement(&data.mismatch_chain);
    if (mask & kDeviceChanged) InterlockedIncrement(&data.mismatch_device);
    if (mask & kThreadChanged) InterlockedIncrement(&data.mismatch_thread);
    auto& snapshot = data.first_mismatch;
    if (InterlockedCompareExchange(&snapshot.state, 1, 0) != 0) return;
    snapshot.mask = mask;
    snapshot.actual = actual;
    inspect(snapshot);
    InterlockedExchange(&snapshot.state, 2);
}

inline void DrawSectionName(DWORD pid, wchar_t (&name)[64]) noexcept {
    SectionName(pid, name);
    wcscat_s(name, L".Draw");
}

class DrawDiagnosticMapping {
public:
    DrawDiagnosticMapping() = default;
    DrawDiagnosticMapping(const DrawDiagnosticMapping&) = delete;
    DrawDiagnosticMapping& operator=(const DrawDiagnosticMapping&) = delete;

    ~DrawDiagnosticMapping() {
        if (data_) UnmapViewOfFile(data_);
        if (handle_) CloseHandle(handle_);
    }

    bool Create(DWORD pid) noexcept {
        wchar_t name[64]{};
        DrawSectionName(pid, name);
        handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                     sizeof(DrawDiagnostics), name);
        if (!handle_ || GetLastError() == ERROR_ALREADY_EXISTS) return false;
        data_ = static_cast<DrawDiagnostics*>(
            MapViewOfFile(handle_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DrawDiagnostics)));
        if (!data_) return false;
        *data_ = {};
        data_->magic = kDrawDiagnosticMagic;
        data_->size = sizeof(DrawDiagnostics);
        data_->pid = pid;
        return true;
    }

    DrawDiagnostics* Data() const noexcept { return data_; }

private:
    HANDLE handle_{};
    DrawDiagnostics* data_{};
};

inline void PrintDrawDiagnostics(std::ostream& out, const DrawDiagnostics& d) {
    out << "draw-diagnostic identity=" << d.identity_state << " sample=" << d.draw_state;
    if (d.identity_state == 2)
        out << " chain=" << d.chain << " device=" << d.device << " tid=" << d.thread
            << " hwnd=" << d.output_window << " size=" << d.width << 'x' << d.height
            << " format=" << d.format << " effect=" << d.swap_effect << " desc_hr=" << d.desc_result
            << " on12_hr=" << d.on12_result << " initial_fullscreen_hr=" << d.fullscreen_result
            << " initial_fullscreen="
            << (SUCCEEDED(d.fullscreen_result) ? (d.fullscreen ? "yes" : "no") : "unknown");
    if (d.draw_state == 2)
        out << " predicate=" << d.predicate_present << ':' << d.predicate_value
            << " lists=" << d.lists << " vertices=" << d.vertices << " commands=" << d.commands
            << " visible_clips=" << d.visible_clips << " first_clip=" << d.clip_left << ','
            << d.clip_top << ',' << d.clip_right << ',' << d.clip_bottom;
    out << " busy=" << d.busy << " mismatch=" << d.mismatch << " invalid=" << d.invalid
        << " changed_chain=" << d.mismatch_chain << " changed_device=" << d.mismatch_device
        << " changed_thread=" << d.mismatch_thread << '\n';
    out << "handoff generation=" << d.generation << " result=" << d.handoff_result << '\n';
    const auto print_decision = [&](const char* name, const HandoffSnapshot& snapshot) {
        out << name << " state=" << snapshot.state;
        if (snapshot.state == 2) {
            const auto& s = snapshot.decision;
            out << " owner_chain=" << s.owner.chain << " owner_device=" << s.owner.device
                << " owner_tid=" << s.owner.thread << " candidate_chain=" << s.candidate.chain
                << " candidate_device=" << s.candidate.device
                << " candidate_tid=" << s.candidate.thread << " generation=" << s.generation
                << " result=" << static_cast<int>(s.result) << " hwnd=" << s.facts.window
                << " valid=" << s.facts.valid << " size=" << s.facts.width << 'x' << s.facts.height
                << " format=" << s.facts.format << " fullscreen="
                << (s.facts.valid ? (s.facts.fullscreen ? "yes" : "no") : "unknown")
                << " tag_state=" << static_cast<int>(s.tag.state) << " tag_hr=" << s.tag.result
                << " tag_size=" << s.tag.size << " tag_session=" << s.tag.stamp.session
                << " tag_generation=" << s.tag.stamp.generation
                << " tag_version=" << s.tag.stamp.version
                << " tag_reserved=" << s.tag.stamp.reserved;
        }
        out << '\n';
    };
    print_decision("first-commit", d.first_commit);
    print_decision("first-latch", d.first_latch);
    if (d.first_mismatch.state == 2) {
        const auto& s = d.first_mismatch;
        out << "first-rejected mask=" << s.mask << " chain=" << s.actual.chain
            << " device=" << s.actual.device << " tid=" << s.actual.thread
            << " hwnd=" << s.output_window << " size=" << s.width << 'x' << s.height
            << " desc_hr=" << s.desc_result << " fullscreen_hr=" << s.fullscreen_result
            << " fullscreen="
            << (SUCCEEDED(s.fullscreen_result) ? (s.fullscreen ? "yes" : "no") : "unknown") << '\n';
    }
}
}  // namespace gtg::research
