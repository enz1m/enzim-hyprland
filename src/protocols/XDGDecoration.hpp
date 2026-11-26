#pragma once

#include <optional>
#include <deque>
#include "../helpers/signal/Signal.hpp"
#include <vector>
#include <unordered_map>
#include "WaylandProtocol.hpp"
#include "xdg-decoration-unstable-v1.hpp"

class CXDGToplevelResource;

class CXDGDecoration {
  public:
    CXDGDecoration(SP<CZxdgToplevelDecorationV1> resource_, wl_resource* toplevel);

    bool         good();
    wl_resource* toplevelResource();

  private:
    void                                                          sendMode(zxdgToplevelDecorationV1Mode mode);

    SP<CZxdgToplevelDecorationV1>                                 m_resource;
    wl_resource*                                                  m_toplevelResource = nullptr; // READ-ONLY.
    WP<CXDGToplevelResource>                                      m_toplevel;

    zxdgToplevelDecorationV1Mode                                  m_pendingMode = ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
    std::optional<zxdgToplevelDecorationV1Mode>                   m_sentMode;
    std::deque<std::pair<uint32_t, zxdgToplevelDecorationV1Mode>> m_configures;
    std::optional<zxdgToplevelDecorationV1Mode>                   m_ackedMode;
    CHyprSignalListener                                           m_ack;

    friend class CXDGDecorationProtocol;
};

class CXDGDecorationProtocol : public IWaylandProtocol {
  public:
    CXDGDecorationProtocol(const wl_interface* iface, const int& ver, const std::string& name);

    virtual void bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id);

    void         configure(SP<CXDGToplevelResource> toplevel, uint32_t serial);

    // The acknowledged mode, or none when the toplevel has no decoration object.
    std::optional<zxdgToplevelDecorationV1Mode> modeFor(SP<CXDGToplevelResource> toplevel) const;

  private:
    void onManagerResourceDestroy(wl_resource* res);
    void destroyDecoration(CXDGDecoration* decoration);
    void onGetDecoration(CZxdgDecorationManagerV1* pMgr, uint32_t id, wl_resource* xdgToplevel);

    //
    std::vector<UP<CZxdgDecorationManagerV1>>            m_managers;
    std::unordered_map<wl_resource*, UP<CXDGDecoration>> m_decorations; // xdg_toplevel -> deco

    friend class CXDGDecoration;
};

namespace PROTO {
    inline UP<CXDGDecorationProtocol> xdgDecoration;
};
