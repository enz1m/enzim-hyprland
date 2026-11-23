#include <protocols/types/QueuedGeometry.hpp>
#include <protocols/types/SurfaceState.hpp>

#include <gtest/gtest.h>

TEST(QueuedGeometry, appliedStateKeepsItsOwnGeometry) {
    CQueuedGeometry queue;
    auto            first  = makeShared<SSurfaceState>();
    auto            second = makeShared<SSurfaceState>();

    queue.push(first, {0, 0, 457, 318});
    queue.push(second, {0, 0, 468, 325});

    EXPECT_EQ(queue.take(), (CBox{0, 0, 457, 318}));
    first.reset();
    EXPECT_EQ(queue.take(), (CBox{0, 0, 468, 325}));
}

TEST(QueuedGeometry, mergedStatesApplyTheNewestGeometry) {
    CQueuedGeometry queue;
    auto            applied = makeShared<SSurfaceState>();
    auto            merged  = makeShared<SSurfaceState>();
    auto            later   = makeShared<SSurfaceState>();

    queue.push(applied, {0, 0, 400, 300});
    queue.push(merged, {0, 0, 410, 305});
    queue.push(later, {0, 0, 420, 310});
    merged.reset();

    EXPECT_EQ(queue.take(), (CBox{0, 0, 410, 305}));
    applied.reset();
    EXPECT_EQ(queue.take(), (CBox{0, 0, 420, 310}));
    EXPECT_EQ(queue.take(), std::nullopt);
}

TEST(QueuedGeometry, droppedStatesAreSkipped) {
    CQueuedGeometry queue;
    auto            dropped = makeShared<SSurfaceState>();
    auto            applied = makeShared<SSurfaceState>();

    queue.push(dropped, {0, 0, 300, 200});
    queue.push(applied, {0, 0, 320, 210});
    dropped.reset();

    EXPECT_EQ(queue.take(), (CBox{0, 0, 320, 210}));
}
