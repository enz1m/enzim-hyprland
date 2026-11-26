#pragma once

#include <hyprutils/math/Region.hpp>

namespace Screenshare {
    // Output damage committed while a capture session had no frame waiting. Another session's
    // waiting frame makes such a commit copy-ready, so the shared mirror does not record it as stale.
    class CUndeliveredDamage {
      public:
        void onOutputCommit(const Hyprutils::Math::CRegion& damage, bool frameWaiting) {
            if (!frameWaiting)
                m_damage.add(damage);
        }

        Hyprutils::Math::CRegion takeForNextFrame() {
            auto damage = m_damage.copy();
            m_damage.clear();
            return damage;
        }

      private:
        Hyprutils::Math::CRegion m_damage;
    };
}
