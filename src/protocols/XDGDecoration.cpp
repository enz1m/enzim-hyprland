#include "XDGDecoration.hpp"
#include "XDGShell.hpp"
#include <algorithm>

CXDGDecoration::CXDGDecoration(SP<CZxdgToplevelDecorationV1> resource_, wl_resource* toplevel) :
    m_resource(resource_), m_toplevelResource(toplevel), m_toplevel(CXDGToplevelResource::fromResource(toplevel)) {
    if UNLIKELY (!m_resource->resource())
        return;

    m_resource->setDestroy([this](CZxdgToplevelDecorationV1* pMgr) { PROTO::xdgDecoration->destroyDecoration(this); });
    m_resource->setOnDestroy([this](CZxdgToplevelDecorationV1* pMgr) { PROTO::xdgDecoration->destroyDecoration(this); });

    m_resource->setSetMode([this](CZxdgToplevelDecorationV1*, zxdgToplevelDecorationV1Mode mode) {
        switch (mode) {
            case ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE:
            case ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE: sendMode(mode); break;
            default: m_resource->error(ZXDG_TOPLEVEL_DECORATION_V1_ERROR_INVALID_MODE, "Invalid decoration mode"); break;
        }
    });
    m_resource->setUnsetMode([this](CZxdgToplevelDecorationV1*) { sendMode(ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE); });

    if (const auto TOPLEVEL = m_toplevel.lock(); TOPLEVEL && TOPLEVEL->m_owner) {
        // Recreating the object before a surface commit preserves the prior mode until configure is acknowledged.
        if (TOPLEVEL->m_owner->m_pending.serverDecorations)
            m_ackedMode = ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
        m_ack = TOPLEVEL->m_owner->m_events.ack.listen([this](uint32_t serial) {
            while (!m_configures.empty() && sc<int32_t>(serial - m_configures.front().first) >= 0) {
                m_ackedMode = m_configures.front().second;
                m_configures.pop_front();
            }
        });
    }
    sendMode(ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void CXDGDecoration::sendMode(zxdgToplevelDecorationV1Mode mode) {
    if (m_pendingMode == mode)
        return;

    m_pendingMode = mode;
    if (const auto TOPLEVEL = m_toplevel.lock(); TOPLEVEL && TOPLEVEL->m_owner && !TOPLEVEL->m_owner->m_initialCommit)
        TOPLEVEL->m_owner->scheduleConfigure();
}

void CXDGDecorationProtocol::configure(SP<CXDGToplevelResource> toplevel, uint32_t serial) {
    if (!toplevel)
        return;

    for (const auto& [_, decoration] : m_decorations) {
        if (decoration->m_toplevel != toplevel || decoration->m_sentMode == decoration->m_pendingMode)
            continue;
        decoration->m_resource->sendConfigure(decoration->m_pendingMode);
        decoration->m_sentMode = decoration->m_pendingMode;
        decoration->m_configures.emplace_back(serial, decoration->m_pendingMode);
        return;
    }
}

bool CXDGDecoration::good() {
    return m_resource->resource();
}

wl_resource* CXDGDecoration::toplevelResource() {
    return m_toplevelResource;
}

CXDGDecorationProtocol::CXDGDecorationProtocol(const wl_interface* iface, const int& ver, const std::string& name) : IWaylandProtocol(iface, ver, name) {
    ;
}

void CXDGDecorationProtocol::bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) {
    const auto RESOURCE = m_managers.emplace_back(makeUnique<CZxdgDecorationManagerV1>(client, ver, id)).get();
    RESOURCE->setOnDestroy([this](CZxdgDecorationManagerV1* p) { this->onManagerResourceDestroy(p->resource()); });

    RESOURCE->setDestroy([this](CZxdgDecorationManagerV1* pMgr) { this->onManagerResourceDestroy(pMgr->resource()); });
    RESOURCE->setGetToplevelDecoration([this](CZxdgDecorationManagerV1* pMgr, uint32_t id, wl_resource* xdgToplevel) { this->onGetDecoration(pMgr, id, xdgToplevel); });
}

void CXDGDecorationProtocol::onManagerResourceDestroy(wl_resource* res) {
    std::erase_if(m_managers, [&](const auto& other) { return other->resource() == res; });
}

void CXDGDecorationProtocol::destroyDecoration(CXDGDecoration* decoration) {
    m_decorations.erase(decoration->toplevelResource());
}

std::optional<zxdgToplevelDecorationV1Mode> CXDGDecorationProtocol::modeFor(SP<CXDGToplevelResource> toplevel) const {
    if (!toplevel)
        return std::nullopt;

    for (const auto& [_, decoration] : m_decorations) {
        if (decoration->m_toplevel == toplevel)
            return decoration->m_ackedMode.value_or(ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    }

    return std::nullopt;
}

void CXDGDecorationProtocol::onGetDecoration(CZxdgDecorationManagerV1* pMgr, uint32_t id, wl_resource* xdgToplevel) {
    if UNLIKELY (m_decorations.contains(xdgToplevel)) {
        pMgr->error(ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED, "Decoration object already exists");
        return;
    }

    const auto CLIENT = pMgr->client();
    const auto RESOURCE =
        m_decorations.emplace(xdgToplevel, makeUnique<CXDGDecoration>(makeShared<CZxdgToplevelDecorationV1>(CLIENT, pMgr->version(), id), xdgToplevel)).first->second.get();

    if UNLIKELY (!RESOURCE->good()) {
        pMgr->noMemory();
        m_decorations.erase(xdgToplevel);
        return;
    }
}
