#pragma once

#include <optional>
#include <vector>
#include <cstdint>
#include "WaylandProtocol.hpp"
#include "kde-server-decoration.hpp"

class CWLSurfaceResource;

class CServerDecorationKDE {
  public:
    CServerDecorationKDE(SP<COrgKdeKwinServerDecoration> resource_, SP<CWLSurfaceResource> surf);

    SP<CWLSurfaceResource> m_surf;

    uint32_t               m_mostRecentlySent = 0;

    bool                   good();

  private:
    uint32_t                        kdeDefaultModeCSD();
    uint32_t                        kdeModeOnRequestCSD(uint32_t modeRequestedByClient);
    void                            sendMode(uint32_t mode);

    SP<COrgKdeKwinServerDecoration> m_resource;
};

class CServerDecorationKDEProtocol : public IWaylandProtocol {
  public:
    CServerDecorationKDEProtocol(const wl_interface* iface, const int& ver, const std::string& name);

    virtual void bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id);

    // The mode last sent for the surface, or none when it has no decoration object.
    std::optional<uint32_t> modeFor(SP<CWLSurfaceResource> surface) const;

  private:
    uint32_t kdeDefaultManagerModeCSD();

    void     onManagerResourceDestroy(wl_resource* res);
    void     destroyResource(CServerDecorationKDE* deco);

    void     createDecoration(COrgKdeKwinServerDecorationManager* pMgr, uint32_t id, wl_resource* surf);

    //
    std::vector<UP<COrgKdeKwinServerDecorationManager>> m_managers;
    std::vector<UP<CServerDecorationKDE>>               m_decos;

    friend class CServerDecorationKDE;
};

namespace PROTO {
    inline UP<CServerDecorationKDEProtocol> serverDecorationKDE;
};
