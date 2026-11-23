#pragma once

#include "../../helpers/math/Math.hpp"
#include "../../helpers/memory/Memory.hpp"

#include <optional>
#include <vector>

struct SSurfaceState;

// The geometry of a fenced commit must survive later client requests.
class CQueuedGeometry {
  public:
    void                push(WP<SSurfaceState> state, const CBox& geometry);
    void                clear();

    // The geometry of the state being applied. Gone states before it were
    // dropped; gone states after it were merged into it.
    std::optional<CBox> take();

  private:
    struct SEntry {
        WP<SSurfaceState> state;
        CBox              geometry;
    };
    std::vector<SEntry> m_entries;
};
