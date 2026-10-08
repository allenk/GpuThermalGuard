// The published bitmap as a D3D12 texture. The GPU half of PublishedBitmapT for
// the D3D12 probe; see published_bitmap.hpp for the contract.
//
// D3D11 maps a dynamic texture and is done. D3D12 has no such thing, so an
// upload is a copy recorded into the overlay's own command list -- the one that
// then draws -- and executed on the host's queue with the rest of the frame:
//
//   CPU writes an upload buffer -> CopyTextureRegion -> barrier -> draw
//
// No CPU wait. ImGui's backend has a texture path of its own, and it is not used
// for this: it executes on the queue and then blocks the render thread until the
// queue drains (imgui_impl_dx12.cpp 1.92.9b, ImGui_ImplDX12_UpdateTexture), which
// is tolerable for a font atlas once and not for a dashboard that changes twice a
// second.
//
// The upload buffer is per upload slot, and the slot is the caller's: the
// UploadListSource opens a command list only on a frame that uploads, waiting
// first for that slot's previous copy to finish, so the CPU never overwrites
// bytes a copy still reads -- and a frame that uploads nothing records nothing
// (design §10.1, L3).
//
// A texture replaced by a resize is retired with the fence value of the frame
// that replaced it, and released once the GPU has passed that value. Not "when
// a slot comes round", as the first version had it: with L3 the same recorded
// draw is executed on every frame, so the frames that may still sample the old
// texture are exactly those before the fence, not those of one slot.
//
// The SRV descriptor is per TEXTURE: written once when the texture is created,
// never rewritten while it lives, returned to the pool when the texture is
// released. A shader-visible descriptor is read when the GPU executes, not when
// the command is recorded, so rewriting one in place would let a frame still in
// flight sample a different texture. Per texture rather than per slot (Spike 3's
// first version) because design §10.1's L2/L3 caches keep the texture handle
// across frames: it must stay valid for as long as the texture does, and change
// exactly when the texture does.
#pragma once

#include <windows.h>

#include <d3d12.h>

#include <imgui.h>

#include "../ipc/osd_section.hpp"
#include "published_bitmap.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gtg::overlay::probe {

// Where an upload is recorded. Opened lazily, on the first upload of a frame:
// Open waits (bounded) for the slot's previous use, resets its allocator and
// list, and returns the list -- or nullptr if the slot could not be had.
class UploadListSource {
public:
    virtual ID3D12GraphicsCommandList* Open() = 0;
    virtual UINT Slot() const = 0;

protected:
    ~UploadListSource() = default;
};

class D3D12Texture {
public:
    static bool SupportsHeader(const ipc::FrameHeader& header) noexcept {
        return header.format == static_cast<std::uint32_t>(ipc::PixelFormat::Bgra8) &&
               header.alpha_mode <= static_cast<std::uint32_t>(ipc::AlphaMode::Premultiplied);
    }

    static constexpr UINT kMaxSlots = 4;

    // The live texture plus those retired and not yet released: retirement is
    // rare (a resize), and a fence passes within a few frames.
    static constexpr UINT kMaxDescriptors = kMaxSlots + 1;

    // `srv_cpu` / `srv_gpu` address `descriptors` consecutive descriptors,
    // `stride` bytes apart, in a shader-visible CBV_SRV_UAV heap owned by the
    // caller. `descriptors` should be `slots` + 1 (see kMaxDescriptors).
    void Bind(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu,
              D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu, UINT stride, UINT slots, UINT descriptors) {
        device_ = device;
        srv_cpu_ = srv_cpu;
        srv_gpu_ = srv_gpu;
        stride_ = stride;
        slots_ = slots < kMaxSlots ? slots : kMaxSlots;
        descriptors_ = descriptors < kMaxDescriptors ? descriptors : kMaxDescriptors;
    }

    // Called each frame before the transport is asked for a bitmap.
    // `completed` is the overlay fence's completed value; `pending` the value
    // this frame will signal.
    void BeginFrame(UploadListSource* source, UINT64 completed, UINT64 pending) {
        source_ = source;
        list_ = nullptr;
        pending_ = pending;
        for (Retired& r : retired_) {
            if (r.texture != nullptr && r.fence <= completed) ReleaseRetired(r);
        }
    }

    bool Bound() const { return device_ != nullptr && source_ != nullptr; }

    bool Has() const {
        return tex_ != nullptr && state_ == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    bool Matches(std::uint32_t w, std::uint32_t h) const { return tex_w_ == w && tex_h_ == h; }

    // Stable from one texture creation to the next -- the L2/L3 cache key.
    ImTextureID Id() const { return static_cast<ImTextureID>(DescriptorGpu(tex_descriptor_).ptr); }

    // The stage is this slot's upload buffer, written straight from the section.
    // Nothing reads it until Commit records a copy, so a torn stage is dropped by
    // simply not committing -- no CPU staging buffer, and no second copy, which
    // D3D11 needs and D3D12 does not (design §10.1).
    bool Stage(const std::uint8_t* section_pixels, const ipc::FrameHeader& h,
               ConsumerStats& stats) {
        // Footprint from the device, not computed: the 256-byte row alignment is
        // the rule today, and asking is what keeps this right if it is not.
        const D3D12_RESOURCE_DESC desc = TextureDesc(h);
        UINT64 total = 0;
        device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint_, nullptr, nullptr, &total);
        // Opening the list is what makes the slot's upload buffer safe to write.
        list_ = source_->Open();
        if (list_ == nullptr) return false;
        slot_ = source_->Slot() % slots_;
        Slot& s = per_slot_[slot_];
        if (!EnsureUploadBuffer(s, total, stats)) return false;

        // Row by row, for the same reason as D3D11: the copy's pitch is the
        // device's, not ours.
        auto* dst = static_cast<std::uint8_t*>(s.mapped) + footprint_.Offset;
        const std::size_t row_bytes = static_cast<std::size_t>(h.width) * 4u;
        for (std::uint32_t y = 0; y < h.height; ++y) {
            std::memcpy(dst + static_cast<std::size_t>(y) * footprint_.Footprint.RowPitch,
                        section_pixels + static_cast<std::size_t>(y) * h.stride, row_bytes);
        }
        return true;
    }

    bool Commit(const ipc::FrameHeader& h, ConsumerStats& stats) {
        if (list_ == nullptr) return false;
        if (tex_ == nullptr || tex_w_ != h.width || tex_h_ != h.height) {
            if (!Recreate(h)) return false;
            stats.texture_recreates.fetch_add(1, std::memory_order_relaxed);
            stats.allocations.fetch_add(1, std::memory_order_relaxed);
        }
        Slot& s = per_slot_[slot_];
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = footprint_;

        if (state_ != D3D12_RESOURCE_STATE_COPY_DEST) {
            Transition(state_, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = tex_;
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = s.upload;
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = footprint;
        list_->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        Transition(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        return true;
    }

    // Only once the GPU is idle for everything this owns -- the caller's job.
    void Release() {
        for (Retired& r : retired_)
            ReleaseRetired(r);
        for (Slot& s : per_slot_) {
            if (s.upload != nullptr) {
                s.upload->Release();
                s.upload = nullptr;
            }
            s.mapped = nullptr;
            s.size = 0;
        }
        if (tex_ != nullptr) {
            tex_->Release();
            tex_ = nullptr;
            descriptor_busy_[tex_descriptor_] = false;
        }
        tex_w_ = 0;
        tex_h_ = 0;
    }

private:
    struct Slot {
        ID3D12Resource* upload = nullptr;
        void* mapped = nullptr;
        UINT64 size = 0;
    };

    struct Retired {
        ID3D12Resource* texture = nullptr;
        UINT descriptor = 0;
        UINT64 fence = 0;
    };

    D3D12_CPU_DESCRIPTOR_HANDLE DescriptorCpu(UINT index) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = srv_cpu_;
        h.ptr += static_cast<SIZE_T>(index) * stride_;
        return h;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE DescriptorGpu(UINT index) const {
        D3D12_GPU_DESCRIPTOR_HANDLE h = srv_gpu_;
        h.ptr += static_cast<UINT64>(index) * stride_;
        return h;
    }

    void ReleaseRetired(Retired& r) {
        if (r.texture == nullptr) return;
        r.texture->Release();
        r.texture = nullptr;
        descriptor_busy_[r.descriptor] = false;
    }

    // Written once, at creation. See the header comment for why never again.
    void WriteSrv() {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // == PixelFormat::Bgra8
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(tex_, &view, DescriptorCpu(tex_descriptor_));
    }

    void Transition(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = tex_;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        list_->ResourceBarrier(1, &barrier);
        state_ = after;
    }

    static D3D12_RESOURCE_DESC TextureDesc(const ipc::FrameHeader& h) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = h.width;
        desc.Height = h.height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // == PixelFormat::Bgra8
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        return desc;
    }

    bool Recreate(const ipc::FrameHeader& h) {
        // A descriptor nothing still uses. Checked before anything is retired,
        // so running out leaves the current texture exactly as it was.
        UINT free_descriptor = descriptors_;
        for (UINT i = 0; i < descriptors_; ++i) {
            if (!descriptor_busy_[i]) {
                free_descriptor = i;
                break;
            }
        }
        if (free_descriptor == descriptors_) {
            std::printf("[T-04] no free SRV descriptor for a %ux%u texture\n", h.width, h.height);
            return false;
        }
        // The old texture may still be sampled by frames already submitted, so
        // it is retired -- with its descriptor -- until this frame's fence has
        // passed. A free descriptor implies a free retirement entry: each
        // retired texture holds one descriptor.
        if (tex_ != nullptr) {
            for (Retired& r : retired_) {
                if (r.texture != nullptr) continue;
                r.texture = tex_;
                r.descriptor = tex_descriptor_;
                r.fence = pending_;
                break;
            }
            tex_ = nullptr;
        }
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        const D3D12_RESOURCE_DESC desc = TextureDesc(h);
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&tex_))) ||
            tex_ == nullptr) {
            std::printf("[T-04] D3D12 texture %ux%u creation failed\n", h.width, h.height);
            tex_ = nullptr;
            return false;
        }
        state_ = D3D12_RESOURCE_STATE_COPY_DEST;
        tex_w_ = h.width;
        tex_h_ = h.height;
        tex_descriptor_ = free_descriptor;
        descriptor_busy_[free_descriptor] = true;
        WriteSrv();
        std::printf("[T-04] texture %ux%u created\n", h.width, h.height);
        return true;
    }

    bool EnsureUploadBuffer(Slot& s, UINT64 bytes, ConsumerStats& stats) {
        if (s.upload != nullptr && s.size >= bytes) return true;
        stats.allocations.fetch_add(1, std::memory_order_relaxed);
        if (s.upload != nullptr) {
            s.upload->Release();  // this slot's fence has passed
            s.upload = nullptr;
            s.mapped = nullptr;
        }
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&s.upload))) ||
            s.upload == nullptr) {
            s.upload = nullptr;
            return false;
        }
        // Persistently mapped: an upload heap may stay mapped for its lifetime,
        // and the CPU only writes it while this slot is idle.
        const D3D12_RANGE no_read{0, 0};
        if (FAILED(s.upload->Map(0, &no_read, &s.mapped)) || s.mapped == nullptr) {
            s.upload->Release();
            s.upload = nullptr;
            return false;
        }
        s.size = bytes;
        return true;
    }

    ID3D12Device* device_ = nullptr;
    UploadListSource* source_ = nullptr;
    ID3D12GraphicsCommandList* list_ = nullptr;  // open only on an upload frame
    UINT64 pending_ = 0;
    std::array<Retired, kMaxDescriptors> retired_{};
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_{};
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu_{};
    UINT stride_ = 0;
    UINT slots_ = 1;
    UINT slot_ = 0;
    UINT descriptors_ = 1;
    UINT tex_descriptor_ = 0;
    std::array<bool, kMaxDescriptors> descriptor_busy_{};

    ID3D12Resource* tex_ = nullptr;
    D3D12_RESOURCE_STATES state_ = D3D12_RESOURCE_STATE_COMMON;
    std::uint32_t tex_w_ = 0;
    std::uint32_t tex_h_ = 0;
    std::array<Slot, kMaxSlots> per_slot_{};
    // Written by Stage, read by the Commit that follows it on the same frame.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
};

}  // namespace gtg::overlay::probe
