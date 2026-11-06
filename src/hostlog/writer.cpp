#include "hostlog/writer.hpp"

#include <windows.h>

#include <format>
#include <random>
#include <string_view>

#include "logging/logger.hpp"

namespace gtg::hostlog {

// What the thread and the Writer both hold, so a thread left behind by a
// bounded Stop keeps everything it uses until it exits.
struct Writer::Shared {
    std::filesystem::path path;
    std::uint64_t rotate_at_bytes{};
    std::string run_id;
    Mailbox<Frame> mailbox;
    HANDLE ready{};   // auto-reset: a frame is in the slot
    HANDLE stop{};    // manual-reset
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> written{0};

    ~Shared() {
        if (ready != nullptr) CloseHandle(ready);
        if (stop != nullptr) CloseHandle(stop);
    }
};

namespace {

constexpr std::uint64_t kRetryAfterMs = 60'000;
constexpr DWORD kStopWaitMs = 2'000;

// The log file, on the writer's thread only. Appends whole lines; renames the
// file to ".1" between lines once the next one would take it past the bound.
// A failure stops writing until a retry a minute later, so a reader holding the
// file without delete sharing cannot make it grow past the bound.
class File {
public:
    File(std::filesystem::path path, const std::uint64_t bound)
        : path_(std::move(path)), bound_(bound) {}

    ~File() { Close(); }

    File(const File&) = delete;
    File& operator=(const File&) = delete;

    [[nodiscard]] bool Append(const std::string_view line) {
        const std::uint64_t now = GetTickCount64();
        if (handle_ == INVALID_HANDLE_VALUE) {
            if (now < retry_after_ms_) return false;
            if (!Open()) return Fail(now, L"opened");
        }
        // A line cut short by a failed write is ended before the next one, so
        // a reader loses that line and no other.
        const std::size_t ending = broken_ ? 1 : 0;
        if (size_ > 0 && size_ + ending + line.size() > bound_) {
            if (!Rotate()) return Fail(now, L"rotated");
        } else if (broken_ && !Write("\n")) {
            return Fail(now, L"written");
        }
        broken_ = false;
        if (!Write(line)) {
            broken_ = true;
            return Fail(now, L"written");
        }
        if (failing_) {
            failing_ = false;
            (void)logging::TryInfo(L"hosts log: writing again");
        }
        return true;
    }

    void Close() noexcept {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }

private:
    bool Open() {
        handle_ = CreateFileW(path_.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        size_ = GetFileSizeEx(handle_, &size) ? static_cast<std::uint64_t>(size.QuadPart) : 0;
        return true;
    }

    bool Rotate() {
        Close();
        const std::wstring previous = path_.wstring() + L".1";
        if (MoveFileExW(path_.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING) == FALSE) {
            return false;
        }
        broken_ = false;
        return Open();
    }

    bool Write(const std::string_view bytes) {
        DWORD written = 0;
        const bool ok = WriteFile(handle_, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                                  nullptr) != FALSE;
        size_ += written;
        return ok && written == bytes.size();
    }

    // Journalled once per streak of failures: the rare-event use of the
    // deferred queue that every Tray thread makes.
    bool Fail(const std::uint64_t now, const wchar_t* what) {
        const DWORD error = GetLastError();
        Close();
        retry_after_ms_ = now + kRetryAfterMs;
        if (!failing_) {
            failing_ = true;
            (void)logging::TryWarning(
                std::format(L"hosts log: {} could not be {} (Win32 {}); retrying in 60 s",
                            path_.filename().wstring(), what, error));
        }
        return false;
    }

    std::filesystem::path path_;
    std::uint64_t bound_{};
    HANDLE handle_{INVALID_HANDLE_VALUE};
    std::uint64_t size_{};
    std::uint64_t retry_after_ms_{};
    bool broken_{};
    bool failing_{};
};

void Run(const std::shared_ptr<Writer::Shared> shared) noexcept {
    // Lowest CPU and I/O priority: this is the least important work GTG does.
    (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    try {
        File file(shared->path, shared->rotate_at_bytes);
        const HANDLE handles[]{shared->stop, shared->ready};
        // The stop event comes first, so a stop and a frame together stop.
        while (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
            auto frame = shared->mailbox.Take();
            if (!frame) continue;
            if (file.Append(FormatLine(*frame, shared->run_id))) {
                shared->written.fetch_add(1, std::memory_order_relaxed);
            } else {
                shared->dropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    } catch (...) {
        // Out of memory: the log stops, nothing else does.
        (void)logging::TryError(L"hosts log: the writer ended after an unexpected failure");
    }
    (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
}

std::string NewRunId() {
    // rand_s underneath: the system's random number generator.
    std::random_device random;
    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        const unsigned int word = random();
        for (std::size_t j = 0; j < 4; ++j)
            bytes[i + j] = static_cast<std::uint8_t>(word >> (8 * j));
    }
    return FormatRunId(bytes);
}

}  // namespace

bool Writer::Start(Options options) {
    if (Running()) return true;
    try {
        auto shared = std::make_shared<Shared>();
        shared->ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        shared->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (shared->ready == nullptr || shared->stop == nullptr) return false;
        shared->path = std::move(options.file);
        shared->rotate_at_bytes = options.rotate_at_bytes;
        shared->run_id = NewRunId();
        thread_ = std::thread(Run, shared);
        run_id_ = shared->run_id;
        seq_ = 0;
        shared_ = std::move(shared);
        return true;
    } catch (...) {
        return false;
    }
}

void Writer::Stop() noexcept {
    if (!thread_.joinable()) return;
    (void)SetEvent(shared_->stop);
    if (WaitForSingleObject(thread_.native_handle(), kStopWaitMs) == WAIT_OBJECT_0) {
        thread_.join();
    } else {
        thread_.detach();
    }
    shared_.reset();
}

bool Writer::Submit(Frame frame) {
    if (!shared_) return false;
    frame.seq = ++seq_;
    frame.dropped = shared_->dropped.load(std::memory_order_relaxed);
    switch (shared_->mailbox.TryPut(std::move(frame))) {
        case Mailbox<Frame>::Put::Busy:
            shared_->dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        case Mailbox<Frame>::Put::Replaced:
            shared_->dropped.fetch_add(1, std::memory_order_relaxed);
            break;
        case Mailbox<Frame>::Put::Taken:
            break;
    }
    (void)SetEvent(shared_->ready);
    return true;
}

std::uint64_t Writer::Dropped() const noexcept {
    return shared_ ? shared_->dropped.load(std::memory_order_relaxed) : 0;
}

std::uint64_t Writer::Written() const noexcept {
    return shared_ ? shared_->written.load(std::memory_order_relaxed) : 0;
}

}  // namespace gtg::hostlog
