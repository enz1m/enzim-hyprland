#include "ServerDecorationKDE.hpp"
#include "core/Compositor.hpp"
CServerDecorationKDE::CServerDecorationKDE(SP<COrgKdeKwinServerDecoration> resource_, SP<CWLSurfaceResource> surf_) : m_surf(surf_), m_resource(resource_) {
    if UNLIKELY (!good())
        return;

    m_resource->setRelease([this](COrgKdeKwinServerDecoration* pMgr) { PROTO::serverDecorationKDE->destroyResource(this); });
    m_resource->setOnDestroy([this](COrgKdeKwinServerDecoration* pMgr) { PROTO::serverDecorationKDE->destroyResource(this); });
    m_resource->setRequestMode([this](COrgKdeKwinServerDecoration*, uint32_t mode) { sendMode(kdeModeOnRequestCSD(mode)); });

    sendMode(kdeDefaultModeCSD());
}

void CServerDecorationKDE::sendMode(uint32_t mode) {
    m_resource->sendMode(mode);
    m_mostRecentlySent = mode;
}

bool CServerDecorationKDE::good() {
    return m_resource->resource();
}

uint32_t CServerDecorationKDE::kdeDefaultModeCSD() {
    return ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_SERVER;
}

uint32_t CServerDecorationKDE::kdeModeOnRequestCSD(uint32_t modeRequestedByClient) {
    if (modeRequestedByClient == ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_NONE || modeRequestedByClient == ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_CLIENT)
        return modeRequestedByClient;
    return kdeDefaultModeCSD();
}

CServerDecorationKDEProtocol::CServerDecorationKDEProtocol(const wl_interface* iface, const int& ver, const std::string& name) : IWaylandProtocol(iface, ver, name) {
    ;
}

void CServerDecorationKDEProtocol::bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) {
    const auto RESOURCE = m_managers.emplace_back(makeUnique<COrgKdeKwinServerDecorationManager>(client, ver, id)).get();
    RESOURCE->setOnDestroy([this](COrgKdeKwinServerDecorationManager* p) { this->onManagerResourceDestroy(p->resource()); });

    RESOURCE->setCreate([this](COrgKdeKwinServerDecorationManager* pMgr, uint32_t id, wl_resource* pointer) { this->createDecoration(pMgr, id, pointer); });

    RESOURCE->sendDefaultMode(kdeDefaultManagerModeCSD());
}

uint32_t CServerDecorationKDEProtocol::kdeDefaultManagerModeCSD() {
    return ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_SERVER;
}

void CServerDecorationKDEProtocol::onManagerResourceDestroy(wl_resource* res) {
    std::erase_if(m_managers, [&](const auto& other) { return other->resource() == res; });
}

void CServerDecorationKDEProtocol::destroyResource(CServerDecorationKDE* decoration) {
    std::erase_if(m_decos, [&](const auto& other) { return other.get() == decoration; });
}

std::optional<uint32_t> CServerDecorationKDEProtocol::modeFor(SP<CWLSurfaceResource> surface) const {
    if (!surface)
        return std::nullopt;

    for (const auto& decoration : m_decos) {
        if (decoration->m_surf == surface)
            return decoration->m_mostRecentlySent;
    }

    return std::nullopt;
}

void CServerDecorationKDEProtocol::createDecoration(COrgKdeKwinServerDecorationManager* pMgr, uint32_t id, wl_resource* surf) {
    const auto CLIENT = pMgr->client();
    const auto RESOURCE =
        m_decos.emplace_back(makeUnique<CServerDecorationKDE>(makeShared<COrgKdeKwinServerDecoration>(CLIENT, pMgr->version(), id), CWLSurfaceResource::fromResource(surf))).get();

    if UNLIKELY (!RESOURCE->good()) {
        pMgr->noMemory();
        m_decos.pop_back();
        return;
    }
}
