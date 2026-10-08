#include "request_policy.hpp"
#include <cstdio>
#include <cstdlib>
using namespace gtg::overlay;

void Check(bool ok, const char* why) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", why);
        std::exit(1);
    }
}

int main() {
    ipc::Request r{};
    r.magic = ipc::kRequestMagic;
    r.version = ipc::kRequestVersion;
    r.header_size = sizeof(r);
    r.writer_pid = 42;
    r.serial = r.serial_begin = 1;
    r.swapchain_width = r.display_width = 1920;
    r.swapchain_height = r.display_height = 1080;
    r.heartbeat_us = 100;
    Check(integration::AcceptRequest(r, 1, 1, 42, 200), "fresh coherent request");
    Check(!integration::AcceptRequest(r, 1, 1, 42, 10'000'101), "stale request rejected");
    Check(!integration::AcceptRequest(r, 1, 1, 42, 99), "future heartbeat rejected");
    Check(!integration::AcceptRequest(r, 1, 2, 42, 200), "torn request rejected");
    Check(!integration::AcceptRequest(r, 1, 1, 43, 200), "other process rejected");
    r.swapchain_width = 16385;
    Check(!integration::AcceptRequest(r, 1, 1, 42, 200), "unbounded allocation refused");
    std::puts("PASS reverse request coherence, identity, dimensions and freshness");
}
