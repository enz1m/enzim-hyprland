#include <render/decorations/CHyprTitleBarDecoration.hpp>

#include <gtest/gtest.h>

using enum CHyprTitleBarDecoration::eControl;
using Controls = std::vector<CHyprTitleBarDecoration::eControl>;

TEST(TitleBar, layoutWithoutColonTrailsTheTitle) {
    const auto LAYOUT = CHyprTitleBarDecoration::parseButtonLayout("minimize,maximize,close");

    EXPECT_TRUE(LAYOUT.leading.empty());
    EXPECT_EQ(LAYOUT.trailing, (Controls{CONTROL_MINIMIZE, CONTROL_MAXIMIZE, CONTROL_CLOSE}));
}

TEST(TitleBar, layoutSplitsAtTheColon) {
    const auto LAYOUT = CHyprTitleBarDecoration::parseButtonLayout("close,minimize:maximize");

    EXPECT_EQ(LAYOUT.leading, (Controls{CONTROL_CLOSE, CONTROL_MINIMIZE}));
    EXPECT_EQ(LAYOUT.trailing, (Controls{CONTROL_MAXIMIZE}));
}

TEST(TitleBar, layoutDropsUnknownAndRepeatedControls) {
    const auto LAYOUT = CHyprTitleBarDecoration::parseButtonLayout("appmenu,close:close,icon,minimize,minimize");

    EXPECT_EQ(LAYOUT.leading, (Controls{CONTROL_CLOSE}));
    EXPECT_EQ(LAYOUT.trailing, (Controls{CONTROL_MINIMIZE}));
}

TEST(TitleBar, emptyLayoutNamesNoControls) {
    const auto LAYOUT = CHyprTitleBarDecoration::parseButtonLayout(":");

    EXPECT_TRUE(LAYOUT.leading.empty());
    EXPECT_TRUE(LAYOUT.trailing.empty());
}
