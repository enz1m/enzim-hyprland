#include "Dnd.hpp"
#ifndef NO_XWAYLAND
#include "XWM.hpp"
#include "XWayland.hpp"
#include "Server.hpp"
#include "../managers/SessionLockManager.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"
#endif
#include "../managers/XWaylandManager.hpp"
#include "../desktop/view/WLSurface.hpp"
#include "../protocols/core/Compositor.hpp"

using namespace Hyprutils::OS;

#define PROPERTY_FORMAT_32BIT 32
#define PROPERTY_LENGTH       1
#define PROPERTY_OFFSET       0

#ifndef NO_XWAYLAND
static xcb_atom_t dndActionToAtom(uint32_t actions) {
    if (actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)
        return HYPRATOMS["XdndActionCopy"];
    else if (actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE)
        return HYPRATOMS["XdndActionMove"];
    else if (actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)
        return HYPRATOMS["XdndActionAsk"];

    return XCB_ATOM_NONE;
}

void CX11DataDevice::sendDndEvent(xcb_atom_t type, xcb_client_message_data_t& data) {
    xcb_client_message_event_t event = {
        .response_type = XCB_CLIENT_MESSAGE,
        .format        = 32,
        .sequence      = 0,
        .window        = m_target,
        .type          = type,
        .data          = data,
    };
    xcb_connection_t* conn = (g_pXWayland->m_wm->getConnection());
    xcb_send_event(conn, 0, m_proxy, XCB_EVENT_MASK_NO_EVENT, rc<const char*>(&event));
    xcb_flush(conn);
}

xcb_window_t CX11DataDevice::getProxyWindow(xcb_window_t window) {
    xcb_window_t              targetWindow = window;
    xcb_get_property_cookie_t proxyCookie =
        xcb_get_property((g_pXWayland->m_wm->getConnection()), PROPERTY_OFFSET, window, HYPRATOMS["XdndProxy"], XCB_ATOM_WINDOW, PROPERTY_OFFSET, PROPERTY_LENGTH);
    xcb_get_property_reply_t* proxyReply = xcb_get_property_reply(g_pXWayland->m_wm->getConnection(), proxyCookie, nullptr);

    const auto                isValidPropertyReply = [](xcb_get_property_reply_t* reply) {
        return reply && reply->type == XCB_ATOM_WINDOW && reply->format == PROPERTY_FORMAT_32BIT && xcb_get_property_value_length(reply) == sizeof(xcb_window_t);
    };

    if (isValidPropertyReply(proxyReply)) {
        xcb_window_t              proxyWindow = *sc<xcb_window_t*>(xcb_get_property_value(proxyReply));

        xcb_get_property_cookie_t proxyVerifyCookie =
            xcb_get_property(g_pXWayland->m_wm->getConnection(), PROPERTY_OFFSET, proxyWindow, HYPRATOMS["XdndProxy"], XCB_ATOM_WINDOW, PROPERTY_OFFSET, PROPERTY_LENGTH);
        xcb_get_property_reply_t* proxyVerifyReply = xcb_get_property_reply(g_pXWayland->m_wm->getConnection(), proxyVerifyCookie, nullptr);

        if (isValidPropertyReply(proxyVerifyReply)) {
            xcb_window_t verifyWindow = *sc<xcb_window_t*>(xcb_get_property_value(proxyVerifyReply));
            if (verifyWindow == proxyWindow) {
                targetWindow = proxyWindow;
                LOG(Log::DEBUG, "Using XdndProxy window {:x} for window {:x}", proxyWindow, window);
            }
        }
        free(proxyVerifyReply); // NOLINT(cppcoreguidelines-no-malloc)
    }
    free(proxyReply); // NOLINT(cppcoreguidelines-no-malloc)

    return targetWindow;
}
static wl_data_device_manager_dnd_action atomToDndAction(xcb_atom_t atom) {
    if (atom == HYPRATOMS["XdndActionCopy"])
        return WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    if (atom == HYPRATOMS["XdndActionMove"])
        return WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
    if (atom == HYPRATOMS["XdndActionAsk"])
        return WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
    return WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
}

SP<IDataSource> CX11DataDevice::dragSource() const {
    return m_lastOffer ? m_lastOffer->getSource() : nullptr;
}

bool CX11DataDevice::canDrop() const {
    return m_lastSurface && dragSource() && m_action != WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
}

bool CX11DataDevice::allowsSelectionRequest(xcb_window_t window) const {
    if (!m_lastSurface || !dragSource() || g_pSessionLockManager->isSessionLocked())
        return false;

    const auto mask = xcb_get_setup(g_pXWayland->m_wm->getConnection())->resource_id_mask;
    return (window & ~mask) == (m_proxy & ~mask);
}

void CX11DataDevice::handleReply(const xcb_client_message_event_t& event) {
    if (event.format != 32 || event.data.data32[0] != m_target || !m_lastOffer)
        return;
    const auto source = m_lastOffer->getSource();
    if (!source)
        return;

    const bool accepted = event.data.data32[1] & 1;
    if (event.type == HYPRATOMS["XdndFinished"]) {
        if (m_dropTimeout) {
            const auto action = atomToDndAction(event.data.data32[2]);
            finishDrop(m_version < 5 || (accepted && action == m_action));
        }
        return;
    }

    if (m_dropTimeout)
        return;
    const auto proposed = atomToDndAction(event.data.data32[4]);
    const auto action   = accepted && (source->actions() & proposed) ? proposed : WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
    if (action != m_action) {
        m_action = action;
        source->sendDndAction(action);
    }
}

static void releaseDndSelection(xcb_connection_t* conn, xcb_window_t sourceWindow) {
    const auto                                                              atom   = HYPRATOMS["XdndSelection"];
    const auto                                                              cookie = xcb_get_selection_owner(conn, atom);
    const std::unique_ptr<xcb_get_selection_owner_reply_t, decltype(&free)> reply(xcb_get_selection_owner_reply(conn, cookie, nullptr), free);
    if (reply && reply->owner == sourceWindow) {
        xcb_set_selection_owner(conn, XCB_WINDOW_NONE, atom, XCB_TIME_CURRENT_TIME);
        xcb_flush(conn);
    }
}

void CX11DataDevice::finishDrop(bool success) {
    const auto source = dragSource();
    cleanupState();
    releaseDndSelection(g_pXWayland->m_wm->getConnection(), g_pXWayland->m_wm->m_dndSelection.window);
    if (source) {
        if (success)
            source->sendDndFinished();
        else
            source->cancelled();
    }
}
#endif

eDataSourceType CX11DataOffer::type() {
    return DATA_SOURCE_TYPE_X11;
}

SP<CWLDataOfferResource> CX11DataOffer::getWayland() {
    return nullptr;
}

SP<CX11DataOffer> CX11DataOffer::getX11() {
    return m_self.lock();
}

SP<IDataSource> CX11DataOffer::getSource() {
    return m_source.lock();
}

void CX11DataDevice::sendDataOffer(SP<IDataOffer> offer) {
    ; // no-op, I don't think this has an X equiv
}

void CX11DataDevice::sendEnter(uint32_t serial, SP<CWLSurfaceResource> surf, const Vector2D& local, SP<IDataOffer> offer) {
#ifndef NO_XWAYLAND
    auto XSURF = g_pXWayland->m_wm->windowForWayland(surf);

    if (!XSURF) {
        LOG(Log::ERR, "CX11DataDevice::sendEnter: No xwayland surface for destination");
        return;
    }

    auto SOURCE = offer->getSource();

    if (!SOURCE) {
        LOG(Log::ERR, "CX11DataDevice::sendEnter: No source");
        return;
    }

    if (m_dropTimeout)
        finishDrop(false);
    const auto                                                       proxy  = getProxyWindow(XSURF->m_xID);
    auto                                                             conn   = g_pXWayland->m_wm->getConnection();
    auto                                                             cookie = xcb_get_property(conn, 0, proxy, HYPRATOMS["XdndAware"], XCB_ATOM_ATOM, 0, 1);
    const std::unique_ptr<xcb_get_property_reply_t, decltype(&free)> aware(xcb_get_property_reply(conn, cookie, nullptr), free);
    if (!aware || aware->format != 32 || aware->value_len < 1)
        return;

    const auto version = *sc<const uint32_t*>(xcb_get_property_value(aware.get()));
    if (version < 3)
        return;

    m_target  = XSURF->m_xID;
    m_proxy   = proxy;
    m_version = std::min<uint32_t>(version, XDND_VERSION);

    std::vector<xcb_atom_t> targets;
    // reserve to avoid reallocations
    targets.reserve(SOURCE->mimes().size());
    for (auto const& m : SOURCE->mimes()) {
        targets.push_back(g_pXWayland->m_wm->mimeToAtom(m));
    }

    xcb_change_property(g_pXWayland->m_wm->getConnection(), XCB_PROP_MODE_REPLACE, g_pXWayland->m_wm->m_dndSelection.window, HYPRATOMS["XdndTypeList"], XCB_ATOM_ATOM, 32,
                        targets.size(), targets.data());

    xcb_set_selection_owner(g_pXWayland->m_wm->getConnection(), g_pXWayland->m_wm->m_dndSelection.window, HYPRATOMS["XdndSelection"], XCB_TIME_CURRENT_TIME);
    xcb_flush(g_pXWayland->m_wm->getConnection());

    xcb_client_message_data_t data = {{0}};
    data.data32[0]                 = g_pXWayland->m_wm->m_dndSelection.window;
    data.data32[1]                 = (m_version << 24) | (targets.size() > 3 ? 1 : 0);
    for (size_t i = 0; i < std::min<size_t>(targets.size(), 3); ++i)
        data.data32[i + 2] = targets[i];

    sendDndEvent(HYPRATOMS["XdndEnter"], data);

    m_lastSurface = XSURF;
    m_lastOffer   = offer;

    auto hlSurface = XSURF->m_surface.lock();
    if (!hlSurface) {
        LOG(Log::ERR, "CX11DataDevice::sendEnter: Non desktop x surface?!");
        m_lastSurfaceCoords = {};
        return;
    }

    m_lastSurfaceCoords = g_pXWaylandManager->xwaylandToWaylandCoords(XSURF->m_geometry.pos());
#endif
}

CX11DataDevice::~CX11DataDevice() {
#ifndef NO_XWAYLAND
    if (m_dropTimeout)
        g_pEventLoopManager->removeTimer(m_dropTimeout);
#endif
}

void CX11DataDevice::cleanupState() {
#ifndef NO_XWAYLAND
    if (m_dropTimeout)
        g_pEventLoopManager->removeTimer(m_dropTimeout);
    m_dropTimeout.reset();
    m_target  = XCB_WINDOW_NONE;
    m_proxy   = XCB_WINDOW_NONE;
    m_version = 0;
    m_action  = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
#endif
    m_lastSurface.reset();
    m_lastOffer.reset();
    m_lastSurfaceCoords = {};
    m_lastTime          = 0;
}

void CX11DataDevice::sendLeave() {
#ifndef NO_XWAYLAND
    if (!m_target)
        return;

    xcb_client_message_data_t data = {{0}};
    data.data32[0]                 = g_pXWayland->m_wm->m_dndSelection.window;

    sendDndEvent(HYPRATOMS["XdndLeave"], data);
    if (const auto source = dragSource(); source && canDrop())
        source->sendDndAction(WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);
    releaseDndSelection(g_pXWayland->m_wm->getConnection(), g_pXWayland->m_wm->m_dndSelection.window);

    cleanupState();
#endif
}

void CX11DataDevice::sendMotion(uint32_t timeMs, const Vector2D& local) {
#ifndef NO_XWAYLAND
    if (!m_lastSurface || !m_lastOffer || !m_lastOffer->getSource())
        return;

    const auto                XCOORDS = g_pXWaylandManager->waylandToXWaylandCoords(m_lastSurfaceCoords + local);
    const uint32_t            coords  = (sc<uint32_t>(XCOORDS.x) << 16) | sc<uint32_t>(XCOORDS.y);

    xcb_client_message_data_t data = {{0}};
    data.data32[0]                 = g_pXWayland->m_wm->m_dndSelection.window;
    data.data32[2]                 = coords;
    data.data32[3]                 = timeMs;
    data.data32[4]                 = dndActionToAtom(m_lastOffer->getSource()->actions());

    sendDndEvent(HYPRATOMS["XdndPosition"], data);

    m_lastTime = timeMs;
#endif
}

void CX11DataDevice::sendDrop() {
#ifndef NO_XWAYLAND
    if (!m_lastSurface || !m_lastOffer) {
        LOG(Log::ERR, "CX11DataDevice::sendDrop: No surface or offer");
        return;
    }

    xcb_client_message_data_t data = {{0}};
    data.data32[0]                 = g_pXWayland->m_wm->m_dndSelection.window;
    data.data32[2]                 = m_lastTime;

    sendDndEvent(HYPRATOMS["XdndDrop"], data);
    if (const auto source = m_lastOffer->getSource())
        source->sendDndDropPerformed();

    m_dropTimeout = makeShared<CEventLoopTimer>(std::chrono::seconds(5), [this](auto, auto) { finishDrop(false); }, nullptr);
    g_pEventLoopManager->addTimer(m_dropTimeout);
#endif
}

void CX11DataDevice::sendSelection(SP<IDataOffer> offer) {
    ; // no-op. Selection is done separately.
}

eDataSourceType CX11DataDevice::type() {
    return DATA_SOURCE_TYPE_X11;
}

SP<CWLDataDeviceResource> CX11DataDevice::getWayland() {
    return nullptr;
}

SP<CX11DataDevice> CX11DataDevice::getX11() {
    return m_self.lock();
}

std::vector<std::string> CX11DataSource::mimes() {
    return m_mimeTypes;
}

void CX11DataSource::send(const std::string& mime, CFileDescriptor fd) {
    ; // no-op
}

void CX11DataSource::accepted(const std::string& mime) {
    ; // no-op
}

void CX11DataSource::cancelled() {
    m_dndSuccess = false;
    m_dropped    = false;
}

bool CX11DataSource::hasDnd() {
    return m_dnd;
}

bool CX11DataSource::dndDone() {
    return m_dropped;
}

void CX11DataSource::error(uint32_t code, const std::string& msg) {
    LOG(Log::ERR, "CX11DataSource error: code {} msg {}", code, msg);
    m_dndSuccess = false;
    m_dropped    = false;
}

void CX11DataSource::sendDndFinished() {
    m_dndSuccess = true;
}

uint32_t CX11DataSource::actions() {
    return m_supportedActions;
}

eDataSourceType CX11DataSource::type() {
    return DATA_SOURCE_TYPE_X11;
}

void CX11DataSource::sendDndDropPerformed() {
    m_dropped = true;
}

void CX11DataSource::sendDndAction(wl_data_device_manager_dnd_action a) {
    ; // no-op
}

void CX11DataDevice::forceCleanupDnd() {
#ifndef NO_XWAYLAND
    if (m_dropTimeout)
        finishDrop(false);
    else
        sendLeave();
#endif
}
