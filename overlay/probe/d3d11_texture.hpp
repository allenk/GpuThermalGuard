// The published bitmap as a D3D11 texture: a dynamic texture, mapped and written
// on the immediate context. The GPU half of PublishedBitmapT for the D3D11
// probe; see published_bitmap.hpp for the contract.
//
// Moved here unchanged from published_bitmap.hpp when Spike 3 needed a D3D12
// counterpart.
#pragma once

#include <windows.h>

#include <d3d11.h>

#include <imgui.h>

#include "../ipc/osd_section.hpp"
#include "published_bitmap.hpp"
#include "probe_print.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gtg::overlay::probe {

class D3D11Texture {
public:
    void Bind(ID3D11Device* device, ID3D11DeviceContext* context) {
        device_ = device;
        context_ = context;
    }

    bool Bound() const { return device_ != nullptr && context_ != nullptr; }
    void SetTargetFormat(DXGI_FORMAT format) noexcept {
        srgb_output_ = format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
            format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    }
    bool Has() const { return srv_ != nullptr; }
    bool Matches(std::uint32_t w, std::uint32_t h) const {
        return tex_w_ == w && tex_h_ == h;
    }
    ImTextureID Id() const {
        return static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(srgb_output_ ? srgb_srv_ : srv_));
    }

    // D3D11's stage is a CPU copy, and it has to be: Map(WRITE_DISCARD) destroys
    // the texture's contents before a torn read could be detected, so the bytes
    // must be held somewhere that can still be thrown away.
    bool Stage(const std::uint8_t* section_pixels, const ipc::FrameHeader& h,
               ConsumerStats&) {
        staging_.resize(h.byte_count);
        std::memcpy(staging_.data(), section_pixels, h.byte_count);
        return true;
    }

    bool Commit(const ipc::FrameHeader& h, ConsumerStats& stats) {
        return Upload(staging_.data(), h, stats);
    }

    bool Upload(const std::uint8_t* pixels, const ipc::FrameHeader& h,
                ConsumerStats& stats) {
        if (tex_ == nullptr || tex_w_ != h.width || tex_h_ != h.height) {
            // A resolution change recreates the texture rather than stretching
            // into the old one. T-06 is the test; this is the mechanism.
            Release();
            D3D11_TEXTURE2D_DESC td{};
            td.Width = h.width;
            td.Height = h.height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            // One encoded BGRA bitmap, two views. sRGB output needs sampling
            // decode; UNORM output preserves the original encoded-byte path.
            td.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DYNAMIC;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device_->CreateTexture2D(&td, nullptr, &tex_)) ||
                tex_ == nullptr) {
                Print("[T-04] CreateTexture2D failed for %ux%u\n",
                            h.width, h.height);
                return false;
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels = 1;
            if (FAILED(device_->CreateShaderResourceView(tex_, &view, &srv_)) ||
                srv_ == nullptr) {
                Release();
                return false;
            }
            view.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            if (FAILED(device_->CreateShaderResourceView(tex_, &view, &srgb_srv_)) || !srgb_srv_) {
                Release();
                return false;
            }
            tex_w_ = h.width;
            tex_h_ = h.height;
            stats.texture_recreates.fetch_add(1, std::memory_order_relaxed);
            stats.allocations.fetch_add(1, std::memory_order_relaxed);
            Print("[T-04] texture %ux%u created\n", h.width, h.height);
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(tex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return false;
        }
        // Row by row: the driver's pitch has nothing to do with ours, and
        // assuming they agree is the classic way to get a sheared image.
        auto* dst = static_cast<std::uint8_t*>(mapped.pData);
        const std::size_t row_bytes = static_cast<std::size_t>(h.width) * 4u;
        for (std::uint32_t y = 0; y < h.height; ++y) {
            std::memcpy(dst + static_cast<std::size_t>(y) * mapped.RowPitch,
                        pixels + static_cast<std::size_t>(y) * h.stride, row_bytes);
        }
        context_->Unmap(tex_, 0);
        return true;
    }

    void Release() {
        if (srgb_srv_ != nullptr) { srgb_srv_->Release(); srgb_srv_ = nullptr; }
        if (srv_ != nullptr) { srv_->Release(); srv_ = nullptr; }
        if (tex_ != nullptr) { tex_->Release(); tex_ = nullptr; }
        tex_w_ = 0;
        tex_h_ = 0;
    }

private:
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11Texture2D* tex_ = nullptr;
    ID3D11ShaderResourceView* srv_ = nullptr;
    ID3D11ShaderResourceView* srgb_srv_ = nullptr;
    bool srgb_output_{};
    std::uint32_t tex_w_ = 0;
    std::uint32_t tex_h_ = 0;
    std::vector<std::uint8_t> staging_;
};

}  // namespace gtg::overlay::probe
