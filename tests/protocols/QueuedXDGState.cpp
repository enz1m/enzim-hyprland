#include <protocols/types/QueuedXDGState.hpp>
#include <protocols/types/SurfaceState.hpp>

#include <gtest/gtest.h>

TEST(QueuedXDGState, appliedStateKeepsItsOwnGeometry) {
    CQueuedXDGState queue;
    auto            first  = makeShared<SSurfaceState>();
    auto            second = makeShared<SSurfaceState>();

    queue.push(first, {.geometry = {0, 0, 457, 318}});
    queue.push(second, {.geometry = {0, 0, 468, 325}, .serverDecorations = true});

    const auto FIRST = queue.take().value();
    EXPECT_EQ(FIRST.geometry, (CBox{0, 0, 457, 318}));
    EXPECT_FALSE(FIRST.serverDecorations);
    first.reset();
    const auto SECOND = queue.take().value();
    EXPECT_EQ(SECOND.geometry, (CBox{0, 0, 468, 325}));
    EXPECT_TRUE(SECOND.serverDecorations);
}

TEST(QueuedXDGState, mergedStatesApplyTheNewestGeometry) {
    CQueuedXDGState queue;
    auto            applied = makeShared<SSurfaceState>();
    auto            merged  = makeShared<SSurfaceState>();
    auto            later   = makeShared<SSurfaceState>();

    queue.push(applied, {.geometry = {0, 0, 400, 300}});
    queue.push(merged, {.geometry = {0, 0, 410, 305}});
    queue.push(later, {.geometry = {0, 0, 420, 310}});
    merged.reset();

    EXPECT_EQ(queue.take().value().geometry, (CBox{0, 0, 410, 305}));
    applied.reset();
    EXPECT_EQ(queue.take().value().geometry, (CBox{0, 0, 420, 310}));
    EXPECT_EQ(queue.take(), std::nullopt);
}

TEST(QueuedXDGState, droppedStatesAreSkipped) {
    CQueuedXDGState queue;
    auto            dropped = makeShared<SSurfaceState>();
    auto            applied = makeShared<SSurfaceState>();

    queue.push(dropped, {.geometry = {0, 0, 300, 200}});
    queue.push(applied, {.geometry = {0, 0, 320, 210}});
    dropped.reset();

    EXPECT_EQ(queue.take().value().geometry, (CBox{0, 0, 320, 210}));
}
