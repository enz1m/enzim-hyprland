#pragma once

#include "../../helpers/math/Math.hpp"
#include "../../helpers/memory/Memory.hpp"

#include <optional>
#include <vector>

struct SSurfaceState;

// The role state of a fenced commit must survive later client requests.
class CQueuedXDGState {
  public:
    struct SState {
        CBox geometry;
        bool serverDecorations = false;
    };

    void push(WP<SSurfaceState> state, const SState& roleState);
    void clear();

    // Expired states before the applied commit were dropped; expired states after it were merged into it.
    std::optional<SState> take();

  private:
    struct SEntry {
        WP<SSurfaceState> state;
        SState            roleState;
    };
    std::vector<SEntry> m_entries;
};
