#pragma once

#include "IHyprWindowDecoration.hpp"
#include <map>

class IHID;
#include "../../helpers/signal/Signal.hpp"
#include "../Texture.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A title bar for windows whose client leaves its frame to the compositor.
class CHyprTitleBarDecoration : public IHyprWindowDecoration {
  public:
    enum eControl : uint8_t {
        CONTROL_MINIMIZE,
        CONTROL_MAXIMIZE,
        CONTROL_CLOSE,
    };

    // Controls before the colon lead the title and the rest trail it; without a colon all trail.
    // Unknown names and repeats are dropped.
    struct SButtonLayout {
        std::vector<eControl> leading;
        std::vector<eControl> trailing;
    };
    static SButtonLayout parseButtonLayout(std::string_view layout);

    CHyprTitleBarDecoration(PHLWINDOW);
    virtual ~CHyprTitleBarDecoration();

    using Contact = std::pair<const IHID*, int64_t>;

    static bool                        beginContact(Contact contact, PHLWINDOW window, const Vector2D& pos, uint32_t time, bool primary = true, PHLMONITOR monitor = {});
    static bool                        moveContact(Contact contact, const Vector2D& pos);
    static bool                        moveTouchContact(Contact contact, const Vector2D& normalizedPos);
    static bool                        endContact(Contact contact, std::optional<Vector2D> pos, uint32_t time, bool cancelled = false);
    static void                        cancelContacts(const IHID* device);
    static bool                        ownsContact(Contact contact);
    static bool                        dragging();

    virtual SDecorationPositioningInfo getPositioningInfo();
    virtual void                       onPositioningReply(const SDecorationPositioningReply& reply);
    virtual void                       draw(PHLMONITOR, float const& a);
    virtual eDecorationType            getDecorationType();
    virtual void                       updateWindow(PHLWINDOW);
    virtual void                       damageEntire();
    virtual eDecorationLayer           getDecorationLayer();
    virtual uint64_t                   getDecorationFlags();
    virtual std::string                getDisplayName();
    virtual void                       updateState();
    virtual void                       onWindowMap();

    bool                               shown() const;
    bool                               acceptsInputAt(const Vector2D& pos) const;
    // Re-evaluates whether the bar is shown and lays the window out again when that changed.
    void refresh();

  private:
    struct SControl {
        eControl kind;
        CBox     box; // bar-local, layout px
    };

    struct SLayout {
        std::vector<SControl> controls;
        CBox                  leading; // bar-local cluster boxes; empty without controls on that side
        CBox                  trailing;
        double                titleX = 0, titleW = 0;
    };

    struct STextureCache {
        std::string          key;
        SP<Render::ITexture> texture;
    };

    bool                    wanted() const;
    CBox                    barBoxGlobal() const;
    SLayout                 layout(double width) const;
    std::optional<eControl> controlAt(const Vector2D& global) const;
    static bool             offers(PHLWINDOW window, eControl control);
    void                    onPointerMove(const Vector2D& global);
    void                    activate(eControl control);
    void                    drawCluster(PHLMONITOR monitor, const SLayout& layout, const CBox& cluster, const CBox& barPx, float a, STextureCache& cache);
    void                    drawTitle(PHLMONITOR monitor, const SLayout& layout, const CBox& barPx, float a);

    CBox                    m_assignedBox;
    bool                    m_shown = false;
    std::optional<eControl> m_hovered;
    struct SContact {
        WP<CHyprTitleBarDecoration> owner;
        PHLMONITORREF               monitor;
        Vector2D                    start, pos;
        std::optional<eControl>     control;
        bool                        primary = false;
        bool                        moved   = false;
    };
    struct STap {
        const IHID* device = nullptr;
        uint32_t    time   = 0;
        Vector2D    pos;
    };
    static std::map<Contact, SContact> s_contacts;
    static std::optional<Contact>      s_dragContact;
    static CHyprSignalListener         s_dragEnded;
    std::optional<STap>                m_lastTap;
    struct SScaleCache {
        STextureCache title, leading, trailing;
    };
    std::map<float, SScaleCache> m_caches;
    CHyprSignalListener          m_mouseMove;
    CHyprSignalListener          m_propsRefreshed;
    CHyprSignalListener          m_titleChanged;
};
