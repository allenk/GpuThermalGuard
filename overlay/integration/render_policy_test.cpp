#include "render_policy.hpp"
#include <cstdio>
#include <limits>
int main() {
    namespace c = gtg::tray::compact;
    namespace ipc = gtg::overlay::ipc;
    using gtg::overlay::integration::BoundRenderScale;
    if (BoundRenderScale(-1, true) || BoundRenderScale(std::numeric_limits<float>::infinity(), false) ||
        BoundRenderScale(std::numeric_limits<float>::quiet_NaN(), true)) return 1;
    unsigned checked{};
    for (unsigned breaks = 0; breaks < (1U << c::kMaxBreaks); ++breaks) {
        c::Layout layout;
        layout.breaks = static_cast<std::uint8_t>(breaks);
        for (unsigned optional = 0; optional < 8; ++optional) {
            const auto placed = c::Resolve(layout, (optional & 1) != 0, (optional & 2) != 0,
                                          (optional & 4) != 0);
            for (bool compact : {false, true}) {
                for (int available : {1, 96, 320, 388, 16384}) {
                    for (float requested : {0.5F, 1.0F, 2.0F, 2.5F, 100.0F}) {
                        const auto scale = BoundRenderScale(requested, compact);
                        const auto footprint = c::ChooseFootprint(compact, placed, available, available);
                        const int width = c::ToPixels(static_cast<float>(footprint.width), scale);
                        const int height = c::ToPixels(static_cast<float>(footprint.height), scale);
                        if (width > ipc::kMaxWidth || height > ipc::kMaxHeight) {
                            std::fprintf(stderr, "FAIL allocation bound %dx%d\n", width, height);
                            return 1;
                        }
                        ++checked;
                    }
                }
            }
        }
    }
    std::printf("PASS %u layout/scale allocation bounds\n", checked);
}
