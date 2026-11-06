#include "render_policy.hpp"
#include "tray/osd_tiny.hpp"
#include <cstdio>
#include <limits>

int main() {
    namespace c = gtg::tray::compact;
    namespace ipc = gtg::overlay::ipc;
    using gtg::overlay::integration::BoundRenderScale;
    if (BoundRenderScale(-1, true) ||
        BoundRenderScale(std::numeric_limits<float>::infinity(), false) ||
        BoundRenderScale(std::numeric_limits<float>::quiet_NaN(), true))
        return 1;
    unsigned checked{};
    for (unsigned breaks = 0; breaks < (1U << c::kMaxBreaks); ++breaks) {
        c::Layout layout;
        layout.breaks = static_cast<std::uint8_t>(breaks);
        // RAM, FPS, network and Hosts: every combination, nine records at most.
        for (unsigned optional = 0; optional < 16; ++optional) {
            const auto placed = c::Resolve(layout, (optional & 1) != 0, (optional & 2) != 0,
                                           (optional & 4) != 0, (optional & 8) != 0);
            for (bool compact : {false, true}) {
                for (int available : {1, 96, 320, 388, 16384}) {
                    for (float requested : {0.5F, 1.0F, 2.0F, 2.5F, 100.0F}) {
                        const auto scale = BoundRenderScale(requested, compact);
                        const auto footprint =
                            c::ChooseFootprint(compact, placed, available, available);
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
    // Tiny is a compact form and must fit the compact bound too.
    for (float requested : {0.5F, 1.0F, 2.0F, 2.5F, 100.0F}) {
        const auto scale = BoundRenderScale(requested, true);
        const auto footprint = gtg::tray::tiny::Footprint();
        if (c::ToPixels(static_cast<float>(footprint.width), scale) > ipc::kMaxWidth ||
            c::ToPixels(static_cast<float>(footprint.height), scale) > ipc::kMaxHeight) {
            std::fprintf(stderr, "FAIL Tiny allocation bound\n");
            return 1;
        }
    }
    std::printf("PASS %u layout/scale allocation bounds\n", checked);

    // The game's copy is drawn on its cadence, and at once on a change of
    // shape, so an image is never placed by another shape's anchor.
    using gtg::overlay::integration::RenderDue;
    static_assert(RenderDue(0, 1'000, 0, 0, 388, 330), "the first tick draws");
    static_assert(!RenderDue(1'000, 1'499, 388, 330, 388, 330), "no redraw inside the cadence");
    static_assert(RenderDue(1'000, 1'500, 388, 330, 388, 330), "the cadence redraws");
    static_assert(RenderDue(1'000, 999, 388, 330, 388, 330), "a clock going back redraws");
    static_assert(RenderDue(1'000, 1'100, 388, 330, 84, 101), "expanded to Tiny draws at once");
    static_assert(RenderDue(1'000, 1'100, 540, 88, 540, 143), "a new row draws at once");
    static_assert(RenderDue(1'000, 1'100, 84, 101, 540, 88), "unfolding Tiny draws at once");
    std::puts("PASS the game's copy redraws on its cadence and at once on a change of shape");
}
