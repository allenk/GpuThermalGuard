#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <vector>
#include "d3d12_bitmap_renderer.hpp"
#include "../injector/renderer_fixture.hpp"
using Microsoft::WRL::ComPtr;
using namespace gtg::overlay::integration;

namespace {
void Check(bool value, const char* text) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", text);
        ExitProcess(1);
    }
}

void Hr(HRESULT value, const char* text) {
    Check(SUCCEEDED(value), text);
}

struct OwnedGpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 value{};

    void Begin() {
        Hr(allocator->Reset(), "oracle allocator");
        Hr(list->Reset(allocator.Get(), nullptr), "oracle list");
    }

    void Finish() {
        Hr(list->Close(), "oracle close");
        ID3D12CommandList* commands = list.Get();
        queue->ExecuteCommandLists(1, &commands);
        Hr(queue->Signal(fence.Get(), ++value), "oracle signal");
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(event != nullptr, "oracle event");
        Hr(fence->SetEventOnCompletion(value, event), "oracle notification");
        Check(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0 &&
                  fence->GetCompletedValue() == value,
              "oracle completion");
        CloseHandle(event);
    }

    static D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES a,
                                          D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
        return barrier;
    }

    void Clear(IDXGISwapChain3* chain) {
        ComPtr<ID3D12Resource> buffer;
        Hr(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer)),
           "oracle buffer");
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> heap;
        Hr(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)), "oracle RTV");
        auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(buffer.Get(), nullptr, rtv);
        Begin();
        auto barrier =
            Barrier(buffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &barrier);
        constexpr float black[] = {0, 0, 0, 1};
        list->ClearRenderTargetView(rtv, black, 0, nullptr);
        barrier =
            Barrier(buffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        list->ResourceBarrier(1, &barrier);
        Finish();
    }

    void Pixel(IDXGISwapChain3* chain, UINT x, UINT y, bool colored, unsigned red = 255,
               unsigned blue = 170) {
        ComPtr<ID3D12Resource> buffer;
        Hr(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer)),
           "readback buffer");
        auto desc = buffer->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 bytes{};
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        D3D12_RESOURCE_DESC read{};
        read.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        read.Width = bytes;
        read.Height = read.DepthOrArraySize = read.MipLevels = 1;
        read.SampleDesc.Count = 1;
        read.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES props{};
        props.Type = D3D12_HEAP_TYPE_READBACK;
        ComPtr<ID3D12Resource> result;
        Hr(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &read,
                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                           IID_PPV_ARGS(&result)),
           "readback allocation");
        Begin();
        auto barrier =
            Barrier(buffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = buffer.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = result.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = footprint;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        barrier =
            Barrier(buffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        list->ResourceBarrier(1, &barrier);
        Finish();
        void* mapped{};
        Hr(result->Map(0, nullptr, &mapped), "readback map");
        const auto* pixel = static_cast<unsigned char*>(mapped) + footprint.Offset +
                            y * footprint.Footprint.RowPitch + x * 4;
        std::printf("pixel %u,%u = %u,%u,%u,%u\n", x, y, pixel[0], pixel[1], pixel[2], pixel[3]);
        if (desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
            UINT packed{};
            std::memcpy(&packed, pixel, sizeof(packed));
            const unsigned r = packed & 1023, g = (packed >> 10) & 1023, b = (packed >> 20) & 1023;
            const auto expected = [](unsigned value) {
                return (value * 1023 + 127) / 255;
            };
            const auto within_quantization = [](unsigned value, unsigned target) {
                return value + 1 >= target && value <= target + 1;
            };
            Check(colored ? within_quantization(r, expected(red)) && g == 0 &&
                                within_quantization(b, expected(blue))
                          : r == 0 && g == 0 && b == 0,
                  "independent packed 10-bit bitmap/movement pixel oracle");
        } else {
            const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
            Check(colored
                      ? pixel[bgra ? 2 : 0] == red && pixel[1] == 0 && pixel[bgra ? 0 : 2] == blue
                      : pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0,
                  "independent bitmap/movement pixel oracle");
        }
        result->Unmap(0, nullptr);
    }
};
}

void RunFormat(DXGI_FORMAT format) {
    ComPtr<ID3D12Debug> debug;
    Hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "debug layer");
    debug->EnableDebugLayer();
    HWND window = CreateWindowExW(0, L"STATIC", L"GTG owned product bitmap test", WS_POPUP, 0, 0,
                                  640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "owned hidden window");
    ComPtr<IDXGIFactory4> factory;
    Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    ComPtr<IDXGIAdapter> warp;
    Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP");
    OwnedGpu gpu;
    Hr(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&gpu.device)), "device");
    D3D12_COMMAND_QUEUE_DESC q{};
    Hr(gpu.device->CreateCommandQueue(&q, IID_PPV_ARGS(&gpu.queue)), "queue");
    Hr(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          IID_PPV_ARGS(&gpu.allocator)),
       "allocator");
    Hr(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(),
                                     nullptr, IID_PPV_ARGS(&gpu.list)),
       "list");
    Hr(gpu.list->Close(), "initial close");
    Hr(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence)), "fence");
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = 640;
    desc.Height = 360;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.BufferCount = 2;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> base;
    Hr(factory->CreateSwapChainForHwnd(gpu.queue.Get(), window, &desc, nullptr, nullptr, &base),
       "chain");
    ComPtr<IDXGISwapChain3> chain;
    Hr(base.As(&chain), "chain3");
    gtg::research::FixturePublisher publisher;
    D3d12BitmapRenderer renderer;
    Check(renderer.Prepare(), "prepare shaders before hook installation");
    Check(renderer.Initialize(chain.Get(), gpu.queue.Get()), "qualified private bitmap owner");
    // No publisher/request mapping exists yet. The square cannot depend on it.
    gpu.Clear(chain.Get());
    Check(renderer.DrawSolidRectangle(), "solid rectangle without IPC publisher");
    gpu.Pixel(chain.Get(), 40, 40, true, 255, 255);
    gpu.Pixel(chain.Get(), 300, 40, false);
    gpu.Pixel(chain.Get(), 40, 200, false);
    Check(renderer.Stats().uploads == 0 && renderer.Stats().copy_attempts == 0 &&
              renderer.Stats().requests_written == 0,
          "solid rectangle has no requests, pixel copies or uploads");
    publisher.Start(GetCurrentProcessId(), true, true);
    renderer.ReportTargetMetadata();
    Check(publisher.RequestReceived(GetCurrentProcessId(), 640, 360) &&
              renderer.Stats().uploads == 0 && renderer.Stats().copy_attempts == 0,
          "metadata-only handshake performs no IPC pixel upload");
    gpu.Clear(chain.Get());
    Check(renderer.Draw(), "first actual IPC bitmap draw");
    gpu.Pixel(chain.Get(), 40, 40, true);
    Check(
        renderer.Stats().uploads == 1 && publisher.RequestReceived(GetCurrentProcessId(), 640, 360),
        "one upload and reverse request");
    publisher.ControlPlacement(1, 16);
    publisher.Beat();
    gpu.Clear(chain.Get());
    Check(renderer.Draw(), "cached bitmap movement");
    gpu.Pixel(chain.Get(), 8, 40, true);
    gpu.Pixel(chain.Get(), 40, 40, false);
    Check(renderer.Stats().uploads == 1 && renderer.Stats().reused == 1,
          "movement needs no bitmap upload");
    // Untrusted enum values must not reach the texture or cached-image path.
    wchar_t bitmap_name[96]{};
    gtg::research::BitmapSectionName(GetCurrentProcessId(), bitmap_name);
    HANDLE bitmap_mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, bitmap_name);
    auto* bitmap = static_cast<gtg::overlay::ipc::Section*>(
        MapViewOfFile(bitmap_mapping, FILE_MAP_ALL_ACCESS, 0, 0, gtg::overlay::ipc::kSectionBytes));
    Check(bitmap != nullptr, "owned adversarial header view");
    auto& published = bitmap->buffer[gtg::overlay::ipc::PublishedIndex(*bitmap)];
    published.header.format = 99;
    Check(!renderer.Draw(), "unknown pixel enum refuses cached bitmap");
    published.header.format = 0;
    published.header.alpha_mode = 99;
    Check(!renderer.Draw(), "unknown alpha enum refuses cached bitmap");
    published.header.alpha_mode = 0;
    gtg::overlay::ipc::BeginFrame(published, 2);
    for (unsigned i = 3; i < published.header.byte_count; i += 4)
        published.pixels[i] = 128;
    gtg::overlay::ipc::FinishFrame(published, 2);
    gpu.Clear(chain.Get());
    Check(renderer.Draw(), "straight alpha conversion");
    gpu.Pixel(chain.Get(), 8, 40, true, 128, 85);
    publisher.Beat();
    std::atomic_ref<std::uint64_t>(bitmap->heartbeat_us).fetch_sub(4'000'000);
    Check(!renderer.Draw(), "stale publisher hides cached bitmap");
    publisher.Beat();
    gtg::overlay::ipc::SetEnabled(*bitmap, false);
    Check(!renderer.Draw(), "disabled publisher hides cached bitmap");
    gtg::overlay::ipc::SetEnabled(*bitmap, true);
    UnmapViewOfFile(bitmap);
    CloseHandle(bitmap_mapping);
    Check(renderer.BeforeResize(), "drain and release complete resource generation");
    const auto resized_format = format == DXGI_FORMAT_R10G10B10A2_UNORM
                                    ? DXGI_FORMAT_R8G8B8A8_UNORM
                                    : DXGI_FORMAT_R10G10B10A2_UNORM;
    const HRESULT resized = chain->ResizeBuffers(0, 320, 180, resized_format, 0);
    Hr(resized, "native resize after product reference release");
    Check(renderer.AfterResize(resized), "requalify generation");
    Check(renderer.TargetFormat() == resized_format,
          "admission uses rebuilt renderer format, not stale discovery desc");
    Check(HasRequiredSdrEvidence(renderer.TargetFormat(), false) ==
              (resized_format != DXGI_FORMAT_R10G10B10A2_UNORM),
          "8-to-10 resize cannot inherit legacy 8-bit color admission");
    publisher.PublishControlFrame(3, 16, 320, 180);
    publisher.Beat();
    gpu.Clear(chain.Get());
    Check(renderer.Draw(), "bitmap draw after resize");
    gpu.Pixel(chain.Get(), 40, 40, true);
    Check(renderer.BeforeResize(), "final owned completion");
    Check(renderer.Close() == Retirement::Released, "all resources released with proof");
    ComPtr<ID3D12InfoQueue> messages;
    Hr(gpu.device.As(&messages), "debug oracle");
    for (UINT64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T bytes{};
        Hr(messages->GetMessage(i, nullptr, &bytes), "message size");
        std::vector<unsigned char> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Hr(messages->GetMessage(i, message, &bytes), "message");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
            std::fprintf(stderr, "%s\n", message->pDescription);
            Check(false, "no debug warnings/errors");
        }
    }
    chain.Reset();
    base.Reset();
    DestroyWindow(window);
    std::puts("PASS product DX12 IPC bitmap pixels, cached movement and resize");
}

int main() {
    Check(MatchesSdrBuffer(DXGI_FORMAT_R8G8B8A8_UNORM, 640, 360, DXGI_FORMAT_R8G8B8A8_UNORM, 640,
                           360, 1),
          "matching native buffer accepted");
    Check(!MatchesSdrBuffer(DXGI_FORMAT_R8G8B8A8_UNORM, 640, 360, DXGI_FORMAT_B8G8R8A8_UNORM, 640,
                            360, 1),
          "packing mismatch refused");
    Check(!MatchesSdrBuffer(DXGI_FORMAT_R10G10B10A2_UNORM, 640, 360, DXGI_FORMAT_R10G10B10A2_UNORM,
                            320, 360, 1),
          "stale generation dimensions refused");
    Check(!MatchesSdrBuffer(DXGI_FORMAT_R8G8B8A8_UNORM, 640, 360, DXGI_FORMAT_R8G8B8A8_UNORM, 640,
                            360, 4),
          "multisample target refused");
    Check(!IsSupportedSdrTarget(DXGI_FORMAT_R16G16B16A16_FLOAT), "FP16 is not implicitly SDR");
    Check(!IsSupportedSdrTarget(DXGI_FORMAT_UNKNOWN), "unknown render target format");
    Check(!HasRequiredSdrEvidence(DXGI_FORMAT_R10G10B10A2_UNORM, false),
          "unknown 10-bit color space draws nothing");
    Check(HasRequiredSdrEvidence(DXGI_FORMAT_R10G10B10A2_UNORM, true),
          "observed SDR permits 10-bit");
    Check(!HasRequiredSdrEvidence(DXGI_FORMAT_R16G16B16A16_FLOAT, true),
          "color evidence does not admit unsupported packing");
    RunFormat(DXGI_FORMAT_R8G8B8A8_UNORM);
    RunFormat(DXGI_FORMAT_B8G8R8A8_UNORM);
    RunFormat(DXGI_FORMAT_R10G10B10A2_UNORM);
}
