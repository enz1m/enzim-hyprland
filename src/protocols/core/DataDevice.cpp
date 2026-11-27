#include "DataDevice.hpp"
#include <algorithm>
#include "../../managers/SeatManager.hpp"
#include "../../pointer/PointerManager.hpp"
#include "../../managers/eventLoop/EventLoopManager.hpp"
#include "../../Compositor.hpp"
#include "../../render/pass/TexPassElement.hpp"
#include "Seat.hpp"
#include "Compositor.hpp"
#include "../../xwayland/XWayland.hpp"
#include "../../xwayland/Server.hpp"
#include "../../managers/input/InputManager.hpp"
#include "../../pointer/cursor/CursorShapeOverrideController.hpp"
#include "../../output/Monitor.hpp"
#include "../../render/Renderer.hpp"
#include "../../xwayland/Dnd.hpp"
#include "../../event/EventBus.hpp"
using namespace Hyprutils::OS;

CWLDataOfferResource::CWLDataOfferResource(SP<CWlDataOffer> resource_, SP<IDataSource> source_) : m_source(source_), m_resource(resource_) {
    if UNLIKELY (!good())
        return;

    m_resource->setDestroy([this](CWlDataOffer* r) { PROTO::data->destroyResource(this); });
    m_resource->setOnDestroy([this](CWlDataOffer* r) { PROTO::data->destroyResource(this); });

    m_resource->setAccept([this](CWlDataOffer* r, uint32_t serial, const char* mime) {
        if (!m_source) {
            LOG(Log::WARN, "Possible bug: Accept on an offer w/o a source");
            return;
        }

        if (m_dead) {
            LOG(Log::WARN, "Possible bug: Accept on an offer that's dead");
            return;
        }

        LOG(Log::DEBUG, "Offer {:x} accepts data from source {:x} with mime {}", (uintptr_t)this, (uintptr_t)m_source.get(), mime ? mime : "null");

        const auto source = m_source.lock();
        m_accepted        = mime;
        source->accepted(mime ? mime : "");
    });

    m_resource->setSetActions([this](CWlDataOffer* r, uint32_t actions, uint32_t preferred) {
        constexpr uint32_t COPY = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
        constexpr uint32_t MOVE = WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
        constexpr uint32_t ASK  = WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
        if (actions & ~(COPY | MOVE | ASK)) {
            r->error(WL_DATA_OFFER_ERROR_INVALID_ACTION_MASK, "Invalid destination drag actions");
            return;
        }
        if (preferred && ((preferred & (preferred - 1)) || !(preferred & actions))) {
            r->error(WL_DATA_OFFER_ERROR_INVALID_ACTION, "Invalid preferred destination drag action");
            return;
        }

        m_destinationActions = actions;
        m_preferredAction    = preferred;
        updateAction();
    });

    m_resource->setReceive([this](CWlDataOffer* r, const char* mime, int fd) {
        CFileDescriptor sendFd{fd};
        if (!m_source) {
            LOG(Log::WARN, "Possible bug: Receive on an offer w/o a source");
            return;
        }

        const auto source = m_source.lock();
        if (source->type() == DATA_SOURCE_TYPE_X11 && source->hasDnd()) {
            if (!m_dead) {
                m_recvd = true;
                source->send(mime ? mime : "", std::move(sendFd));
            }
            return;
        }

        if (m_dead) {
            LOG(Log::WARN, "Possible bug: Receive on an offer that's dead");
            return;
        }

        LOG(Log::DEBUG, "Offer {:x} asks to send data from source {:x}", (uintptr_t)this, (uintptr_t)m_source.get());

        if (!m_accepted) {
            LOG(Log::WARN, "Offer was never accepted, sending accept first");
            m_source->accepted(mime ? mime : "");
        }

        m_source->send(mime ? mime : "", std::move(sendFd));

        m_recvd = true;

        // if (source->hasDnd())
        //     PROTO::data->completeDrag();
    });

    m_resource->setFinish([this](CWlDataOffer* r) {
        if (m_dead)
            return;
        m_dead = true;
        if (const auto source = m_source.lock(); source && source->type() == DATA_SOURCE_TYPE_X11) {
            if (source->dndDone() && m_recvd && m_accepted)
                source->sendDndFinished();
            else
                source->cancelled();
            return;
        }
        if (!m_source || !m_recvd || !m_accepted)
            PROTO::data->abortDrag();
        else
            PROTO::data->completeDrag();
    });
}

CWLDataOfferResource::~CWLDataOfferResource() {
    const auto source = m_source.lock();
    if (!source || !source->hasDnd() || m_dead)
        return;

    if (source->type() != DATA_SOURCE_TYPE_X11)
        source->sendDndFinished();
}

void CWLDataOfferResource::updateAction() {
    const auto source = m_source.lock();
    if (!source || !source->hasDnd() || m_dead || (source->type() == DATA_SOURCE_TYPE_X11 && source->dndDone()))
        return;

    constexpr uint32_t COPY      = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    constexpr uint32_t MOVE      = WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
    const auto         allowed   = source->actions() & m_destinationActions & (COPY | MOVE);
    const auto         requested = source->preferredDndAction();
    const auto         preferred = (requested & allowed) ? requested : m_preferredAction;
    const auto         selected  = sc<wl_data_device_manager_dnd_action>((preferred & allowed) ? preferred : (allowed & COPY) ? COPY : (allowed & MOVE));
    if (m_selectedAction == selected)
        return;

    m_selectedAction = selected;
    if (m_resource->version() >= 3)
        m_resource->sendAction(selected);
    source->sendDndAction(selected);
}

bool CWLDataOfferResource::good() {
    return m_resource->resource();
}

void CWLDataOfferResource::sendData() {
    if (!m_source)
        return;

    const auto SOURCEACTIONS = m_source->actions();

    if (m_source->type() == DATA_SOURCE_TYPE_X11 && m_source->hasDnd()) {
        if (m_resource->version() >= 3) {
            m_destinationActions = 0;
            m_preferredAction    = 0;
            m_resource->sendSourceActions(SOURCEACTIONS);
        } else
            updateAction();
    } else if (m_resource->version() >= 3 && SOURCEACTIONS > 0) {
        m_resource->sendSourceActions(SOURCEACTIONS);
        if (SOURCEACTIONS & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE)
            m_resource->sendAction(WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
        else if (SOURCEACTIONS & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)
            m_resource->sendAction(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
        else {
            LOG(Log::ERR, "Client bug? dnd source has no action move or copy. Sending move, f this.");
            m_resource->sendAction(WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
        }
    }

    for (auto const& m : m_source->mimes()) {
        LOG(Log::DEBUG, " | offer {:x} supports mime {}", (uintptr_t)this, m);
        m_resource->sendOffer(m.c_str());
    }
}

eDataSourceType CWLDataOfferResource::type() {
    return DATA_SOURCE_TYPE_WAYLAND;
}

SP<CWLDataOfferResource> CWLDataOfferResource::getWayland() {
    return m_self.lock();
}

SP<CX11DataOffer> CWLDataOfferResource::getX11() {
    return nullptr;
}

SP<IDataSource> CWLDataOfferResource::getSource() {
    return m_source.lock();
}

CWLDataSourceResource::CWLDataSourceResource(SP<CWlDataSource> resource_, SP<CWLDataDeviceResource> device_) : m_device(device_), m_resource(resource_) {
    if UNLIKELY (!good())
        return;

    m_resource->setData(this);

    m_resource->setDestroy([this](CWlDataSource* r) {
        m_events.destroy.emit();
        PROTO::data->onDestroyDataSource(m_self);
        PROTO::data->destroyResource(this);
    });
    m_resource->setOnDestroy([this](CWlDataSource* r) {
        m_events.destroy.emit();
        PROTO::data->onDestroyDataSource(m_self);
        PROTO::data->destroyResource(this);
    });

    m_resource->setOffer([this](CWlDataSource* r, const char* mime) { m_mimeTypes.emplace_back(mime); });
    m_resource->setSetActions([this](CWlDataSource* r, uint32_t a) {
        LOG(Log::DEBUG, "DataSource {:x} actions {}", (uintptr_t)this, a);
        m_supportedActions = a;
    });
}

CWLDataSourceResource::~CWLDataSourceResource() {
    m_events.destroy.emit();
    PROTO::data->onDestroyDataSource(m_self);
}

SP<CWLDataSourceResource> CWLDataSourceResource::fromResource(wl_resource* res) {
    auto data = sc<CWLDataSourceResource*>(sc<CWlDataSource*>(wl_resource_get_user_data(res))->data());
    return data ? data->m_self.lock() : nullptr;
}

bool CWLDataSourceResource::good() {
    return m_resource->resource();
}

void CWLDataSourceResource::accepted(const std::string& mime) {
    if (mime.empty()) {
        m_resource->sendTarget(nullptr);
        return;
    }

    if (std::ranges::find(m_mimeTypes, mime) == m_mimeTypes.end()) {
        LOG(Log::ERR, "Compositor/App bug: CWLDataSourceResource::sendAccepted with non-existent mime");
        return;
    }

    m_resource->sendTarget(mime.c_str());
}

std::vector<std::string> CWLDataSourceResource::mimes() {
    return m_mimeTypes;
}

void CWLDataSourceResource::send(const std::string& mime, CFileDescriptor fd) {
    if (std::ranges::find(m_mimeTypes, mime) == m_mimeTypes.end()) {
        LOG(Log::ERR, "Compositor/App bug: CWLDataSourceResource::sendAskSend with non-existent mime");
        return;
    }

    m_resource->sendSend(mime.c_str(), fd.get());
}

void CWLDataSourceResource::cancelled() {
    m_resource->sendCancelled();
}

bool CWLDataSourceResource::hasDnd() {
    return m_dnd;
}

bool CWLDataSourceResource::dndDone() {
    return m_dndSuccess;
}

void CWLDataSourceResource::error(uint32_t code, const std::string& msg) {
    m_resource->error(code, msg);
}

void CWLDataSourceResource::sendDndDropPerformed() {
    if (m_resource->version() < 3)
        return;
    m_resource->sendDndDropPerformed();
    m_dropped = true;
}

void CWLDataSourceResource::sendDndFinished() {
    if (m_resource->version() < 3)
        return;
    m_resource->sendDndFinished();
}

void CWLDataSourceResource::sendDndAction(wl_data_device_manager_dnd_action a) {
    if (m_resource->version() < 3)
        return;
    m_resource->sendAction(a);
}

uint32_t CWLDataSourceResource::actions() {
    return m_supportedActions;
}

eDataSourceType CWLDataSourceResource::type() {
    return DATA_SOURCE_TYPE_WAYLAND;
}

CWLDataDeviceResource::CWLDataDeviceResource(SP<CWlDataDevice> resource_) : m_resource(resource_) {
    if UNLIKELY (!good())
        return;

    m_resource->setRelease([this](CWlDataDevice* r) { PROTO::data->destroyResource(this); });
    m_resource->setOnDestroy([this](CWlDataDevice* r) { PROTO::data->destroyResource(this); });

    m_client = m_resource->client();

    m_resource->setSetSelection([](CWlDataDevice* r, wl_resource* sourceR, uint32_t serial) {
        auto source = sourceR ? CWLDataSourceResource::fromResource(sourceR) : CSharedPointer<CWLDataSourceResource>{};
        if (!source) {
            LOG(Log::DEBUG, "Reset selection received");
            g_pSeatManager->setCurrentSelection(nullptr);
            return;
        }

        if (source && source->m_used)
            LOG(Log::WARN, "setSelection on a used resource. By protocol, this is a violation, but firefox et al insist on doing this.");

        source->markUsed();

        g_pSeatManager->setCurrentSelection(source);
    });

    m_resource->setStartDrag([](CWlDataDevice* r, wl_resource* sourceR, wl_resource* origin, wl_resource* icon, uint32_t serial) {
        const auto originSurface = origin ? CWLSurfaceResource::fromResource(origin) : nullptr;
#ifndef NO_XWAYLAND
        if (g_pXWayland && g_pXWayland->m_wm && g_pXWayland->m_wm->xDragBlocksNewDrag()) {
            const auto seat = g_pSeatManager->m_state.pointerFocusResource.lock();
            if (g_pXWayland->m_wm->xDragActive() || !originSurface || originSurface->client() != r->client() ||
                !g_pSeatManager->pointerButtonSerialValid(seat, serial, originSurface, false))
                return;
            g_pXWayland->m_wm->cancelXDrag();
        }
#endif
        auto source = CWLDataSourceResource::fromResource(sourceR);
        if (!source) {
            LOG(Log::ERR, "No source in drag");
            return;
        }

        if (source && source->m_used)
            LOG(Log::WARN, "setSelection on a used resource. By protocol, this is a violation, but firefox et al insist on doing this.");

        source->markUsed();

        source->m_dnd = true;

        PROTO::data->initiateDrag(source, icon ? CWLSurfaceResource::fromResource(icon) : nullptr, originSurface);
    });
}

bool CWLDataDeviceResource::good() {
    return m_resource->resource();
}

wl_client* CWLDataDeviceResource::client() {
    return m_client;
}

void CWLDataDeviceResource::sendDataOffer(SP<IDataOffer> offer) {
    if (!offer)
        m_resource->sendDataOfferRaw(nullptr);
    else if (const auto WL = offer->getWayland(); WL)
        m_resource->sendDataOffer(WL->m_resource.get());
    //FIXME: X11
}

void CWLDataDeviceResource::sendEnter(uint32_t serial, SP<CWLSurfaceResource> surf, const Vector2D& local, SP<IDataOffer> offer) {
    if (const auto WL = offer->getWayland(); WL)
        m_resource->sendEnterRaw(serial, surf->getResource()->resource(), wl_fixed_from_double(local.x), wl_fixed_from_double(local.y), WL->m_resource->resource());

    m_entered = surf;

    // FIXME: X11
}

void CWLDataDeviceResource::sendLeave() {
    if (!m_entered)
        return;

    m_entered.reset();
    m_resource->sendLeave();
}

void CWLDataDeviceResource::sendMotion(uint32_t timeMs, const Vector2D& local) {
    m_resource->sendMotion(timeMs, wl_fixed_from_double(local.x), wl_fixed_from_double(local.y));
}

void CWLDataDeviceResource::sendDrop() {
    m_resource->sendDrop();
}

void CWLDataDeviceResource::sendSelection(SP<IDataOffer> offer) {
    if (!offer)
        m_resource->sendSelectionRaw(nullptr);
    else if (const auto WL = offer->getWayland(); WL)
        m_resource->sendSelection(WL->m_resource.get());
}

eDataSourceType CWLDataDeviceResource::type() {
    return DATA_SOURCE_TYPE_WAYLAND;
}

SP<CWLDataDeviceResource> CWLDataDeviceResource::getWayland() {
    return m_self.lock();
}

SP<CX11DataDevice> CWLDataDeviceResource::getX11() {
    return nullptr;
}

CWLDataDeviceManagerResource::CWLDataDeviceManagerResource(SP<CWlDataDeviceManager> resource_) : m_resource(resource_) {
    if UNLIKELY (!good())
        return;

    m_resource->setOnDestroy([this](CWlDataDeviceManager* r) { PROTO::data->destroyResource(this); });

    m_resource->setCreateDataSource([this](CWlDataDeviceManager* r, uint32_t id) {
        std::erase_if(m_sources, [](const auto& e) { return e.expired(); });

        const auto RESOURCE = PROTO::data->m_sources.emplace_back(makeShared<CWLDataSourceResource>(makeShared<CWlDataSource>(r->client(), r->version(), id), m_device.lock()));

        if UNLIKELY (!RESOURCE->good()) {
            r->noMemory();
            PROTO::data->m_sources.pop_back();
            return;
        }

        if (!m_device)
            LOG(Log::WARN, "New data source before a device was created");

        RESOURCE->m_self = RESOURCE;

        m_sources.emplace_back(RESOURCE);

        LOG(Log::DEBUG, "New data source bound at {:x}", (uintptr_t)RESOURCE.get());
    });

    m_resource->setGetDataDevice([this](CWlDataDeviceManager* r, uint32_t id, wl_resource* seat) {
        const auto RESOURCE = PROTO::data->m_devices.emplace_back(makeShared<CWLDataDeviceResource>(makeShared<CWlDataDevice>(r->client(), r->version(), id)));

        if UNLIKELY (!RESOURCE->good()) {
            r->noMemory();
            PROTO::data->m_devices.pop_back();
            return;
        }

        RESOURCE->m_self = RESOURCE;

        for (auto const& s : m_sources) {
            if (!s)
                continue;
            s->m_device = RESOURCE;
        }

        LOG(Log::DEBUG, "New data device bound at {:x}", (uintptr_t)RESOURCE.get());
    });
}

bool CWLDataDeviceManagerResource::good() {
    return m_resource->resource();
}

CWLDataDeviceProtocol::CWLDataDeviceProtocol(const wl_interface* iface, const int& ver, const std::string& name) : IWaylandProtocol(iface, ver, name) {
    g_pEventLoopManager->doLater([this]() {
        m_listeners.onKeyboardFocusChange   = g_pSeatManager->m_events.keyboardFocusChange.listen([this] { onKeyboardFocus(); });
        m_listeners.onDndPointerFocusChange = g_pSeatManager->m_events.dndPointerFocusChange.listen([this] { onDndPointerFocus(); });
    });
}

void CWLDataDeviceProtocol::bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) {
    const auto RESOURCE = m_managers.emplace_back(makeShared<CWLDataDeviceManagerResource>(makeShared<CWlDataDeviceManager>(client, ver, id)));

    if UNLIKELY (!RESOURCE->good()) {
        wl_client_post_no_memory(client);
        m_managers.pop_back();
        return;
    }

    LOG(Log::DEBUG, "New datamgr resource bound at {:x}", (uintptr_t)RESOURCE.get());
}

void CWLDataDeviceProtocol::destroyResource(CWLDataDeviceManagerResource* seat) {
    std::erase_if(m_managers, [&](const auto& other) { return other.get() == seat; });
}

void CWLDataDeviceProtocol::destroyResource(CWLDataDeviceResource* resource) {
    if (m_xDnd.device.get() == resource) {
        m_xDnd.device.reset();
        if (const auto source = m_xDnd.source.lock())
            source->cancelled();
    }
    std::erase_if(m_devices, [&](const auto& other) { return other.get() == resource; });
}

void CWLDataDeviceProtocol::destroyResource(CWLDataSourceResource* resource) {
    std::erase_if(m_sources, [&](const auto& other) { return other.get() == resource; });
}

void CWLDataDeviceProtocol::destroyResource(CWLDataOfferResource* resource) {
    if (m_xDnd.offer.get() == resource && !resource->m_dead) {
        resource->m_dead = true;
        if (const auto source = resource->m_source.lock()) {
            if (resource->m_resource->version() < 3 && resource->m_recvd && source->dndDone())
                source->sendDndFinished();
            else
                source->cancelled();
        }
    }
    std::erase_if(m_offers, [&](const auto& other) { return other.get() == resource; });
}

SP<IDataDevice> CWLDataDeviceProtocol::dataDeviceForClient(wl_client* c) {
#ifndef NO_XWAYLAND
    if (g_pXWayland && g_pXWayland->m_server && c == g_pXWayland->m_server->m_xwaylandClient)
        return g_pXWayland->m_wm->getDataDevice();
#endif

    auto it = std::ranges::find_if(m_devices, [c](const auto& e) { return e->client() == c; });
    if (it == m_devices.end())
        return nullptr;
    return *it;
}

void CWLDataDeviceProtocol::sendSelectionToDevice(SP<IDataDevice> dev, SP<IDataSource> sel) {
    if (dev->type() != DATA_SOURCE_TYPE_WAYLAND)
        return;

    if (!sel) {
        dev->sendSelection(nullptr);
        return;
    }

    SP<IDataOffer> offer;

    if (const auto WL = dev->getWayland(); WL) {
        const auto OFFER = m_offers.emplace_back(makeShared<CWLDataOfferResource>(makeShared<CWlDataOffer>(WL->m_resource->client(), WL->m_resource->version(), 0), sel));
        if UNLIKELY (!OFFER->good()) {
            WL->m_resource->noMemory();
            m_offers.pop_back();
            return;
        }
        OFFER->m_source = sel;
        OFFER->m_self   = OFFER;
        offer           = OFFER;
    }

    if UNLIKELY (!offer) {
        LOG(Log::ERR, "No offer could be created in sendSelectionToDevice");
        return;
    }

    LOG(Log::DEBUG, "New {} offer {:x} for data source {:x}", offer->type() == DATA_SOURCE_TYPE_WAYLAND ? "wayland" : "X11", (uintptr_t)offer.get(), (uintptr_t)sel.get());

    dev->sendDataOffer(offer);
    if (const auto WL = offer->getWayland(); WL)
        WL->sendData();
    dev->sendSelection(offer);
}

void CWLDataDeviceProtocol::onDestroyDataSource(WP<CWLDataSourceResource> source) {
    if (m_dnd.currentSource == source)
        abortDrag();
}

void CWLDataDeviceProtocol::setSelection(SP<IDataSource> source) {
    for (auto const& o : m_offers) {
        if (o->m_source && o->m_source->hasDnd())
            continue;
        o->m_dead = true;
    }

    if (!source) {
        LOG(Log::DEBUG, "resetting selection");

        if (!g_pSeatManager->m_state.keyboardFocusResource)
            return;

        auto DESTDEVICE = dataDeviceForClient(g_pSeatManager->m_state.keyboardFocusResource->client());
        if (DESTDEVICE && DESTDEVICE->type() == DATA_SOURCE_TYPE_WAYLAND)
            sendSelectionToDevice(DESTDEVICE, nullptr);

        return;
    }

    LOG(Log::DEBUG, "New selection for data source {:x}", (uintptr_t)source.get());

    if (!g_pSeatManager->m_state.keyboardFocusResource)
        return;

    auto DESTDEVICE = dataDeviceForClient(g_pSeatManager->m_state.keyboardFocusResource->client());

    if (!DESTDEVICE) {
        LOG(Log::DEBUG, "CWLDataDeviceProtocol::setSelection: cannot send selection to a client without a data_device");
        return;
    }

    if (DESTDEVICE->type() != DATA_SOURCE_TYPE_WAYLAND) {
        LOG(Log::DEBUG, "CWLDataDeviceProtocol::setSelection: ignoring X11 data device");
        return;
    }

    sendSelectionToDevice(DESTDEVICE, source);
}

void CWLDataDeviceProtocol::updateSelection() {
    if (!g_pSeatManager->m_state.keyboardFocusResource)
        return;

    auto DESTDEVICE = dataDeviceForClient(g_pSeatManager->m_state.keyboardFocusResource->client());

    if (!DESTDEVICE) {
        LOG(Log::DEBUG, "CWLDataDeviceProtocol::onKeyboardFocus: cannot send selection to a client without a data_device");
        return;
    }

    sendSelectionToDevice(DESTDEVICE, g_pSeatManager->m_selection.currentSelection.lock());
}

void CWLDataDeviceProtocol::onKeyboardFocus() {
    for (auto const& o : m_offers) {
        if (o->m_source && o->m_source->hasDnd())
            continue;
        o->m_dead = true;
    }

    updateSelection();
}

void CWLDataDeviceProtocol::onDndPointerFocus() {
    for (auto const& o : m_offers) {
        if (m_xDnd.offer == o || (o->m_source && !o->m_source->hasDnd()))
            continue;
        o->m_dead = true;
    }

    updateDrag();
}

static std::optional<Vector2D> dragSurfaceLocal(SP<CWLSurfaceResource> surface, const Vector2D& position) {
    const auto view = Desktop::View::CWLSurface::fromResource(surface);
    if (const auto box = view ? view->getSurfaceBoxGlobal() : std::nullopt)
        return position - box->pos();
    return std::nullopt;
}

void CWLDataDeviceProtocol::initiateDrag(WP<CWLDataSourceResource> currentSource, SP<CWLSurfaceResource> dragSurface, SP<CWLSurfaceResource> origin) {

    if (m_dnd.currentSource) {
        LOG(Log::WARN, "New drag started while old drag still active??");
        abortDrag();
    }

    Pointer::Cursor::overrideController->setOverride("grabbing", Pointer::Cursor::CURSOR_OVERRIDE_DND);
    m_dnd.overriddenCursor = true;

    // For touch-initiated drags, anchor the drag icon to the touch point
    // right away. Until the first touch motion arrives it would otherwise
    // be rendered at the (possibly far away) mouse cursor position.
    if (g_pInputManager->m_lastInputTouch)
        m_dnd.touchPos = g_pInputManager->m_touchData.lastTouchPos;

    LOG(Log::DEBUG, "initiateDrag: source {:x}, surface: {:x}, origin: {:x}", (uintptr_t)currentSource.get(), (uintptr_t)dragSurface.get(), (uintptr_t)origin.get());

    currentSource->m_used = true;

    m_dnd.currentSource = currentSource;
    m_dnd.originSurface = origin;
    m_dnd.dndSurface    = dragSurface;
    if (dragSurface) {
        m_dnd.dndSurfaceDestroy = dragSurface->m_events.destroy.listen([this] { abortDrag(); });
        m_dnd.dndSurfaceCommit  = dragSurface->m_events.commit.listen([this] {
            if (m_dnd.dndSurface->m_current.texture && !m_dnd.dndSurface->m_mapped) {
                m_dnd.dndSurface->map();
                return;
            }

            if (!m_dnd.dndSurface->m_current.texture && m_dnd.dndSurface->m_mapped) {
                m_dnd.dndSurface->unmap();
                return;
            }
        });
    }

    m_dnd.mouseButton = Event::bus()->m_events.input.mouse.button.listen([this](IPointer::SButtonEvent e, Event::SCallbackInfo&) {
        if (e.state == WL_POINTER_BUTTON_STATE_RELEASED) {
            LOG(Log::DEBUG, "Dropping drag on mouseUp");
            dropDrag();
        }
    });

    m_dnd.touchUp = Event::bus()->m_events.input.touch.up.listen([this](ITouch::SUpEvent e, Event::SCallbackInfo&) {
        LOG(Log::DEBUG, "Dropping drag on touchUp");
        dropDrag();
    });

    m_dnd.tabletTip = Event::bus()->m_events.input.tablet.tip.listen([this](CTablet::STipEvent e, Event::SCallbackInfo&) {
        if (!e.in) {
            LOG(Log::DEBUG, "Dropping drag on tablet tipUp");
            dropDrag();
        }
    });

    m_dnd.mouseMove = Event::bus()->m_events.input.mouse.move.listen([this](Vector2D pos, Event::SCallbackInfo&) {
        if (m_dnd.focusedDevice && !m_dnd.touchPos) {
            if (const auto local = dragSurfaceLocal(g_pSeatManager->m_state.dndPointerFocus.lock(), pos)) {
                m_dnd.focusedDevice->sendMotion(Time::millis(Time::steadyNow()), *local);
                LOG(Log::DEBUG, "Drag motion {}", *local);
            }
        }
    });

    m_dnd.touchMove = Event::bus()->m_events.input.touch.motion.listen([this](ITouch::SMotionEvent e, Event::SCallbackInfo&) {
        if (!m_dnd.touchPos)
            return;
        m_dnd.touchPos = g_pInputManager->m_touchData.lastTouchPos;
        if (m_dnd.focusedDevice) {
            if (const auto local = dragSurfaceLocal(g_pSeatManager->m_state.dndPointerFocus.lock(), *m_dnd.touchPos)) {
                m_dnd.focusedDevice->sendMotion(e.timeMs, *local);
                LOG(Log::DEBUG, "Drag motion {}", *local);
            }
        }
    });

    // unfocus the pointer from the surface, this is part of """standard""" wayland procedure and gtk will freak out if this isn't happening.
    // BTW, the spec does NOT require this explicitly...
    // Fuck you gtk.
    const auto LASTDNDFOCUS = g_pSeatManager->m_state.dndPointerFocus;
    g_pSeatManager->setPointerFocus(nullptr, {});
    g_pSeatManager->m_state.dndPointerFocus = LASTDNDFOCUS;

    // make a new offer, etc
    updateDrag();
}

void CWLDataDeviceProtocol::updateDrag() {
    if (!dndActive())
        return;

    if (m_dnd.focusedDevice)
        m_dnd.focusedDevice->sendLeave();
    m_dnd.focusedDevice.reset();

    const auto surface = g_pSeatManager->m_state.dndPointerFocus.lock();
    if (!surface)
        return;
    const auto position = m_dnd.touchPos.value_or(g_pInputManager->getMouseCoordsInternal());
    const auto local    = dragSurfaceLocal(surface, position);
    if (!local)
        return;
    m_dnd.focusedDevice = dataDeviceForClient(surface->client());
    if (const auto device = m_dnd.focusedDevice.lock())
        offerDragToSurface(device, m_dnd.currentSource.lock(), surface, *local);
}

uint32_t CWLDataDeviceProtocol::enterXDrag(SP<IDataSource> source, SP<CWLSurfaceResource> surface, const Vector2D& local) {
    const auto device = dataDeviceForClient(surface->client());
    if (!device || !device->getWayland())
        return 0;

    const auto offer = offerDragToSurface(device, source, surface, local);
    if (!offer)
        return 0;
    m_xDnd = {device, source, offer};
    return offer->m_resource->version();
}

void CWLDataDeviceProtocol::leaveXDrag() {
    if (m_xDnd.offer)
        m_xDnd.offer->m_dead = true;
    const auto source = m_xDnd.source.lock();
    if (m_xDnd.device && source && !source->dndDone())
        m_xDnd.device->sendLeave();
    m_xDnd = {};
}

void CWLDataDeviceProtocol::updateXDrag(uint32_t time, const Vector2D& local) {
    if (!m_xDnd.offer || !m_xDnd.device)
        return;
    m_xDnd.offer->updateAction();
    m_xDnd.device->sendMotion(time, local);
}

bool CWLDataDeviceProtocol::dropXDrag() {
    if (!m_xDnd.offer || !m_xDnd.device)
        return false;
    m_xDnd.device->sendDrop();
    return true;
}

SP<CWLDataOfferResource> CWLDataDeviceProtocol::offerDragToSurface(SP<IDataDevice> device, SP<IDataSource> source, SP<CWLSurfaceResource> surface, const Vector2D& local) {
    SP<IDataOffer> offer;

    if (const auto WL = device->getWayland(); WL) {
        const auto OFFER = m_offers.emplace_back(makeShared<CWLDataOfferResource>(makeShared<CWlDataOffer>(WL->m_resource->client(), WL->m_resource->version(), 0), source));
        if (!OFFER->good()) {
            WL->m_resource->noMemory();
            m_offers.pop_back();
            return nullptr;
        }
        OFFER->m_source = source;
        OFFER->m_self   = OFFER;
        offer           = OFFER;
    }
#ifndef NO_XWAYLAND
    else if (device->getX11()) {
        const auto xOffer = makeShared<CX11DataOffer>();
        xOffer->m_self    = xOffer;
        xOffer->m_source  = source;
        offer             = xOffer;
    }
#endif

    if (!offer) {
        LOG(Log::ERR, "No offer could be created in updateDrag");
        return nullptr;
    }

    LOG(Log::DEBUG, "New {} dnd offer {:x} for data source {:x}", offer->type() == DATA_SOURCE_TYPE_WAYLAND ? "wayland" : "X11", (uintptr_t)offer.get(), (uintptr_t)source.get());

    device->sendDataOffer(offer);
    if (const auto WL = offer->getWayland(); WL)
        WL->sendData();
    device->sendEnter(wl_display_next_serial(g_pCompositor->m_wlDisplay), surface, local, offer);
#ifndef NO_XWAYLAND
    if (const auto x11 = device->getX11())
        x11->sendMotion(Time::millis(Time::steadyNow()), local);
#endif
    return offer->getWayland();
}

void CWLDataDeviceProtocol::cleanupDndState(bool resetDevice, bool resetSource, bool simulateInput) {
    m_dnd.dndSurface.reset();
    m_dnd.dndSurfaceCommit.reset();
    m_dnd.dndSurfaceDestroy.reset();
    m_dnd.mouseButton.reset();
    m_dnd.mouseMove.reset();
    m_dnd.touchUp.reset();
    m_dnd.touchMove.reset();
    m_dnd.touchPos.reset();
    m_dnd.tabletTip.reset();

    if (resetDevice)
        m_dnd.focusedDevice.reset();
    if (resetSource)
        m_dnd.currentSource.reset();

    if (simulateInput) {
        g_pInputManager->simulateMouseMovement();
        g_pSeatManager->resendEnterEvents();
    }
}

void CWLDataDeviceProtocol::dropDrag() {
    if (!m_dnd.focusedDevice || !m_dnd.currentSource) {
        if (m_dnd.currentSource)
            abortDrag();
        return;
    }

    if (!wasDragSuccessful()) {
        abortDrag();
        return;
    }

    m_dnd.focusedDevice->sendDrop();

#ifndef NO_XWAYLAND
    if (m_dnd.focusedDevice->getX11()) {
        if (m_dnd.overriddenCursor)
            Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_DND);
        m_dnd.overriddenCursor = false;
        cleanupDndState(true, true, true);
        return;
    }
#endif

    m_dnd.focusedDevice->sendLeave();
    if (m_dnd.overriddenCursor)
        Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_DND);
    m_dnd.overriddenCursor = false;
    cleanupDndState(false, false, false);
}

bool CWLDataDeviceProtocol::wasDragSuccessful() {
    if (!m_dnd.currentSource)
        return false;

#ifndef NO_XWAYLAND
    if (const auto x11 = m_dnd.focusedDevice->getX11())
        return x11->canDrop();
#endif

    for (auto const& o : m_offers) {
        if (o->m_dead || o->m_source != m_dnd.currentSource)
            continue;

        if (o->m_recvd || o->m_accepted)
            return true;
    }

    return false;
}

void CWLDataDeviceProtocol::completeDrag() {
    if (!m_dnd.focusedDevice && !m_dnd.currentSource)
        return;

    if (m_dnd.currentSource) {
        m_dnd.currentSource->sendDndDropPerformed();
        m_dnd.currentSource->sendDndFinished();
    }

    cleanupDndState(true, true, true);
}

void CWLDataDeviceProtocol::abortDrag() {
    cleanupDndState(false, false, false);

    if (m_dnd.overriddenCursor)
        Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_DND);
    m_dnd.overriddenCursor = false;

    if (!m_dnd.focusedDevice && !m_dnd.currentSource)
        return;

    if (m_dnd.focusedDevice) {
#ifndef NO_XWAYLAND
        if (auto x11Device = m_dnd.focusedDevice->getX11(); x11Device)
            x11Device->forceCleanupDnd();
#endif
        m_dnd.focusedDevice->sendLeave();
    }

    if (m_dnd.currentSource)
        m_dnd.currentSource->cancelled();

    m_dnd.focusedDevice.reset();
    m_dnd.currentSource.reset();

    g_pInputManager->simulateMouseMovement();
    g_pSeatManager->resendEnterEvents();
}

void CWLDataDeviceProtocol::renderDND(PHLMONITOR pMonitor, const Time::steady_tp& when) {
    if (!m_dnd.dndSurface || !m_dnd.dndSurface->m_current.texture)
        return;

    const auto POS = m_dnd.touchPos.value_or(g_pInputManager->getMouseCoordsInternal());

    Vector2D   surfacePos = POS;

    surfacePos += m_dnd.dndSurface->m_current.offset;

    CBox                         box = CBox{surfacePos, m_dnd.dndSurface->m_current.size}.translate(-pMonitor->m_position).scale(pMonitor->m_scale).round();

    CTexPassElement::SRenderData data;
    data.tex = m_dnd.dndSurface->m_current.texture;
    data.box = box;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(std::move(data)));

    CBox damageBox = CBox{surfacePos, m_dnd.dndSurface->m_current.size}.expand(5);
    g_pHyprRenderer->damageBox(damageBox);

    m_dnd.dndSurface->frame(when);
}

bool CWLDataDeviceProtocol::dndActive() {
    return !!m_dnd.currentSource;
}

void CWLDataDeviceProtocol::abortDndIfPresent() {
#ifndef NO_XWAYLAND
    if (g_pXWayland && g_pXWayland->m_wm)
        g_pXWayland->m_wm->cancelXDrag();
#endif
    if (!dndActive())
        return;
    abortDrag();
}
