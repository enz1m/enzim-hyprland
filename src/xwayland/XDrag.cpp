#ifndef NO_XWAYLAND

#include "XWayland.hpp"
#include "../Compositor.hpp"
#include "../managers/SeatManager.hpp"
#include "../managers/SessionLockManager.hpp"
#include "../managers/XWaylandManager.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"
#include "../managers/input/InputManager.hpp"
#include "../protocols/core/Compositor.hpp"
#include "../protocols/core/DataDevice.hpp"
#include "../protocols/XDGShell.hpp"

#include <algorithm>
#include <xcb/xcbext.h>
#include <memory>
#include <unordered_set>
#include <ranges>

using namespace Hyprutils::OS;
using namespace std::chrono_literals;

static constexpr uint32_t COPY = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
static constexpr uint32_t MOVE = WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;

static uint32_t           dragAction(xcb_atom_t atom) {
    if (atom == HYPRATOMS["XdndActionCopy"])
        return COPY;
    if (atom == HYPRATOMS["XdndActionMove"])
        return MOVE;
    return 0;
}

static xcb_atom_t dragActionAtom(uint32_t action) {
    if (action == COPY)
        return HYPRATOMS["XdndActionCopy"];
    if (action == MOVE)
        return HYPRATOMS["XdndActionMove"];
    return XCB_ATOM_NONE;
}

static std::vector<xcb_atom_t> atomList(xcb_connection_t* connection, xcb_window_t window, xcb_atom_t property) {
    const auto                                                       cookie = xcb_get_property(connection, false, window, property, XCB_ATOM_ATOM, 0, 256);
    const std::unique_ptr<xcb_get_property_reply_t, decltype(&free)> reply(xcb_get_property_reply(connection, cookie, nullptr), free);
    if (!reply || reply->type != XCB_ATOM_ATOM || reply->format != 32 || reply->bytes_after || !reply->value_len)
        return {};

    const auto atoms = sc<const xcb_atom_t*>(xcb_get_property_value(reply.get()));
    return {atoms, atoms + reply->value_len};
}

static xcb_extension_t XDND_AUTH_EXTENSION = {"ENZIM-XDND-1", 0};
enum eXDND_AUTH_REQUEST : uint8_t {
    REGISTER_PROXY  = 1,
    QUERY           = 2,
    GUARD_REQUESTOR = 3
};

static uint32_t sendXdndAuthRequest(xcb_connection_t* connection, eXDND_AUTH_REQUEST opcode, xcb_window_t window = 0, uint32_t generation = 0) {
    const xcb_protocol_request_t request{2, &XDND_AUTH_EXTENSION, opcode, opcode != QUERY};
    struct {
        uint8_t  majorOpcode, minorOpcode;
        uint16_t length;
        uint32_t window, generation;
    } payload{0, 0, 0, window, generation};
    iovec parts[4] = {{}, {}, {&payload, sizeof(payload)}, {nullptr, 0}};
    return xcb_send_request(connection, XCB_REQUEST_CHECKED, parts + 2, &request);
}

bool CXWM::registerXDragProxy() {
    const auto  conn      = getConnection();
    const auto* extension = xcb_get_extension_data(conn, &XDND_AUTH_EXTENSION);
    if (!extension || !extension->present)
        return false;
    const std::unique_ptr<xcb_generic_error_t, decltype(&free)> error(xcb_request_check(conn, {sendXdndAuthRequest(conn, REGISTER_PROXY, m_dndSelection.window)}), free);
    return !error;
}

CXWM::SXDragClaim CXWM::xDragClaim() {
    if (!m_xDragAuthorized)
        return {};
    const auto conn = getConnection();
    struct SReply {
        uint8_t     type, pad;
        uint16_t    sequence;
        uint32_t    length;
        SXDragClaim claim;
    };
    const auto                                     sequence = sendXdndAuthRequest(conn, QUERY);
    const std::unique_ptr<SReply, decltype(&free)> reply(static_cast<SReply*>(xcb_wait_for_reply(conn, sequence, nullptr)), free);
    return reply ? reply->claim : SXDragClaim{};
}

bool CXWM::guardXDragRequestor(xcb_window_t requestor, uint32_t generation) {
    if (!m_xDragAuthorized || !generation)
        return false;
    const auto                                                  conn = getConnection();
    const std::unique_ptr<xcb_generic_error_t, decltype(&free)> error(xcb_request_check(conn, {sendXdndAuthRequest(conn, GUARD_REQUESTOR, requestor, generation)}), free);
    return !error;
}

static void sendXdndMessage(xcb_connection_t* connection, xcb_window_t proxy, xcb_window_t window, xcb_atom_t type, uint32_t flags, xcb_atom_t action) {
    xcb_client_message_event_t message = {
        .response_type = XCB_CLIENT_MESSAGE,
        .format        = 32,
        .window        = window,
        .type          = type,
        .data          = {{0}},
    };
    message.data.data32[0]                                       = proxy;
    message.data.data32[1]                                       = flags;
    message.data.data32[type == HYPRATOMS["XdndStatus"] ? 4 : 2] = action;
    xcb_send_event(connection, 0, window, XCB_EVENT_MASK_NO_EVENT, rc<const char*>(&message));
    xcb_flush(connection);
}

class CX11Drag : public IDataSource {
  public:
    CX11Drag(CXWM& wm, xcb_window_t owner, SP<CXWaylandSurface> origin, SP<IPointer> pointer, uint32_t button, uint32_t generation);
    ~CX11Drag() override;

    std::vector<std::string> mimes() override;
    void                     send(const std::string& mime, CFileDescriptor fd) override;
    void                     accepted(const std::string& mime) override;
    void                     cancelled() override;
    bool                     hasDnd() override;
    bool                     dndDone() override;
    void                     error(uint32_t code, const std::string& msg) override;
    void                     sendDndFinished() override;
    eDataSourceType          type() override;
    uint32_t                 actions() override;
    uint32_t                 preferredDndAction() override;
    void                     sendDndAction(wl_data_device_manager_dnd_action action) override;

    void                     motion(SP<CWLSurfaceResource> surface, PHLMONITOR monitor, const Vector2D& point, const Vector2D& local, uint32_t time);
    void                     release(uint32_t time);
    void                     handleMessage(const xcb_client_message_event_t& event);
    void                     end(bool success, bool notifySource);

  private:
    struct SPosition {
        uint32_t offer = 0;
        Vector2D local;
        Vector2D point;
        uint32_t action = 0;
    };

    void                                            enter(const xcb_client_message_data_t& data);
    bool                                            watchSourceWindow(xcb_window_t window);
    void                                            unwatchSourceWindow();
    void                                            updateOffer();
    void                                            leave();
    void                                            position(const xcb_client_message_data_t& data);
    void                                            awaitFeedback();
    void                                            tryStatus();
    void                                            drop(xcb_timestamp_t time);
    void                                            setProxyBox(std::optional<CBox> box);
    void                                            sendMessage(xcb_atom_t type, uint32_t flags, xcb_atom_t action);
    void                                            sendStatus(bool eligible);
    void                                            reportStatus(uint32_t action, const Vector2D& point);
    void                                            reevaluateStatus();
    uint32_t                                        statusAction(uint32_t requested) const;
    bool                                            acceptsDrop() const;
    void                                            updateDropDeadline();
    void                                            complete();
    bool                                            ownsSelection(bool requireRelease = false);

    CXWM&                                           m_wm;
    xcb_window_t                                    m_owner;
    WP<CXWaylandSurface>                            m_origin;
    WP<IPointer>                                    m_pointer;
    uint32_t                                        m_button;
    uint32_t                                        m_generation;

    WP<CWLSurfaceResource>                          m_target;
    Vector2D                                        m_local;
    Vector2D                                        m_xPoint;
    std::optional<Vector2D>                         m_reportedPoint;
    uint32_t                                        m_motionTime    = 0;
    xcb_window_t                                    m_sourceWindow  = XCB_WINDOW_NONE;
    xcb_window_t                                    m_watchedSource = XCB_WINDOW_NONE;
    uint32_t                                        m_watchedMask   = 0;
    xcb_timestamp_t                                 m_selectionTime = XCB_CURRENT_TIME;
    uint32_t                                        m_version       = 0;
    std::vector<std::pair<std::string, xcb_atom_t>> m_types;

    uint32_t                                        m_actions        = COPY | MOVE;
    uint32_t                                        m_requested      = COPY;
    uint32_t                                        m_selected       = 0;
    uint32_t                                        m_reportedAction = 0;
    std::string                                     m_acceptedMime, m_reportedMime;
    std::optional<SPosition>                        m_position;
    uint32_t                                        m_pingSerial      = 0;
    bool                                            m_feedbackCurrent = false;
    uint32_t                                        m_statusWaits     = 0;
    std::optional<CBox>                             m_proxyBox;
    bool                                            m_entered           = false;
    uint32_t                                        m_offerVersion      = 0;
    uint32_t                                        m_offerSerial       = 0;
    bool                                            m_released          = false;
    bool                                            m_receivedDrop      = false;
    bool                                            m_dropped           = false;
    bool                                            m_finishRequested   = false;
    bool                                            m_readFailed        = false;
    bool                                            m_dropReadSucceeded = false;
    bool                                            m_deleting          = false;
    std::optional<Time::steady_tp>                  m_dropStarted;
    std::unordered_set<std::string>                 m_previewMimes;
    size_t                                          m_pendingReads = 0;

    SP<CEventLoopTimer>                             m_statusTimer;
    SP<CEventLoopTimer>                             m_dropTimer;
    CHyprSignalListener                             m_originUnmap, m_originDestroy, m_pointerDestroy, m_targetUnmap, m_targetDestroy, m_pong;

    friend class CXWM;
};

CX11Drag::CX11Drag(CXWM& wm, xcb_window_t owner, SP<CXWaylandSurface> origin, SP<IPointer> pointer, uint32_t button, uint32_t generation) :
    m_wm(wm), m_owner(owner), m_origin(origin), m_pointer(pointer), m_button(button), m_generation(generation) {
    const auto resource = origin->m_surface.lock();
    const auto cancel   = [&wm] { wm.cancelXDrag(); };
    m_originUnmap       = resource->m_events.unmap.listen(cancel);
    m_originDestroy     = resource->m_events.destroy.listen(cancel);
    m_pointerDestroy    = pointer->m_events.destroy.listen(cancel);
    m_statusTimer       = makeShared<CEventLoopTimer>(
        std::nullopt,
        [this](auto, auto) {
            if (!m_position)
                return;
            m_feedbackCurrent = true;
            // A destination may decide acceptance from a preview read, so give outstanding reads one more period.
            if (m_pendingReads && !m_dropped && m_statusWaits++ == 0) {
                m_statusTimer->updateTimeout(1s);
                return;
            }
            sendStatus(true);
        },
        nullptr);
    m_dropTimer = makeShared<CEventLoopTimer>(
        std::nullopt,
        [this](auto, auto) {
            const auto drag = m_wm.m_xDrag;
            if (drag && m_dropped && m_offerVersion < 3)
                sendDndFinished();
            else
                m_wm.cancelXDrag();
        },
        nullptr);
    g_pEventLoopManager->addTimer(m_statusTimer);
    g_pEventLoopManager->addTimer(m_dropTimer);
}

CX11Drag::~CX11Drag() {
    g_pEventLoopManager->removeTimer(m_statusTimer);
    g_pEventLoopManager->removeTimer(m_dropTimer);
}

std::vector<std::string> CX11Drag::mimes() {
    std::vector<std::string> result;
    result.reserve(m_types.size());
    for (const auto& [mime, atom] : m_types)
        result.push_back(mime);
    return result;
}

bool CX11Drag::ownsSelection(bool requireRelease) {
    const auto claim = m_wm.xDragClaim();
    return claim.owner == m_owner && claim.generation == m_generation && (!requireRelease || claim.released);
}

void CX11Drag::send(const std::string& mime, CFileDescriptor fd) {
    const auto atom = std::ranges::find(m_types, mime, &std::pair<std::string, xcb_atom_t>::first);
    if (atom == m_types.end() || m_finishRequested || m_pendingReads >= 16 || !fd.isValid()) {
        // A refused preview only withholds that preview; a refused read after the drop fails the drag.
        if (m_dropped)
            m_readFailed = true;
        return;
    }
    if (!ownsSelection()) {
        cancelled();
        return;
    }

    ++m_pendingReads;
    updateDropDeadline();
    m_wm.receiveSelection(
        m_wm.m_dndSelection, HYPRATOMS["XdndSelection"], atom->second, m_selectionTime, std::move(fd),
        [drag = WP<CX11Drag>{m_wm.m_xDrag}, mime, dropRead = m_dropped](bool success) {
            if (const auto source = drag.lock()) {
                if (!success) {
                    if (dropRead)
                        source->m_readFailed = true;
                } else if (dropRead)
                    source->m_dropReadSucceeded = true;
                else
                    source->m_previewMimes.insert(mime);
                --source->m_pendingReads;
                source->updateDropDeadline();
                source->tryStatus();
                source->complete();
            }
        },
        m_generation);
}

void CX11Drag::accepted(const std::string& mime) {
    if (m_dropped)
        return;
    m_acceptedMime = std::ranges::find(m_types, mime, &std::pair<std::string, xcb_atom_t>::first) != m_types.end() ? mime : "";
    reevaluateStatus();
}

void CX11Drag::cancelled() {
    m_wm.cancelXDrag();
}

bool CX11Drag::hasDnd() {
    return true;
}

bool CX11Drag::dndDone() {
    return m_dropped;
}

void CX11Drag::error(uint32_t code, const std::string& msg) {
    LOG(Log::ERR, "X11 drag error {}: {}", code, msg);
    cancelled();
}

void CX11Drag::sendDndFinished() {
    if (!m_dropped || m_finishRequested)
        return;
    m_finishRequested = true;
    complete();
}

eDataSourceType CX11Drag::type() {
    return DATA_SOURCE_TYPE_X11;
}

uint32_t CX11Drag::actions() {
    return m_actions;
}

uint32_t CX11Drag::preferredDndAction() {
    return m_requested;
}

void CX11Drag::sendDndAction(wl_data_device_manager_dnd_action action) {
    if (m_dropped)
        return;
    m_selected = action;
    reevaluateStatus();
}

void CX11Drag::setProxyBox(std::optional<CBox> box) {
    if (box == m_proxyBox)
        return;
    const auto conn  = m_wm.getConnection();
    const auto proxy = m_wm.m_dndSelection.window;
    if (box) {
        const uint32_t values[] = {sc<uint32_t>(sc<int32_t>(box->x)), sc<uint32_t>(sc<int32_t>(box->y)), sc<uint32_t>(box->w), sc<uint32_t>(box->h), XCB_STACK_MODE_ABOVE};
        xcb_configure_window(conn, proxy, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_STACK_MODE, values);
        if (!m_proxyBox)
            xcb_map_window(conn, proxy);
    } else
        xcb_unmap_window(conn, proxy);
    m_proxyBox = box;
    xcb_flush(conn);
}

void CX11Drag::motion(SP<CWLSurfaceResource> surface, PHLMONITOR monitor, const Vector2D& point, const Vector2D& local, uint32_t time) {
    const auto origin = m_origin.lock();
    if (!origin || !origin->m_surface || g_pSeatManager->m_state.pointerFocus != origin->m_surface) {
        cancelled();
        return;
    }

    const bool nativeTarget = surface && surface->client() != origin->m_surface->client();
    if (m_target != (nativeTarget ? surface : nullptr)) {
        leave();
        m_targetUnmap.reset();
        m_targetDestroy.reset();
        m_target = nativeTarget ? surface : nullptr;
        if (m_target) {
            const auto cancel = [wm = &m_wm] { wm->cancelXDrag(); };
            m_targetUnmap     = surface->m_events.unmap.listen(cancel);
            m_targetDestroy   = surface->m_events.destroy.listen(cancel);
        }
    }
    m_local      = local;
    m_motionTime = time;
    if (nativeTarget) {
        const auto     topLeft = g_pXWaylandManager->waylandToXWaylandCoords(point - local, monitor).floor();
        const auto     farEdge = g_pXWaylandManager->waylandToXWaylandCoords(point - local + surface->m_current.size, monitor);
        const Vector2D bottomRight{std::ceil(farEdge.x), std::ceil(farEdge.y)};
        setProxyBox(CBox{topLeft, bottomRight - topLeft});
    } else
        setProxyBox(std::nullopt);
    if (!nativeTarget)
        m_entered = false;

    // XWayland needs motion on the original wl_pointer focus to drive its X pointer grab.
    m_xPoint = g_pXWaylandManager->waylandToXWaylandCoords(point, monitor).floor();
    g_pSeatManager->sendPointerMotion(time, m_xPoint - origin->m_geometry.pos());
    g_pSeatManager->sendPointerFrame();
}

void CX11Drag::release(uint32_t time) {
    const auto origin = m_origin.lock();
    if (!origin || !origin->m_surface || g_pSeatManager->m_state.pointerFocus != origin->m_surface) {
        cancelled();
        return;
    }
    m_released = true;
    m_dropTimer->updateTimeout(2s);
    g_pSeatManager->sendPointerButton(time, m_button, WL_POINTER_BUTTON_STATE_RELEASED);
    g_pSeatManager->sendPointerFrame();
    wl_client_flush(origin->m_surface->client());
    setProxyBox(std::nullopt);
}

bool CX11Drag::watchSourceWindow(xcb_window_t window) {
    const auto                                                                conn             = m_wm.getConnection();
    const auto                                                                attributesCookie = xcb_get_window_attributes(conn, window);
    const auto                                                                treeCookie       = xcb_query_tree(conn, window);
    const std::unique_ptr<xcb_get_window_attributes_reply_t, decltype(&free)> attributes(xcb_get_window_attributes_reply(conn, attributesCookie, nullptr), free);
    const std::unique_ptr<xcb_query_tree_reply_t, decltype(&free)>            tree(xcb_query_tree_reply(conn, treeCookie, nullptr), free);
    if (!attributes || !tree)
        return false;
    // The root's SubstructureNotify already reports the destruction of top-level windows.
    if (tree->parent == m_wm.m_screen->root || (attributes->your_event_mask & XCB_EVENT_MASK_STRUCTURE_NOTIFY))
        return true;

    const uint32_t mask = attributes->your_event_mask | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(conn, window, XCB_CW_EVENT_MASK, &mask);
    const std::unique_ptr<xcb_get_window_attributes_reply_t, decltype(&free)> watched(xcb_get_window_attributes_reply(conn, xcb_get_window_attributes(conn, window), nullptr),
                                                                                      free);
    if (!watched)
        return false;
    m_watchedSource = window;
    m_watchedMask   = attributes->your_event_mask;
    return true;
}

void CX11Drag::unwatchSourceWindow() {
    if (!m_watchedSource)
        return;
    xcb_change_window_attributes(m_wm.getConnection(), m_watchedSource, XCB_CW_EVENT_MASK, &m_watchedMask);
    m_watchedSource = XCB_WINDOW_NONE;
}

void CX11Drag::enter(const xcb_client_message_data_t& data) {
    leave();
    unwatchSourceWindow();
    m_version      = data.data32[1] >> 24;
    m_entered      = m_version >= 3 && m_version <= XDND_VERSION && watchSourceWindow(data.data32[0]);
    m_sourceWindow = m_entered ? data.data32[0] : xcb_window_t{XCB_WINDOW_NONE};
    m_types.clear();
    if (!m_entered)
        return;

    const auto conn    = m_wm.getConnection();
    const auto atoms   = data.data32[1] & 1 ? atomList(conn, m_sourceWindow, HYPRATOMS["XdndTypeList"]) : std::vector<xcb_atom_t>{data.data32[2], data.data32[3], data.data32[4]};
    const auto addType = [this](std::string mime, xcb_atom_t atom) {
        if (std::ranges::find(m_types, mime, &std::pair<std::string, xcb_atom_t>::first) == m_types.end())
            m_types.emplace_back(std::move(mime), atom);
    };
    for (auto atom : atoms) {
        if (!atom)
            continue;
        auto mime = m_wm.mimeFromAtom(atom);
        if (mime.contains('/'))
            addType(std::move(mime), atom);
    }
    if (std::ranges::contains(atoms, HYPRATOMS["UTF8_STRING"]))
        addType("text/plain", HYPRATOMS["UTF8_STRING"]);
    else if (std::ranges::contains(atoms, HYPRATOMS["TEXT"]))
        addType("text/plain", HYPRATOMS["TEXT"]);

    const auto actionAtoms = atomList(conn, m_sourceWindow, HYPRATOMS["XdndActionList"]);
    m_actions              = actionAtoms.empty() ? COPY | MOVE : 0;
    for (const auto action : actionAtoms)
        m_actions |= dragAction(action);
    if (m_version < 5)
        m_actions &= COPY;
}

void CX11Drag::updateOffer() {
    const auto target = m_target.lock();
    if (!m_entered || !target || m_types.empty())
        return;

    if (!m_offerVersion) {
        m_offerVersion = PROTO::data->enterXDrag(m_wm.m_xDrag, target, m_local);
        if (m_offerVersion)
            ++m_offerSerial;
    }
    if (m_offerVersion)
        PROTO::data->updateXDrag(m_motionTime, m_local);
}

void CX11Drag::leave() {
    sendStatus(false);
    if (m_offerVersion)
        PROTO::data->leaveXDrag();
    m_offerVersion = 0;
    m_acceptedMime.clear();
    m_selected       = 0;
    m_reportedAction = 0;
    m_reportedPoint.reset();
    m_pendingReads = 0;
    m_readFailed   = false;
    m_previewMimes.clear();
    std::erase_if(m_wm.m_dndSelection.transfers, [](const auto& transfer) { return transfer->incomingWindow != XCB_WINDOW_NONE; });
}

void CX11Drag::position(const xcb_client_message_data_t& data) {
    sendStatus(false);
    const Vector2D point{sc<double>(data.data32[2] >> 16), sc<double>(data.data32[2] & 0xffff)};
    m_selectionTime = data.data32[3];
    m_requested     = dragAction(data.data32[4]);
    m_position      = SPosition{.offer = m_offerSerial, .local = m_local, .point = point, .action = m_requested};
    if (point != m_xPoint || (!m_released && !m_proxyBox)) {
        sendStatus(false);
        return;
    }

    // After release, only the frozen target and point remain eligible, and no new motion is sent.
    if (!m_released)
        updateOffer();
    if (!m_offerVersion) {
        sendStatus(true);
        return;
    }
    m_position->offer = m_offerSerial;
    awaitFeedback();
}

void CX11Drag::awaitFeedback() {
    static uint32_t nextPing = 0x58444e44;
    m_feedbackCurrent        = false;
    m_statusWaits            = 0;
    m_pong.reset();
    const auto target = m_target.lock();
    const auto base   = target && PROTO::xdgShell ? PROTO::xdgShell->wmBaseForClient(target->client()) : nullptr;
    if (!base) {
        m_statusTimer->updateTimeout(100ms);
        return;
    }

    // The client answers a ping only after handling every earlier event, including this motion.
    m_pingSerial = ++nextPing;
    m_pong       = base->m_events.pongSerial.listen([this](uint32_t serial) {
        if (serial != m_pingSerial)
            return;
        m_feedbackCurrent = true;
        tryStatus();
    });
    base->ping(m_pingSerial);
    m_statusTimer->updateTimeout(1s);
}

void CX11Drag::tryStatus() {
    if (m_position && m_feedbackCurrent && (!m_pendingReads || m_dropped))
        sendStatus(true);
}

void CX11Drag::drop(xcb_timestamp_t time) {
    if (m_receivedDrop || !m_released)
        return;
    m_receivedDrop = true;
    m_dropped      = acceptsDrop() && !m_position && m_reportedAction && m_reportedAction == m_selected && m_reportedPoint == m_xPoint && m_acceptedMime == m_reportedMime &&
        ownsSelection(true) && PROTO::data->dropXDrag();
    if (!m_dropped) {
        cancelled();
        return;
    }

    m_selectionTime = time;
    setProxyBox(std::nullopt);
    updateDropDeadline();
}

void CX11Drag::sendMessage(xcb_atom_t type, uint32_t flags, xcb_atom_t action) {
    sendXdndMessage(m_wm.getConnection(), m_wm.m_dndSelection.window, m_sourceWindow, type, flags, action);
}

uint32_t CX11Drag::statusAction(uint32_t requested) const {
    return acceptsDrop() && (m_selected == COPY || (m_version >= 5 && m_selected == requested)) ? m_selected : 0;
}

bool CX11Drag::acceptsDrop() const {
    // Wayland v1 and v2 destinations use implicit COPY, so their acceptance is only feedback.
    return m_offerVersion && (m_offerVersion < 3 || !m_acceptedMime.empty());
}

void CX11Drag::sendStatus(bool eligible) {
    if (!m_position)
        return;

    const auto position = *m_position;
    m_position.reset();
    m_pong.reset();
    m_statusTimer->updateTimeout(std::nullopt);
    const bool current = eligible && m_offerVersion && position.offer == m_offerSerial && position.local == m_local && position.point == m_xPoint;
    reportStatus(current ? statusAction(position.action) : 0, position.point);
}

void CX11Drag::reportStatus(uint32_t action, const Vector2D& point) {
    m_reportedAction = action;
    m_reportedPoint  = point;
    m_reportedMime   = m_acceptedMime;
    sendMessage(HYPRATOMS["XdndStatus"], 2 | (action != 0), dragActionAtom(action));
}

void CX11Drag::reevaluateStatus() {
    if (m_position || m_receivedDrop || !m_entered || !m_offerVersion || m_reportedPoint != m_xPoint)
        return;
    // A destination that settles after the last status gets a fresh status for the unchanged point.
    const auto action = statusAction(m_requested);
    if (action != m_reportedAction || m_acceptedMime != m_reportedMime)
        reportStatus(action, *m_reportedPoint);
}

void CX11Drag::updateDropDeadline() {
    if (!m_dropped)
        return;
    // Reads may progress slowly, but the whole drop must finish within one budget from the drop.
    if (!m_dropStarted)
        m_dropStarted = Time::steadyNow();
    const auto LEFT = std::max<Time::steady_dur>(*m_dropStarted + 2min - Time::steadyNow(), 0s);
    m_dropTimer->updateTimeout(m_pendingReads || m_deleting ? LEFT : std::min<Time::steady_dur>(LEFT, 10s));
}

void CX11Drag::complete() {
    if (!m_finishRequested || m_pendingReads || m_deleting)
        return;
    const bool cachedRead    = m_acceptedMime.empty() ? !m_previewMimes.empty() : m_previewMimes.contains(m_acceptedMime);
    const bool readSucceeded = cachedRead || m_dropReadSucceeded;
    if (!readSucceeded || m_readFailed || !ownsSelection()) {
        cancelled();
        return;
    }
    if (m_reportedAction == MOVE) {
        m_deleting = true;
        updateDropDeadline();
        m_wm.receiveSelection(
            m_wm.m_dndSelection, HYPRATOMS["XdndSelection"], HYPRATOMS["DELETE"], m_selectionTime, {}, [wm = &m_wm](bool success) { wm->endXDrag(success); }, m_generation);
        return;
    }
    m_wm.endXDrag(true);
}

void CX11Drag::end(bool success, bool notifySource) {
    if (notifySource && m_receivedDrop)
        sendMessage(HYPRATOMS["XdndFinished"], m_version >= 5 && success, m_version >= 5 && success ? dragActionAtom(m_reportedAction) : xcb_atom_t{XCB_ATOM_NONE});
    else if (notifySource && m_entered) {
        // The source may still drop on its last accepting status, so it learns of the refusal and keeps a receipt.
        if (m_position)
            sendStatus(false);
        else if (m_reportedAction)
            reportStatus(0, m_reportedPoint.value_or(m_xPoint));
        m_wm.m_xDragReceipt = CXWM::SXDragReceipt{.source = m_sourceWindow, .owner = m_owner, .generation = m_generation, .expires = Time::steadyNow() + 10s};
    }
    m_position.reset();
    leave();
    unwatchSourceWindow();
    setProxyBox(std::nullopt);
}

void CX11Drag::handleMessage(const xcb_client_message_event_t& event) {
    if (m_receivedDrop)
        return;
    const auto& data = event.data;
    if (event.type == HYPRATOMS["XdndEnter"]) {
        if (m_proxyBox && !m_released)
            enter(data);
    } else if (event.type == HYPRATOMS["XdndPosition"] && m_entered)
        position(data);
    else if (event.type == HYPRATOMS["XdndLeave"]) {
        leave();
        m_entered = false;
        if (m_released)
            cancelled();
    } else if (event.type == HYPRATOMS["XdndDrop"] && m_entered)
        drop(data.data32[2]);
}

bool CXWM::xDragActive() const {
    return m_xDrag && !m_xDrag->m_released;
}

bool CXWM::xDragBlocksNewDrag() const {
    return m_xDrag && !m_xDrag->m_dropped;
}

bool CXWM::routeXDragMotion(SP<CWLSurfaceResource> surface, PHLMONITOR monitor, const Vector2D& point, const Vector2D& local, uint32_t time) {
    if (!xDragActive())
        return false;
    const auto drag = m_xDrag;
    drag->motion(surface, monitor, point, local, time);
    return xDragActive();
}

bool CXWM::routeXDragButton(const IPointer::SButtonEvent& event, SP<IPointer> mouse, SP<CWLSurfaceResource> pressOrigin) {
    if (!xDragActive())
        return false;
    const auto drag = m_xDrag;
    if (event.state != WL_POINTER_BUTTON_STATE_RELEASED)
        return true;
    if (drag->m_pointer == mouse && drag->m_button == event.button)
        drag->release(event.timeMs);
    else if (pressOrigin && g_pSeatManager->m_state.pointerFocus == pressOrigin) {
        g_pSeatManager->sendPointerButton(event.timeMs, event.button, WL_POINTER_BUTTON_STATE_RELEASED);
        g_pSeatManager->sendPointerFrame();
    }
    return true;
}

void CXWM::cancelXDragForPointer(SP<IPointer> pointer) {
    if (xDragActive() && m_xDrag->m_pointer == pointer)
        cancelXDrag();
}

void CXWM::endXDrag(bool success, bool notifySource) {
    if (const auto drag = std::move(m_xDrag))
        drag->end(success, notifySource);
}

void CXWM::cancelXDrag() {
    endXDrag(false);
}

void CXWM::handleXDragOwner(const xcb_xfixes_selection_notify_event_t& event) {
    endXDrag(false, false);
    m_xDragReceipt.reset();
    if (!event.owner || event.owner == m_dndSelection.window || PROTO::data->dndActive() || g_pSeatManager->m_seatGrab || g_pSessionLockManager->isSessionLocked())
        return;

    const auto claim = xDragClaim();
    if (claim.owner != event.owner || claim.timestamp != event.selection_timestamp || claim.released)
        return;
    const auto originResource = g_pSeatManager->m_state.pointerFocus.lock();
    const auto origin         = originResource ? windowForWayland(originResource) : nullptr;
    if (!origin || !g_pSeatManager->pointerButtonSerialValid(g_pSeatManager->m_state.pointerFocusResource.lock(), claim.serial, originResource, false))
        return;

    for (const auto& held : g_pInputManager->m_currentlyHeldButtons | std::views::reverse) {
        const auto pointer = held.pointer.lock();
        if (!pointer || held.origin != originResource || held.button != claim.button)
            continue;
        m_xDrag = makeShared<CX11Drag>(*this, claim.owner, origin, pointer, held.button, claim.generation);
        g_pInputManager->simulateMouseMovement();
        return;
    }
}

void CXWM::handleXDragWindowDestroyed(xcb_window_t window) {
    if (m_xDragReceipt && m_xDragReceipt->source == window)
        m_xDragReceipt.reset();
    if (!m_xDrag || m_xDrag->m_sourceWindow != window)
        return;
    m_xDrag->m_watchedSource = XCB_WINDOW_NONE;
    endXDrag(false, false);
}

bool CXWM::answerCancelledDrop(const xcb_client_message_event_t& event) {
    if (!m_xDragReceipt || event.data.data32[0] != m_xDragReceipt->source)
        return false;
    const auto receipt = *m_xDragReceipt;
    if (Time::steadyNow() > receipt.expires || event.type == HYPRATOMS["XdndLeave"]) {
        m_xDragReceipt.reset();
        return true;
    }
    if (event.type == HYPRATOMS["XdndPosition"])
        sendXdndMessage(getConnection(), m_dndSelection.window, receipt.source, HYPRATOMS["XdndStatus"], 0, XCB_ATOM_NONE);
    else if (event.type == HYPRATOMS["XdndDrop"]) {
        m_xDragReceipt.reset();
        const auto claim = xDragClaim();
        if (claim.owner == receipt.owner && claim.generation == receipt.generation)
            sendXdndMessage(getConnection(), m_dndSelection.window, receipt.source, HYPRATOMS["XdndFinished"], 0, XCB_ATOM_NONE);
    }
    return true;
}

bool CXWM::handleXDragMessage(const xcb_client_message_event_t& event) {
    if (event.window != m_dndSelection.window)
        return false;
    if (event.type == HYPRATOMS["_ENZIM_XDND_REVOKED"]) {
        // XWayland generates this when the source's drag grab ends; it refuses copies sent by clients.
        if (!(event.response_type & 0x80) && event.format == 32 && xDragActive() && event.data.data32[0] == m_xDrag->m_generation)
            cancelXDrag();
        return true;
    }
    if (event.type != HYPRATOMS["XdndEnter"] && event.type != HYPRATOMS["XdndPosition"] && event.type != HYPRATOMS["XdndLeave"] && event.type != HYPRATOMS["XdndDrop"])
        return false;

    const auto source = event.data.data32[0];
    if (event.format != 32 || !source)
        return true;
    const auto drag = m_xDrag;
    if (!drag || (event.type != HYPRATOMS["XdndEnter"] && source != drag->m_sourceWindow)) {
        answerCancelledDrop(event);
        return true;
    }
    if (drag->ownsSelection())
        drag->handleMessage(event);
    return true;
}

#endif
