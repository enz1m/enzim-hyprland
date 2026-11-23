#include "QueuedGeometry.hpp"
#include "SurfaceState.hpp"

#include <algorithm>

void CQueuedGeometry::push(WP<SSurfaceState> state, const CBox& geometry) {
    m_entries.push_back({.state = std::move(state), .geometry = geometry});
}

void CQueuedGeometry::clear() {
    m_entries.clear();
}

std::optional<CBox> CQueuedGeometry::take() {
    const auto APPLIED = std::ranges::find_if(m_entries, [](const auto& entry) { return !entry.state.expired(); });
    m_entries.erase(m_entries.begin(), APPLIED);
    if (m_entries.empty())
        return std::nullopt;

    auto   geometry = m_entries.front().geometry;
    size_t taken    = 1;
    for (; taken < m_entries.size() && m_entries[taken].state.expired(); ++taken)
        geometry = m_entries[taken].geometry;

    m_entries.erase(m_entries.begin(), m_entries.begin() + taken);
    return geometry;
}
