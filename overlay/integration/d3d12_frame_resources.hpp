#pragma once
#include "d3d12_slots.hpp"
#include "d3d12_transition_wait.hpp"
#include <d3d12.h>
#include <wrl/client.h>

namespace gtg::overlay::integration {
enum class Retirement { Released, Retained };
class D3d12FrameResources {
public:
    // All access, including destruction, requires the same external admission.
    // The caller qualifies queue/chain identity before initializing this owner.
    D3d12FrameResources() = default;
    D3d12FrameResources(const D3d12FrameResources&) = delete;
    D3d12FrameResources& operator=(const D3d12FrameResources&) = delete;
    ~D3d12FrameResources() noexcept { Close(); }
    bool Initialize(ID3D12CommandQueue* queue) noexcept {
        if (attempted_ || closed_) return false;
        attempted_ = true;
        if (!queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
            FAILED(queue->GetDevice(IID_PPV_ARGS(&device_))) || !device_ || device_->GetNodeCount() != 1)
            return FailInitialization();
        queue_ = queue;
        if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
            return FailInitialization();
        transition_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!transition_event_) return FailInitialization();
        for (int i = 0; i < D3d12Slots::kSlots; ++i) {
            if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(&allocators_[i]))) ||
                FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    allocators_[i].Get(), nullptr, IID_PPV_ARGS(&lists_[i]))) ||
                FAILED(lists_[i]->Close())) return FailInitialization();
        }
        initialized_ = true;
        return true;
    }
    ID3D12GraphicsCommandList* TryBegin() noexcept {
        if (!initialized_ || closed_ || Failed() || open_ >= 0) return nullptr;
        const int index = slots_.Reserve(fence_->GetCompletedValue());
        if (index < 0) return nullptr;
        if (FAILED(allocators_[index]->Reset()) ||
            FAILED(lists_[index]->Reset(allocators_[index].Get(), nullptr))) {
            slots_.Submitted(index, 0, false);
            failed_ = true;
            return nullptr;
        }
        open_ = index;
        return lists_[index].Get();
    }
    bool Submit() noexcept {
        const auto signal = [](ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) noexcept {
            return queue->Signal(fence, value);
        };
        return Submit(signal);
    }
    // Injectable operation is for owned failure tests. Product uses Submit().
    template<class Signal> bool Submit(Signal& signal) noexcept {
        if (closed_ || Failed() || open_ < 0) return false;
        const int index = open_;
        open_ = -1;
        if (FAILED(lists_[index]->Close()) || last_value_ >= kRemoved - 1) {
            slots_.Submitted(index, 0, false);
            failed_ = true;
            return false;
        }
        ID3D12CommandList* list = lists_[index].Get();
        queue_->ExecuteCommandLists(1, &list);
        submitted_work_ = true;
        ++last_value_;
        const bool signaled = SUCCEEDED(signal(queue_.Get(), fence_.Get(), last_value_));
        if (!signaled) uncertain_ = true;
        return slots_.Submitted(index, last_value_, signaled);
    }
    bool Cancel() noexcept {
        if (closed_ || Failed() || open_ < 0) return false;
        const int index = open_;
        open_ = -1;
        if (FAILED(lists_[index]->Close())) {
            slots_.Submitted(index, 0, false);
            failed_ = true;
            return false;
        }
        return slots_.Cancel(index);
    }
    void Invalidate() noexcept {
        if (closed_) return;
        if (open_ >= 0) Cancel();
        slots_.Invalidate();
    }
    bool Resume() noexcept {
        return initialized_ && !closed_ && !Failed() &&
            slots_.Resume(fence_->GetCompletedValue());
    }
    // Only approved resize/color transitions may call this. The caller already
    // owns admission and passes the remaining shared transition deadline.
    // Invalidate stops old-generation reservations before any wait. Neither
    // TryBegin nor Submit waits for GPU work.
    DrainResult DrainForTransition(DWORD budget = kTransitionWaitMs) noexcept {
        if (!initialized_ || closed_) return DrainResult::Unproven;
        Invalidate();
        if (Failed() || uncertain_) return DrainResult::Unproven;
        struct Ops {
            D3d12FrameResources& owner;
            std::uint64_t Now() noexcept { return GetTickCount64(); }
            std::uint64_t Completed() noexcept { return owner.fence_->GetCompletedValue(); }
            HRESULT Notify(std::uint64_t target) noexcept {
                // Retain the event even on a failed registration unless proof
                // later establishes completion. Never pass a null HANDLE.
                owner.event_registered_ = true;
                return owner.fence_->SetEventOnCompletion(target, owner.transition_event_);
            }
            DWORD Wait(DWORD remaining) noexcept {
                return WaitForSingleObject(owner.transition_event_, remaining);
            }
        } ops{*this};
        const DrainResult result = DrainPrivateFence(ops, last_value_, budget);
        if (result == DrainResult::Complete) {
            event_registered_ = false;
        } else {
            failed_ = true;
            // Completion and asynchronous event lifetime are no longer proven.
            // This owner permanently stops; runtime must not replace it.
            if (submitted_work_) uncertain_ = true;
        }
        return result;
    }
    bool Failed() const noexcept { return failed_ || slots_.Failed(); }
    UINT OpenSlot() const noexcept { return static_cast<UINT>(open_); }
    UINT64 Completed() const noexcept { return fence_ ? fence_->GetCompletedValue() : 0; }
    UINT64 PendingValue() const noexcept { return last_value_ + 1; }
    unsigned Quarantined() const noexcept { return slots_.Quarantined(); }
    Retirement Close() noexcept {
        if (closed_) return retirement_;
        closed_ = true;
        if (open_ >= 0) { lists_[open_]->Close(); open_ = -1; }
        const UINT64 completed = fence_ ? fence_->GetCompletedValue() : 0;
        if (event_registered_ ||
            (submitted_work_ && (uncertain_ || completed == kRemoved || completed < last_value_))) {
            // Unknown GPU lifetime: one bounded owner becomes permanently
            // disabled. Intentionally retain its COM references until process
            // exit, never release possibly executing resources or hot-unload.
            for (auto& list : lists_) list.Detach();
            for (auto& allocator : allocators_) allocator.Detach();
            fence_.Detach(); queue_.Detach(); device_.Detach();
            transition_event_ = nullptr;  // Intentionally retained with its fence.
            retirement_ = Retirement::Retained;
        } else {
            for (auto& list : lists_) list.Reset();
            for (auto& allocator : allocators_) allocator.Reset();
            fence_.Reset(); queue_.Reset(); device_.Reset();
            if (transition_event_) CloseHandle(transition_event_);
            transition_event_ = nullptr;
        }
        return retirement_;
    }

private:
    bool FailInitialization() noexcept { failed_ = true; return false; }
    static constexpr UINT64 kRemoved = std::numeric_limits<UINT64>::max();
    D3d12Slots slots_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, D3d12Slots::kSlots> allocators_;
    std::array<Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>, D3d12Slots::kSlots> lists_;
    int open_ = -1;
    UINT64 last_value_ = 0;
    Retirement retirement_ = Retirement::Released;
    bool attempted_ = false;
    bool initialized_ = false;
    bool failed_ = false;
    bool closed_ = false;
    bool submitted_work_ = false;
    bool uncertain_ = false;
    HANDLE transition_event_ = nullptr;
    bool event_registered_ = false;
};
}  // namespace gtg::overlay::integration
