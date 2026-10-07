#include "tray/ui_theme.hpp"

#include <Darkmodelib.h>

#include <array>
#include <utility>

namespace gtg::tray::theme {
namespace {

bool initialized = false;
settings::Appearance current = settings::Appearance::Classic;

bool HighContrast() noexcept {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) != FALSE &&
           (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

COLORREF Mix(const COLORREF a, const COLORREF b, const int b_percent) noexcept {
    const auto channel = [&](const int ca, const int cb) {
        return static_cast<BYTE>((ca * (100 - b_percent) + cb * b_percent) / 100);
    };
    return RGB(channel(GetRValue(a), GetRValue(b)), channel(GetGValue(a), GetGValue(b)),
               channel(GetBValue(a), GetBValue(b)));
}

}  // namespace

void Initialize() noexcept {
    if (initialized) return;
    dmlib::initDarkMode();
    initialized = true;
}

settings::Appearance Configure(const settings::UiTheme preference) noexcept {
    Initialize();
    current = settings::ResolveAppearance(preference, dmlib::isDarkModeReg(),
                                          dmlib::isExperimentalSupported(), HighContrast());
    dmlib::setDarkModeConfigEx(static_cast<UINT>(current == settings::Appearance::Dark
                                                      ? dmlib::DarkModeType::dark
                                                      : dmlib::DarkModeType::classic));
    dmlib::setDefaultColors(true);
    return current;
}

settings::Appearance Current() noexcept { return current; }

void AttachDialog(const HWND dialog) noexcept {
    if (!initialized) return;
    // The setting-change subclass is left off: MainDialog decides what a
    // change means, because an explicit Dark or Light must not follow it.
    dmlib::setDarkWndNotifySafeEx(dialog, false, true);
}

void Repaint(const HWND dialog) noexcept {
    if (!initialized) return;
    dmlib::setDarkTitleBarEx(dialog, true);
    dmlib::setChildCtrlsTheme(dialog);
    RedrawWindow(dialog, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_FRAME);
}

COLORREF DialogBackground() noexcept {
    return IsDark() ? dmlib::getDlgBackgroundColor() : GetSysColor(COLOR_BTNFACE);
}

HBRUSH DialogBackgroundBrush() noexcept {
    return IsDark() ? dmlib::getDlgBackgroundBrush() : GetSysColorBrush(COLOR_BTNFACE);
}

COLORREF ControlBackground() noexcept {
    return IsDark() ? dmlib::getCtrlBackgroundColor() : GetSysColor(COLOR_BTNFACE);
}

COLORREF HotBackground() noexcept {
    return IsDark() ? dmlib::getHotBackgroundColor() : GetSysColor(COLOR_3DSHADOW);
}

COLORREF Edge() noexcept {
    return IsDark() ? dmlib::getEdgeColor() : GetSysColor(COLOR_BTNSHADOW);
}

COLORREF Text() noexcept {
    return IsDark() ? dmlib::getTextColor() : GetSysColor(COLOR_BTNTEXT);
}

COLORREF DisabledText() noexcept {
    return IsDark() ? dmlib::getDisabledTextColor() : GetSysColor(COLOR_GRAYTEXT);
}

COLORREF Accent(const COLORREF light_accent) noexcept {
    if (!IsDark()) return light_accent;
    // The main window's own accents, lifted by hand so they keep their
    // saturation on a dark background (the theme lab's values; owner decision
    // 1 of the theme plan is still open). Anything else is mixed toward white.
    static constexpr std::array<std::pair<COLORREF, COLORREF>, 8> kLifted{{
        {RGB(201, 58, 55), RGB(255, 120, 112)},    // temperature
        {RGB(37, 103, 184), RGB(110, 168, 255)},   // power
        {RGB(184, 103, 21), RGB(255, 170, 90)},    // VRAM
        {RGB(25, 118, 71), RGB(96, 204, 140)},     // armed
        {RGB(164, 92, 0), RGB(255, 184, 77)},      // warning
        {RGB(25, 98, 156), RGB(110, 180, 245)},    // protected
        {RGB(190, 45, 48), RGB(255, 110, 110)},    // fault
        {RGB(176, 104, 0), RGB(230, 150, 40)},     // dirty Save & Apply border
    }};
    for (const auto& [light, dark] : kLifted) {
        if (light == light_accent) return dark;
    }
    return Mix(light_accent, RGB(255, 255, 255), 40);
}

std::optional<ChartPalette> HistoryChartPalette() noexcept {
    if (!IsDark()) return std::nullopt;
    const COLORREF background = dmlib::getDlgBackgroundColor();
    const COLORREF control = dmlib::getCtrlBackgroundColor();
    const COLORREF edge = dmlib::getEdgeColor();
    const COLORREF plot = Mix(background, control, 35);
    return ChartPalette{
        background,                      // background
        plot,                            // plot
        Mix(plot, edge, 55),             // border
        Mix(plot, edge, 70),             // grid (drawn translucent)
        dmlib::getDarkerTextColor(),     // muted_text
        plot,                            // halo: blends into the lane
        control,                         // track
        RGB(96, 165, 250),               // thumb_live
        edge,                            // thumb_paused
        RGB(0, 0, 0),                    // gap_shade
        RGB(120, 160, 220),              // net_sent, lightened for a dark plot
    };
}

}  // namespace gtg::tray::theme
