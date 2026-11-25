#pragma once

#include "IHyprWindowDecoration.hpp"
#include "../../devices/IPointer.hpp"
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

    // Called for every pointer button press before decorations see it.
    static void notePress();

    virtual SDecorationPositioningInfo getPositioningInfo();
    virtual void                       onPositioningReply(const SDecorationPositioningReply& reply);
    virtual void                       draw(PHLMONITOR, float const& a);
    virtual eDecorationType            getDecorationType();
    virtual void                       updateWindow(PHLWINDOW);
    virtual void                       damageEntire();
    virtual bool                       onInputOnDeco(const eInputType, const Vector2D&, std::any = {});
    virtual eDecorationLayer           getDecorationLayer();
    virtual uint64_t                   getDecorationFlags();
    virtual std::string                getDisplayName();
    virtual void                       updateState();
    virtual void                       onWindowMap();

    bool                               shown() const;
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

    bool                  wanted() const;
    CBox                  barBoxGlobal() const;
    SLayout               layout(double width) const;
    std::optional<size_t> controlAt(const Vector2D& global) const;
    static bool           offers(PHLWINDOW window, eControl control);
    bool                  onButton(const Vector2D& global, const IPointer::SButtonEvent& e);
    void                  onPointerMove(const Vector2D& global);
    void                  activate(eControl control);
    void                  drawCluster(PHLMONITOR monitor, const SLayout& layout, const CBox& cluster, const CBox& barPx, float a, STextureCache& cache);
    void                  drawTitle(PHLMONITOR monitor, const SLayout& layout, const CBox& barPx, float a);

    CBox                  m_assignedBox;
    bool                  m_shown = false;
    std::optional<size_t> m_hovered;
    std::optional<size_t> m_pressed;
    // The bar that took the last button press, so it alone takes the release.
    // Compared, never dereferenced.
    static inline const CHyprTitleBarDecoration* s_pressOwner = nullptr;
    uint32_t              m_lastPressMs = 0;
    Vector2D              m_lastPressPos;
    bool                  m_lastPressValid = false;
    STextureCache         m_titleCache;
    STextureCache         m_leadingCache;
    STextureCache         m_trailingCache;
    CHyprSignalListener   m_mouseMove;
    CHyprSignalListener   m_propsRefreshed;
    CHyprSignalListener   m_titleChanged;
};
