#pragma once
#include <windows.h>
#include <cwchar>
#include <stdexcept>
#include "../ipc/osd_section.hpp"

namespace gtg::research {
inline void BitmapSectionName(DWORD pid, wchar_t (&name)[96]) {
    swprintf_s(name, L"Local\\GTG.Research.Bitmap.%lu", pid);
}

inline void BitmapRequestName(DWORD pid, wchar_t (&name)[96]) {
    swprintf_s(name, L"Local\\GTG.Research.Request.%lu", pid);
}

// Parent-owned synthetic publisher. Never opens the desktop publisher section.
class FixturePublisher {
public:
    FixturePublisher() = default;

    ~FixturePublisher() {
        if (request_) UnmapViewOfFile(request_);
        if (request_mapping_) CloseHandle(request_mapping_);
        if (section_) UnmapViewOfFile(section_);
        if (mapping_) CloseHandle(mapping_);
    }

    FixturePublisher(const FixturePublisher&) = delete;
    FixturePublisher& operator=(const FixturePublisher&) = delete;

    void Start(DWORD pid, bool enabled, bool request = false) {
        namespace ipc = gtg::overlay::ipc;
        wchar_t name[96]{};
        BitmapSectionName(pid, name);
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      static_cast<DWORD>(ipc::kSectionBytes), name);
        if (!mapping_ || GetLastError() == ERROR_ALREADY_EXISTS)
            throw std::runtime_error("unique fixture bitmap section");
        section_ = static_cast<ipc::Section*>(
            MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, ipc::kSectionBytes));
        if (!section_) throw std::runtime_error("map fixture bitmap");
        section_->magic = ipc::kMagic;
        section_->version = ipc::kVersion;
        section_->header_size = sizeof(ipc::Section);
        section_->writer_pid = GetCurrentProcessId();
        if (request) {
            BitmapRequestName(pid, name);
            request_mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                                  static_cast<DWORD>(ipc::kRequestBytes), name);
            if (!request_mapping_ || GetLastError() == ERROR_ALREADY_EXISTS)
                throw std::runtime_error("unique fixture request section");
            request_ = static_cast<ipc::Request*>(
                MapViewOfFile(request_mapping_, FILE_MAP_ALL_ACCESS, 0, 0, ipc::kRequestBytes));
            if (!request_) throw std::runtime_error("map fixture request");
            request_->magic = ipc::kRequestMagic;
            request_->version = ipc::kRequestVersion;
            request_->header_size = sizeof(ipc::Request);
        }
        auto& frame = section_->buffer[0];
        frame.header.width = frame.header.height = 16;
        frame.header.stride = 64;
        frame.header.byte_count = 1024;
        frame.header.offset_dip_x = frame.header.offset_dip_y = 32;
        frame.header.frame_width = 640;
        frame.header.frame_height = 360;
        ipc::BeginFrame(frame, 1);
        for (unsigned i = 0; i < frame.header.byte_count; i += 4) {
            frame.pixels[i] = 170;
            frame.pixels[i + 1] = 0;
            frame.pixels[i + 2] = 255;
            frame.pixels[i + 3] = 255;
        }
        ipc::FinishFrame(frame, 1);
        ipc::Publish(*section_, 0);
        ipc::SetEnabled(*section_, enabled);
        Beat();
    }

    bool RequestReceived(DWORD pid, unsigned width, unsigned height) const {
        return request_ && gtg::overlay::ipc::RequestSerial(*request_) > 0 &&
               gtg::overlay::ipc::RequestSerialBegun(*request_) ==
                   gtg::overlay::ipc::RequestSerial(*request_) &&
               request_->writer_pid == pid && request_->swapchain_width == width &&
               request_->swapchain_height == height;
    }

    void Beat() {
        if (!section_) return;
        LARGE_INTEGER now{}, frequency{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        std::atomic_ref<std::uint64_t>(section_->heartbeat_us)
            .store(static_cast<std::uint64_t>(now.QuadPart) * 1'000'000ULL / frequency.QuadPart,
                   std::memory_order_release);
    }

    // Bounded test schedule, independent clocks for metadata and content.
    // No catch-up burst after scheduling delays; the final dwell verifies idle.
    void TickMotion(bool content, unsigned frame_width = 640, unsigned frame_height = 360) {
        namespace ipc = gtg::overlay::ipc;
        const auto now = GetTickCount64();
        if (!motion_started_) {
            motion_started_ = true;
            last_placement_ms_ = last_content_ms_ = now;
        }
        if (placement_count_ < 40 && now - last_placement_ms_ >= 100) {
            last_placement_ms_ = now;
            ++placement_count_;
            ipc::BeginPlacement(*section_, placement_count_);
            auto& p = section_->placement;
            p.left = placement_count_ % 2 ? 32 : 0;
            p.top = 32;
            p.width = p.height = 16;
            p.frame_width = frame_width;
            p.frame_height = frame_height;
            ipc::FinishPlacement(*section_, placement_count_);
        }
        if (content && content_count_ < 10 && now - last_content_ms_ >= 500) {
            last_content_ms_ = now;
            const auto serial = ++content_count_ + 1;
            const auto index = 1 - ipc::PublishedIndex(*section_);
            auto& frame = section_->buffer[index];
            ipc::BeginFrame(frame, serial);
            // Dimensions stay fixed; pure placement never reaches this branch.
            frame.header.width = frame.header.height = 16;
            frame.header.stride = 64;
            frame.header.byte_count = 1024;
            frame.header.offset_dip_x = frame.header.offset_dip_y = 32;
            frame.header.frame_width = 640;
            frame.header.frame_height = 360;
            for (unsigned i = 0; i < frame.header.byte_count; i += 4) {
                frame.pixels[i] = serial % 2 ? 170 : 85;
                frame.pixels[i + 1] = 0;
                frame.pixels[i + 2] = frame.pixels[i + 3] = 255;
            }
            ipc::FinishFrame(frame, serial);
            ipc::Publish(*section_, index);
        }
    }

    bool MotionFinished(bool content) const {
        return placement_count_ == 40 && content_count_ == (content ? 10U : 0U) &&
               GetTickCount64() - last_placement_ms_ >= 1000 &&
               (!content || GetTickCount64() - last_content_ms_ >= 1000) &&
               gtg::overlay::ipc::PlacementSerial(*section_) == 40;
    }

    void PublishControlFrame(unsigned serial, unsigned size, unsigned frame_width,
                             unsigned frame_height) {
        namespace ipc = gtg::overlay::ipc;
        const auto index = 1 - ipc::PublishedIndex(*section_);
        auto& frame = section_->buffer[index];
        ipc::BeginFrame(frame, serial);
        frame.header.width = frame.header.height = size;
        frame.header.stride = size * 4;
        frame.header.byte_count = size * size * 4;
        frame.header.offset_dip_x = frame.header.offset_dip_y = 32;
        frame.header.frame_width = frame_width;
        frame.header.frame_height = frame_height;
        for (unsigned i = 0; i < frame.header.byte_count; i += 4) {
            frame.pixels[i] = serial % 2 ? 170 : 85;
            frame.pixels[i + 1] = 0;
            frame.pixels[i + 2] = frame.pixels[i + 3] = 255;
        }
        ipc::FinishFrame(frame, serial);
        ipc::Publish(*section_, index);
    }

    void ControlPlacement(unsigned serial, unsigned size) {
        namespace ipc = gtg::overlay::ipc;
        ipc::BeginPlacement(*section_, serial);
        auto& p = section_->placement;
        p.left = 0;
        p.top = 32;
        p.width = p.height = static_cast<std::int32_t>(size);
        p.frame_width = 640;
        p.frame_height = 360;
        ipc::FinishPlacement(*section_, serial);
    }

    void TickControl(int mode, LONG resized_hidden_samples, volatile LONG& epoch) {
        namespace ipc = gtg::overlay::ipc;
        const auto now = GetTickCount64();
        if (!control_start_) control_start_ = now;
        const auto elapsed = now - control_start_;
        if (mode == 1) {
            const LONG phase = elapsed < 1500 ? 0 : elapsed < 3500 ? 1 : 2;
            if (InterlockedCompareExchange(&epoch, 0, 0) != phase) {
                InterlockedExchange(&epoch, -1);
                ipc::SetEnabled(*section_, phase != 1);
                InterlockedExchange(&epoch, phase);
            }
        } else if (mode == 2 && !control_step_ && resized_hidden_samples >= 12) {
            PublishControlFrame(2, 16, 320, 180);
            control_step_ = 1;
        } else if (mode == 3) {
            if (control_step_ == 0 && elapsed >= 1000) {
                ControlPlacement(1, 24);
                ++control_step_;
            }
            if (control_step_ == 1 && elapsed >= 1500) {
                PublishControlFrame(2, 24, 640, 360);
                ++control_step_;
            }
            if (control_step_ == 2 && elapsed >= 3000) {
                ControlPlacement(2, 16);
                ++control_step_;
            }
            if (control_step_ == 3 && elapsed >= 3500) {
                PublishControlFrame(3, 16, 640, 360);
                ++control_step_;
            }
        }
    }

private:
    ULONGLONG control_start_{};
    unsigned control_step_{};
    bool motion_started_{};
    ULONGLONG last_placement_ms_{}, last_content_ms_{};
    std::uint32_t placement_count_{}, content_count_{};
    HANDLE mapping_{};
    gtg::overlay::ipc::Section* section_{};
    HANDLE request_mapping_{};
    gtg::overlay::ipc::Request* request_{};
};
}  // namespace gtg::research
