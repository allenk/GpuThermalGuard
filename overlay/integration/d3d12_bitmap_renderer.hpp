#pragma once
#include "d3d12_frame_resources.hpp"
#include "d3d12_lifecycle_gate.hpp"
#include "sdr_target_format.hpp"
#include "../probe/d3d12_texture.hpp"
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <atomic>
#include <cstring>

namespace gtg::overlay::integration {
class D3d12BitmapRenderer {
public:
    D3d12BitmapRenderer() = default;
    D3d12BitmapRenderer(const D3d12BitmapRenderer&) = delete;
    D3d12BitmapRenderer& operator=(const D3d12BitmapRenderer&) = delete;

    ~D3d12BitmapRenderer() noexcept { Close(); }

    // Worker-only, before installing hooks. No runtime compilation in Present.
    bool Prepare() noexcept {
        if (prepared_) return true;
        if (compiler_ || runtime_) return false;
        compiler_ = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        runtime_ = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!compiler_ || !runtime_) return false;
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return false;
        qpc_frequency_ = static_cast<UINT64>(frequency.QuadPart);
        const auto compile =
            reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler_, "D3DCompile"));
        const auto serialize = reinterpret_cast<decltype(&D3D12SerializeRootSignature)>(
            GetProcAddress(runtime_, "D3D12SerializeRootSignature"));
        if (!compile || !serialize) return false;
        constexpr char shader[] =
            "cbuffer Placement:register(b0){float4 rect;uint alphaMode;}"
            "struct V{float4 position:SV_POSITION;float2 uv:TEXCOORD0;};"
            "V VS(uint id:SV_VertexID){V v;float2 uv=float2(id&1,id>>1);"
            "v.uv=uv;v.position=float4(lerp(rect.xy,rect.zw,uv),0,1);return v;}"
            "Texture2D image:register(t0);SamplerState linearSampler:register(s0);"
            "float4 PS(V v):SV_TARGET{float4 c=image.Sample(linearSampler,v.uv);if(alphaMode==0)c.rgb*=c.a;return c;}";
        ComPtr<ID3DBlob> errors;
        if (FAILED(compile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "VS", "vs_5_0",
                           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs_, &errors)) ||
            FAILED(compile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "PS", "ps_5_0",
                           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps_, &errors)))
            return false;
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = {0, 0, 5};
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = {1, &range};
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC desc{2, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        prepared_ =
            SUCCEEDED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_bytes_, &errors));
        return prepared_;
    }

    // Caller owns the product initialization admission and qualified queue.
    bool Initialize(IDXGISwapChain3* chain, ID3D12CommandQueue* queue) noexcept {
        if (!prepared_ || attempted_ || closed_ || !chain || !queue) return false;
        attempted_ = true;
        chain_ = chain;
        ComPtr<ID3D12Device> queue_device;
        ComPtr<IUnknown> a, b;
        if (FAILED(chain_->GetDevice(IID_PPV_ARGS(&device_))) || !device_ ||
            FAILED(queue->GetDevice(IID_PPV_ARGS(&queue_device))) || FAILED(device_.As(&a)) ||
            FAILED(queue_device.As(&b)) || a.Get() != b.Get() || device_->GetNodeCount() != 1 ||
            queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
            queue->GetDesc().NodeMask > 1 || !frames_.Initialize(queue))
            return Stop();
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = probe::D3D12Texture::kMaxDescriptors;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srv_))) ||
            FAILED(device_->CreateRootSignature(0, root_bytes_->GetBufferPointer(),
                                                root_bytes_->GetBufferSize(),
                                                IID_PPV_ARGS(&root_))))
            return Stop();
        bitmap_.texture().Bind(
            device_.Get(), srv_->GetCPUDescriptorHandleForHeapStart(),
            srv_->GetGPUDescriptorHandleForHeapStart(),
            device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV),
            D3d12Slots::kSlots, heap.NumDescriptors);
        return QualifyBuffers();
    }

    // Caller holds the selected-chain owner; no bitmap Acquire, GPU slot,
    // command-list reset or submission is needed to report dimensions.
    void ReportTargetMetadata() noexcept {
        if (closed_ || failed_.load(std::memory_order_acquire) || !chain_ || !width_ || !height_)
            return;
        LARGE_INTEGER stamp{};
        if (!QueryPerformanceCounter(&stamp)) return;
        const auto ticks = static_cast<UINT64>(stamp.QuadPart);
        const auto now = (ticks / qpc_frequency_) * 1'000'000 +
                         (ticks % qpc_frequency_) * 1'000'000 / qpc_frequency_;
        const auto& facts = facts_.Get(chain_.Get(), width_, height_, now);
        bitmap_.ReportTarget(width_, height_, facts.display_width, facts.display_height,
                             facts.scaling, now);
    }

    bool Draw() noexcept {
        if (closed_ || failed_.load(std::memory_order_acquire) || !gate_.TryDraw()) return false;

        struct Scope {
            D3d12LifecycleGate& gate;

            ~Scope() { gate.EndDraw(); }
        } scope{gate_};

        if (!chain_ || !pipeline_) return false;
        ReportTargetMetadata();
        const UINT index = chain_->GetCurrentBackBufferIndex();
        if (index >= buffer_count_) return Stop();
        auto* list = frames_.TryBegin();
        if (!list) {
            if (frames_.Failed()) Stop();
            return false;
        }

        struct Source final : probe::UploadListSource {
            ID3D12GraphicsCommandList* list;
            UINT slot;

            Source(ID3D12GraphicsCommandList* value, UINT index) : list(value), slot(index) {}

            ID3D12GraphicsCommandList* Open() override { return list; }

            UINT Slot() const override { return slot; }
        } source(list, frames_.OpenSlot());

        const UINT64 completed = frames_.Completed();
        if (completed == std::numeric_limits<UINT64>::max()) {
            frames_.Cancel();
            return Stop();
        }
        bitmap_.texture().BeginFrame(&source, completed, frames_.PendingValue());
        LARGE_INTEGER stamp{};
        QueryPerformanceCounter(&stamp);
        const auto ticks = static_cast<UINT64>(stamp.QuadPart);
        const auto now = (ticks / qpc_frequency_) * 1'000'000 +
                         (ticks % qpc_frequency_) * 1'000'000 / qpc_frequency_;
        if (!bitmap_.Acquire(width_, height_, now)) {
            // Acquire may have recorded a valid upload before refusing a later
            // operation. Submit even an empty list so texture state/fence never
            // describes commands that were discarded.
            if (!frames_.Submit()) Stop();
            return false;
        }
        auto barrier = Barrier(buffers_[index].Get(), D3D12_RESOURCE_STATE_PRESENT,
                               D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &barrier);
        auto rtv = rtv_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += index * rtv_stride_;
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(width_), static_cast<float>(height_),
                                      0, 1};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->SetPipelineState(pipeline_.Get());
        list->SetGraphicsRootSignature(root_.Get());
        ID3D12DescriptorHeap* heaps[] = {srv_.Get()};
        list->SetDescriptorHeaps(1, heaps);
        const float rect[] = {
            bitmap_.DestLeft() * 2 / width_ - 1, 1 - bitmap_.DestTop() * 2 / height_,
            bitmap_.DestRight() * 2 / width_ - 1, 1 - bitmap_.DestBottom() * 2 / height_};
        list->SetGraphicsRoot32BitConstants(0, 4, rect, 0);
        list->SetGraphicsRoot32BitConstant(0, bitmap_.AlphaMode(), 4);
        list->SetGraphicsRootDescriptorTable(1, {static_cast<UINT64>(bitmap_.TextureId())});
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        list->DrawInstanced(4, 1, 0, 0);
        barrier = Barrier(buffers_[index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_PRESENT);
        list->ResourceBarrier(1, &barrier);
        if (!frames_.Submit()) return Stop();
        return true;
    }

    bool DrawSolidRectangle() noexcept {
        if (closed_ || failed_.load(std::memory_order_acquire) || !gate_.TryDraw()) return false;

        struct Scope {
            D3d12LifecycleGate& gate;

            ~Scope() { gate.EndDraw(); }
        } scope{gate_};

        if (!chain_ || !rtv_ || width_ <= 32 || height_ <= 32) return false;
        const UINT index = chain_->GetCurrentBackBufferIndex();
        if (index >= buffer_count_) return Stop();
        auto* list = frames_.TryBegin();
        if (!list) {
            if (frames_.Failed()) Stop();
            return false;
        }
        auto barrier = Barrier(buffers_[index].Get(), D3D12_RESOURCE_STATE_PRESENT,
                               D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &barrier);
        auto rtv = rtv_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += index * rtv_stride_;
        const D3D12_RECT rectangle{32, 32, static_cast<LONG>(std::min(width_, 288U)),
                                   static_cast<LONG>(std::min(height_, 160U))};
        constexpr float magenta[] = {1, 0, 1, 1};
        // Explicit one-rectangle clear only; no texture, viewport, shader draw,
        // blend state, bitmap section, metadata request or placement dependency.
        list->ClearRenderTargetView(rtv, magenta, 1, &rectangle);
        barrier = Barrier(buffers_[index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_PRESENT);
        list->ResourceBarrier(1, &barrier);
        if (!frames_.Submit()) return Stop();
        return true;
    }

    bool Failed() const noexcept { return failed_.load(std::memory_order_acquire); }

    bool BeforeResize(DWORD budget = kTransitionWaitMs) noexcept {
        if (closed_ || failed_.load(std::memory_order_acquire) ||
            !gate_.BeginTransition(clock_, budget))
            return Stop();
        transition_owned_ = true;
        if (frames_.DrainForTransition(gate_.Remaining(clock_)) != DrainResult::Complete) {
            Stop();
            gate_.EndTransition(false);
            transition_owned_ = false;
            return false;
        }
        for (auto& buffer : buffers_)
            buffer.Reset();
        rtv_.Reset();
        pipeline_.Reset();
        buffer_count_ = 0;
        return true;
    }

    bool AfterResize(HRESULT result) noexcept {
        if (!transition_owned_) return false;
        const bool qualified = SUCCEEDED(result) && !failed_.load(std::memory_order_acquire) &&
                               QualifyBuffers() && frames_.Resume();
        if (!qualified) Stop();
        gate_.EndTransition(qualified);
        transition_owned_ = false;
        return qualified;
    }

    probe::ConsumerStats& Stats() noexcept { return bitmap_.stats(); }

    DXGI_FORMAT TargetFormat() const noexcept { return format_; }

    const probe::DestRect& Destination() const noexcept { return bitmap_.dest(); }

    std::uint32_t PlacementSerial() const noexcept { return bitmap_.ResearchPlacementSerial(); }

    // Only after exclusive external shutdown admission; installed DLL is never
    // hot-unloaded. Any unknown GPU lifetime retains the complete bounded owner.
    Retirement Close() noexcept {
        if (closed_) return retirement_;
        closed_ = true;
        retirement_ = frames_.Close();
        if (retirement_ == Retirement::Retained) {
            for (auto& buffer : buffers_)
                buffer.Detach();
            rtv_.Detach();
            srv_.Detach();
            root_.Detach();
            pipeline_.Detach();
            device_.Detach();
            chain_.Detach();
            vs_.Detach();
            ps_.Detach();
            root_bytes_.Detach();
            compiler_ = runtime_ = nullptr;
        } else {
            bitmap_.Release();
            for (auto& buffer : buffers_)
                buffer.Reset();
            rtv_.Reset();
            srv_.Reset();
            root_.Reset();
            pipeline_.Reset();
            chain_.Reset();
            device_.Reset();
            vs_.Reset();
            ps_.Reset();
            root_bytes_.Reset();
            if (compiler_) FreeLibrary(compiler_);
            if (runtime_) FreeLibrary(runtime_);
            compiler_ = runtime_ = nullptr;
        }
        return retirement_;
    }

private:
    template <class T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    bool Stop() noexcept {
        failed_.store(true, std::memory_order_release);
        return false;
    }

    static D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES a,
                                          D3D12_RESOURCE_STATES b) noexcept {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
        return barrier;
    }

    bool QualifyBuffers() noexcept {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        if (FAILED(chain_->GetDesc1(&desc)) || desc.Width == 0 || desc.Height == 0 ||
            desc.BufferCount < 2 || desc.BufferCount > buffers_.size() ||
            desc.SampleDesc.Count != 1 || !IsSupportedSdrTarget(desc.Format))
            return Stop();
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = root_.Get();
        pso.VS = {vs_->GetBufferPointer(), vs_->GetBufferSize()};
        pso.PS = {ps_->GetBufferPointer(), ps_->GetBufferSize()};
        auto& blend = pso.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pso.DepthStencilState.StencilReadMask = pso.DepthStencilState.StencilWriteMask = 255;
        pso.DepthStencilState.FrontFace =
            pso.DepthStencilState.BackFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                              D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = desc.Format;
        pso.SampleDesc.Count = 1;
        if (FAILED(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline_))))
            return Stop();
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = desc.BufferCount;
        if (FAILED(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv_)))) return Stop();
        rtv_stride_ = device_->GetDescriptorHandleIncrementSize(heap.Type);
        auto handle = rtv_->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < desc.BufferCount; ++i) {
            if (FAILED(chain_->GetBuffer(i, IID_PPV_ARGS(&buffers_[i])))) return Stop();
            const auto actual = buffers_[i]->GetDesc();
            if (actual.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                !MatchesSdrBuffer(desc.Format, desc.Width, desc.Height, actual.Format, actual.Width,
                                  actual.Height, actual.SampleDesc.Count))
                return Stop();
            device_->CreateRenderTargetView(buffers_[i].Get(), nullptr, handle);
            handle.ptr += rtv_stride_;
        }
        width_ = desc.Width;
        height_ = desc.Height;
        buffer_count_ = desc.BufferCount;
        format_ = desc.Format;
        return true;
    }

    struct Clock {
        UINT64 Now() noexcept { return GetTickCount64(); }

        void Pause() noexcept { Sleep(1); }
    } clock_;

    D3d12LifecycleGate gate_;
    D3d12FrameResources frames_;
    probe::PublishedBitmapT<probe::D3D12Texture> bitmap_;
    probe::TargetFactsCache facts_;
    ComPtr<IDXGISwapChain3> chain_;
    ComPtr<ID3D12Device> device_;
    std::array<ComPtr<ID3D12Resource>, 8> buffers_;
    ComPtr<ID3D12DescriptorHeap> rtv_, srv_;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> pipeline_;
    ComPtr<ID3DBlob> vs_, ps_, root_bytes_;
    HMODULE compiler_ = nullptr, runtime_ = nullptr;
    UINT64 qpc_frequency_ = 0;
    UINT width_ = 0, height_ = 0, buffer_count_ = 0, rtv_stride_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    Retirement retirement_ = Retirement::Released;
    std::atomic<bool> failed_{false};
    bool prepared_ = false, attempted_ = false, closed_ = false, transition_owned_ = false;
};
}
