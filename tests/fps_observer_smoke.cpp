#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "fps/dxgi_observer.hpp"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: gtg_fps_observer_smoke <pid> <seconds>\n";
        return 2;
    }
    const auto pid = static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10));
    const auto seconds = static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10));
    const auto identity = gtg::fps::ProcessIdentity(pid);
    if (!identity || seconds == 0 || seconds > 30) {
        std::cerr << "target identity unavailable or duration out of range\n";
        return 2;
    }
    gtg::fps::DxgiObserver observer;
    observer.SetTarget(identity);
    unsigned ready = 0;
    for (unsigned i = 0; i < seconds * 4; ++i) {
        Sleep(250);
        const auto snapshot = observer.TryRead();
        if (snapshot.status == gtg::fps::Status::Ready) {
            ++ready;
            std::cout << "fps=" << snapshot.displayed_fps
                      << " surface=" << snapshot.surface << '\n';
        } else {
            std::cout << "status=" << static_cast<int>(snapshot.status) << '\n';
        }
    }
    observer.SetTarget(std::nullopt);
    observer.Stop();
    return ready != 0 ? 0 : 1;
}
