#pragma once
#include "tray/osd_compact.hpp"
#include "../ipc/osd_section.hpp"
#include <algorithm>
#include <cmath>
namespace gtg::overlay::integration {
inline float BoundRenderScale(float scale, bool collapsed) noexcept {
    // ChooseFootprint cannot exceed these bounds for any valid arrangement,
    // including all optional records. Cap before GDI+ allocates its bitmap.
    namespace compact = tray::compact;
    constexpr int width = compact::CollapsedWidth(compact::kRecordCount);
    constexpr int height = compact::kCollapsedSingleRowHeight +
                           (compact::kRecordCount - 1) * compact::kCollapsedRowHeightIncrement;
    constexpr int expanded_height =
        compact::Height(false) + (compact::kRecordCount - compact::kCellsPerRow) * 25;
    if (!std::isfinite(scale) || scale <= 0) return 0;
    return std::min({scale, static_cast<float>(ipc::kMaxWidth) / (collapsed ? width : 388),
                     static_cast<float>(ipc::kMaxHeight) / (collapsed ? height : expanded_height)});
}
}  // namespace gtg::overlay::integration
