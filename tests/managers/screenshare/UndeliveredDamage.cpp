#include <managers/screenshare/UndeliveredDamage.hpp>

#include <gtest/gtest.h>

using namespace Screenshare;
using Hyprutils::Math::CBox;
using Hyprutils::Math::CRegion;

TEST(UndeliveredDamage, commitWithWaitingFrameIsDelivered) {
    CUndeliveredDamage undelivered;
    undelivered.onOutputCommit(CRegion{0, 0, 100, 100}, true);

    EXPECT_TRUE(undelivered.takeForNextFrame().empty());
}

TEST(UndeliveredDamage, commitsWithoutWaitingFrameAccumulateUntilTaken) {
    CUndeliveredDamage undelivered;
    undelivered.onOutputCommit(CRegion{920, 40, 536, 100}, false);
    undelivered.onOutputCommit(CRegion{920, 140, 536, 246}, false);
    undelivered.onOutputCommit(CRegion{0, 0, 10, 10}, true);

    auto damage = undelivered.takeForNextFrame();
    EXPECT_EQ(damage.getExtents(), (CBox{920, 40, 536, 346}));
    EXPECT_FALSE(damage.containsPoint({5, 5}));
    EXPECT_TRUE(undelivered.takeForNextFrame().empty());
}
