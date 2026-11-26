#include "QueuedXDGState.hpp"
#include "SurfaceState.hpp"

#include <algorithm>

void CQueuedXDGState::push(WP<SSurfaceState> state, const SState& roleState) {
    m_entries.push_back({.state = std::move(state), .roleState = roleState});
}

void CQueuedXDGState::clear() {
    m_entries.clear();
}

std::optional<CQueuedXDGState::SState> CQueuedXDGState::take() {
    const auto APPLIED = std::ranges::find_if(m_entries, [](const auto& entry) { return !entry.state.expired(); });
    m_entries.erase(m_entries.begin(), APPLIED);
    if (m_entries.empty())
        return std::nullopt;

    auto   roleState = m_entries.front().roleState;
    size_t taken     = 1;
    for (; taken < m_entries.size() && m_entries[taken].state.expired(); ++taken)
        roleState = m_entries[taken].roleState;

    m_entries.erase(m_entries.begin(), m_entries.begin() + taken);
    return roleState;
}
