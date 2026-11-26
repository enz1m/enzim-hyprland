#include "ProtocolManager.hpp"
#include "SessionLockManager.hpp"
#include "../Compositor.hpp"
#include "../config/ConfigValue.hpp"
#include "../protocols/FractionalScale.hpp"
#include "../protocols/InputCapture.hpp"
#include "../protocols/ImageCopyCapture.hpp"
#include "../protocols/core/DataDevice.hpp"
#include "../protocols/SessionLock.hpp"
#include "../render/Renderer.hpp"
#include "screenshare/ScreenshareManager.hpp"
#include "../desktop/state/FocusState.hpp"
#include "../desktop/view/SessionLock.hpp"
#include "../managers/SeatManager.hpp"
#include "../managers/input/InputManager.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"
#include "../state/MonitorState.hpp"
#include <algorithm>
#include <chrono>
#include <ranges>
#include <utility>

#ifdef USES_SYSTEMD
#include <systemd/sd-bus.h>
#include <poll.h>
#include <fcntl.h>
#include <wayland-server-core.h>
#endif

class CSleepCoverLogind {
  public:
    explicit CSleepCoverLogind(CSessionLockManager& locks) : m_locks(locks) {
#ifdef USES_SYSTEMD
        m_timer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, onTimer, this);
        if (m_timer)
            wl_event_source_timer_update(m_timer, 1);
#endif
    }

    void releaseIfPrivate() {
#ifdef USES_SYSTEMD
        if ((m_delayStale || m_locks.preparingForSleep()) && m_locks.sleepCoverReady())
            m_delay.reset();
#endif
    }

    ~CSleepCoverLogind() {
#ifdef USES_SYSTEMD
        disconnect();
        if (m_timer)
            wl_event_source_remove(m_timer);
#endif
    }

  private:
    CSessionLockManager& m_locks;

#ifdef USES_SYSTEMD
    static constexpr auto                 LOGIN   = "org.freedesktop.login1";
    static constexpr auto                 PATH    = "/org/freedesktop/login1";
    static constexpr auto                 MANAGER = "org.freedesktop.login1.Manager";

    sd_bus*                               m_bus        = nullptr;
    sd_bus_slot*                          m_inhibit    = nullptr;
    sd_bus_slot*                          m_reconcile  = nullptr;
    sd_bus_slot*                          m_delayLimit = nullptr;
    wl_event_source*                      m_source     = nullptr;
    wl_event_source*                      m_timer      = nullptr;
    Hyprutils::OS::CFileDescriptor        m_delay;
    bool                                  m_delayStale    = false;
    bool                                  m_reconciled    = false;
    bool                                  m_logindChanged = false;
    std::chrono::steady_clock::time_point m_retryAt;

    void                                  retryLater() {
        m_retryAt = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        m_locks.setLogindHealthy(false);
    }

    void disconnect() {
        m_reconcile  = sd_bus_slot_unref(m_reconcile);
        m_delayLimit = sd_bus_slot_unref(m_delayLimit);
        m_inhibit    = sd_bus_slot_unref(m_inhibit);
        if (m_source)
            wl_event_source_remove(std::exchange(m_source, nullptr));
        m_bus        = sd_bus_unref(m_bus);
        m_reconciled = false;
    }

    bool connect() {
        if (sd_bus_open_system(&m_bus) < 0)
            return false;
        sd_bus_set_method_call_timeout(m_bus, 3'000'000);
        if (sd_bus_match_signal_async(m_bus, nullptr, LOGIN, PATH, MANAGER, "PrepareForSleep", onSleep, onSubscribed, this) < 0 ||
            sd_bus_add_match_async(m_bus, nullptr,
                                   "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
                                   "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.freedesktop.login1'",
                                   onLogindChanged, onSubscribed, this) < 0)
            return false;
        sd_bus_call_method_async(m_bus, &m_delayLimit, LOGIN, PATH, "org.freedesktop.DBus.Properties", "Get", onDelayLimit, this, "ss", MANAGER, "InhibitDelayMaxUSec");
        m_source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, sd_bus_get_fd(m_bus), WL_EVENT_READABLE, onBus, this);
        return m_source;
    }

    void service() {
        releaseIfPrivate();

        int result = 0;
        if (!m_bus && !connect())
            result = -1;
        else {
            do {
                result = sd_bus_process(m_bus, nullptr);
            } while (result > 0 && !m_logindChanged);
        }

        if (result < 0 || m_logindChanged) {
            const bool reconnect = std::exchange(m_logindChanged, false);
            m_delayStale         = true;
            m_locks.setLogindHealthy(false);
            disconnect();
            wl_event_source_timer_update(m_timer, reconnect ? 1 : 1000);
            return;
        }

        releaseIfPrivate();
        int interval = 0;
        if (!m_reconciled) {
            if (!m_reconcile && std::chrono::steady_clock::now() >= m_retryAt &&
                sd_bus_call_method_async(m_bus, &m_reconcile, LOGIN, PATH, "org.freedesktop.DBus.Properties", "Get", onPreparingProperty, this, "ss", MANAGER,
                                         "PreparingForSleep") < 0)
                retryLater();
            interval = 1000;
        } else if (!m_locks.preparingForSleep() && !m_delay.isValid()) {
            if (!m_inhibit && std::chrono::steady_clock::now() >= m_retryAt &&
                sd_bus_call_method_async(m_bus, &m_inhibit, LOGIN, PATH, MANAGER, "Inhibit", onInhibited, this, "ssss", "sleep", "enzimOS compositor",
                                         "Present a private lock screen before sleep", "delay") < 0)
                retryLater();
            interval = 1000;
        }

        if (m_delay.isValid() && (m_delayStale || m_locks.preparingForSleep()))
            interval = 50;

        const int events = sd_bus_get_events(m_bus);
        wl_event_source_fd_update(m_source, (events & POLLIN ? WL_EVENT_READABLE : 0) | (events & POLLOUT ? WL_EVENT_WRITABLE : 0));
        wl_event_source_timer_update(m_timer, interval);
    }

    void reconciled(bool preparing) {
        m_reconciled = true;
        m_retryAt    = {};
        m_locks.setPreparingForSleep(preparing);
        if (!preparing && m_delay.isValid() && !m_delayStale)
            m_locks.setLogindHealthy(true);
    }

    static int onTimer(void* data) {
        static_cast<CSleepCoverLogind*>(data)->service();
        return 0;
    }

    static int onBus(int, uint32_t, void* data) {
        return onTimer(data);
    }

    static int onSubscribed(sd_bus_message* reply, void* data, sd_bus_error*) {
        if (sd_bus_message_is_method_error(reply, nullptr) > 0)
            static_cast<CSleepCoverLogind*>(data)->m_logindChanged = true;
        return 0;
    }

    static int onDelayLimit(sd_bus_message* reply, void* data, sd_bus_error*) {
        auto& self        = *static_cast<CSleepCoverLogind*>(data);
        self.m_delayLimit = sd_bus_slot_unref(self.m_delayLimit);
        uint64_t limit    = 0;
        if (sd_bus_message_enter_container(reply, 'v', "t") > 0 && sd_bus_message_read(reply, "t", &limit) > 0)
            self.m_locks.setSleepInhibitorLimit(limit);
        return 0;
    }

    static int onPreparingProperty(sd_bus_message* reply, void* data, sd_bus_error*) {
        auto& self       = *static_cast<CSleepCoverLogind*>(data);
        self.m_reconcile = sd_bus_slot_unref(self.m_reconcile);
        int preparing    = 0;
        if (sd_bus_message_enter_container(reply, 'v', "b") > 0 && sd_bus_message_read(reply, "b", &preparing) > 0)
            self.reconciled(preparing);
        else
            self.retryLater();
        return 0;
    }

    static int onSleep(sd_bus_message* message, void* data, sd_bus_error*) {
        auto& self      = *static_cast<CSleepCoverLogind*>(data);
        int   preparing = 0;
        if (sd_bus_message_read(message, "b", &preparing) > 0) {
            self.m_reconcile = sd_bus_slot_unref(self.m_reconcile);
            self.reconciled(preparing);
        }
        return 0;
    }

    static int onLogindChanged(sd_bus_message*, void* data, sd_bus_error*) {
        static_cast<CSleepCoverLogind*>(data)->m_logindChanged = true;
        return 0;
    }

    static int onInhibited(sd_bus_message* reply, void* data, sd_bus_error*) {
        auto& self     = *static_cast<CSleepCoverLogind*>(data);
        self.m_inhibit = sd_bus_slot_unref(self.m_inhibit);
        int fd         = -1;
        if (sd_bus_message_read(reply, "h", &fd) > 0) {
            self.m_delay      = Hyprutils::OS::CFileDescriptor(fcntl(fd, F_DUPFD_CLOEXEC, 3));
            self.m_delayStale = false;
        }
        if (!self.m_delay.isValid())
            self.retryLater();
        else if (!self.m_locks.preparingForSleep())
            self.m_locks.setLogindHealthy(true);
        return 0;
    }
#endif
};

SSessionLockSurface::SSessionLockSurface(SP<CSessionLockSurface> surface_) : surface(surface_) {
    pWlrSurface = surface->surface();

    listeners.map = surface_->m_events.map.listen([this] {
        mapped = true;

        g_pInputManager->simulateMouseMovement();

        const auto PMONITOR = State::monitorState()->query().id(iMonitorID).run();

        if (PMONITOR)
            g_pHyprRenderer->damageMonitor(PMONITOR);
    });

    listeners.destroy = surface_->m_events.destroy.listen([this] {
        if (pWlrSurface == Desktop::focusState()->surface())
            Desktop::focusState()->surface().reset();

        g_pSessionLockManager->removeSessionLockSurface(this);
    });

    listeners.commit = surface_->m_events.commit.listen([this] {
        const auto PMONITOR = State::monitorState()->query().id(iMonitorID).run();

        if (mapped && !Desktop::focusState()->surface())
            g_pInputManager->simulateMouseMovement();

        if (PMONITOR)
            g_pHyprRenderer->damageMonitor(PMONITOR);
    });
}

static bool enabledEnv(const char* name) {
    const auto value = getenv(name);
    return value && std::string_view(value) == "1";
}

CSessionLockManager::CSessionLockManager() :
    m_managedLock(enabledEnv("ENZUI_MANAGED_LOCK")), m_logindRequired(m_managedLock && enabledEnv("ENZUI_SLEEP_COVER_LOGIND")), m_startupProtection(m_logindRequired) {
    if (m_logindRequired)
        m_sleepLogind = makeUnique<CSleepCoverLogind>(*this);
    m_listeners.newLock = PROTO::sessionLock->m_events.newLock.listen([this](const auto& lock) { this->onNewSessionLock(lock); });
}

CSessionLockManager::~CSessionLockManager() = default;

void enzuiRevokePrivileged();

bool CSessionLockManager::preparingForSleep() const {
    return m_preparingForSleep;
}

bool CSessionLockManager::blocksUnlock() const {
    // A logind outage keeps the cover armed but must not lock the user out: authentication unlocks unless a sleep is in progress.
    return m_preparingForSleep || std::chrono::steady_clock::now() < m_sleepRequestExpires;
}

bool CSessionLockManager::logindHealthy() const {
    return m_logindHealthy && !m_preparingForSleep;
}

void CSessionLockManager::setLogindHealthy(bool healthy) {
    m_logindHealthy = healthy;
    if (!healthy)
        armSleepCover();

    if (!std::exchange(m_startupProtection, false))
        return;

    updateLockOutputs();
    if (!isSessionLocked())
        g_pInputManager->refocus();
}

bool CSessionLockManager::managedLock() const {
    return m_managedLock;
}

bool CSessionLockManager::forcesOpaqueLock() {
    return m_startupProtection || m_sleepCover || (m_managedLock && isSessionLocked());
}

bool CSessionLockManager::allowsInput(const SP<CWLSurfaceResource>& surface) {
    return !forcesOpaqueLock() || (surface && isSurfaceSessionLock(surface));
}

uint64_t CSessionLockManager::sleepCoverGeneration() const {
    return m_sleepCover ? m_coverGeneration : 0;
}

bool CSessionLockManager::privateOutputsReady() const {
    return std::ranges::all_of(State::monitorState()->allMonitors(), [](const auto& monitor) { return monitor->privateOrOff(); });
}

bool CSessionLockManager::sleepCoverReady() const {
    return m_sleepCover && privateOutputsReady();
}

bool CSessionLockManager::needsLocker() const {
    return m_managedLock && m_sleepCover && !m_sessionLock && !m_preparingForSleep && std::chrono::steady_clock::now() >= m_sleepRequestExpires;
}

bool CSessionLockManager::privateOutputFailed() const {
    return m_privateOutputFailed && !privateOutputsReady();
}

void CSessionLockManager::reserveSleepCover() {
    m_sleepRequestExpires = std::chrono::steady_clock::now() + std::chrono::seconds(5);
}

void CSessionLockManager::setSleepInhibitorLimit(uint64_t microseconds) {
    // Leave time for backend completion before logind's delay inhibitor expires.
    m_privateRecoveryDelay = std::chrono::microseconds(std::clamp<uint64_t>(microseconds / 2, 1'000, 350'000));
    if (m_preparingForSleep && m_privateOutputRecovery)
        m_privateOutputRecovery->updateTimeout(m_privateRecoveryDelay);
}

void CSessionLockManager::schedulePrivateOutputRecovery() {
    if (!forcesOpaqueLock() || m_privateOutputRecovery)
        return;

    m_privateOutputRecovery = makeShared<CEventLoopTimer>(
        m_privateRecoveryDelay,
        [this](auto, auto) {
            m_privateOutputRecovery.reset();
            if (!forcesOpaqueLock() || privateOutputsReady())
                return;

            for (const auto& monitor : State::monitorState()->allMonitors()) {
                if (!monitor->privateOrOff())
                    monitor->recoverPrivateScanout();
            }

            if (privateOutputsReady()) {
                maybeAcknowledgePrivateLock();
                return;
            }

            if (!std::exchange(m_privateOutputFailed, true))
                LOG(Log::ERR, "Session lock cannot yet confirm private presentation or output disable");
            schedulePrivateOutputRecovery();
        },
        nullptr);
    g_pEventLoopManager->addTimer(m_privateOutputRecovery);
}

void CSessionLockManager::maybeAcknowledgePrivateLock() {
    if (!forcesOpaqueLock() || !privateOutputsReady())
        return;

    if (m_sleepLogind)
        m_sleepLogind->releaseIfPrivate();
    if (m_sessionLock && !clientLocked() && !clientDenied() && m_sessionLock->lock) {
        m_sessionLock->lock->sendLocked();
        m_sessionLock->hasSentLocked = true;
    }
}

void CSessionLockManager::updateLockOutputs() {
    for (const auto& monitor : State::monitorState()->allMonitors()) {
        monitor->setPrivateScanout(forcesOpaqueLock());
        if (monitor->m_enabled)
            monitor->addDamage(CBox{{}, monitor->m_transformedSize});
    }
}

void CSessionLockManager::secureSeat() {
    if (forcesOpaqueLock()) {
        PROTO::imageCopyCapture->suspendCursorCapture();
        if (Screenshare::mgr())
            Screenshare::mgr()->suspendCapture();
    }
    enzuiRevokeAssistantControl();
    enzuiRevokePrivileged();
    PROTO::inputCapture->forceRelease();
    PROTO::data->abortDndIfPresent();
    g_pSeatManager->clearRetainedClipboard();
    if (forcesOpaqueLock())
        g_pSeatManager->clearGrabWithoutRefocus();
    else
        g_pSeatManager->setGrab(nullptr);
    Desktop::focusState()->rawSurfaceFocus(nullptr);
    g_pSeatManager->setKeyboardFocus(nullptr);
    g_pSeatManager->setPointerFocus(nullptr, {});
    g_pSeatManager->sendTouchCancel();
    if (forcesOpaqueLock()) {
        g_pInputManager->cancelPrivateInputContacts();
        g_pInputManager->resetClientCursor();
    }
}

void CSessionLockManager::armSleepCover() {
    if (m_sleepCover)
        return;

    m_sleepCover          = true;
    m_privateOutputFailed = false;
    ++m_coverGeneration;
    if (!isSessionLocked()) {
        PROTO::sessionLock->forceLock();
        updateLockOutputs();
        secureSeat();
        m_events.lock.emit();
    } else {
        updateLockOutputs();
        secureSeat();
    }
}

void CSessionLockManager::setPreparingForSleep(bool preparing) {
    const bool wasPreparing = std::exchange(m_preparingForSleep, preparing);
    if (preparing) {
        m_sleepRequestExpires = {};
        if (m_privateOutputRecovery)
            m_privateOutputRecovery->updateTimeout(m_privateRecoveryDelay);
        m_logindHealthy = false;
        armSleepCover();
    } else if (wasPreparing) {
        for (const auto& monitor : State::monitorState()->monitors()) {
            monitor->setDPMS(true);
            monitor->scheduleFrame();
        }
    }
}

void CSessionLockManager::onNewSessionLock(SP<CSessionLock> pLock) {
    static auto PALLOWRELOCK = CConfigValue<Config::INTEGER>("misc:allow_session_lock_restore");

    if (m_sessionLock || (isSessionLocked() && !m_managedLock && !m_sleepCover && !*PALLOWRELOCK && g_pCompositor->m_startLockedCommand.empty())) {
        LOG(Log::DEBUG, "Refusing session-lock request while the session is locked");
        pLock->sendDenied();
        return;
    }

    LOG(Log::DEBUG, "Session got locked by {:x}", (uintptr_t)pLock.get());
    PROTO::sessionLock->beginLock();
    updateLockOutputs();
    secureSeat();

    m_sessionLock       = makeUnique<SSessionLock>();
    m_sessionLock->lock = pLock;
    m_sessionLock->lockTimer.reset();

    m_sessionLock->listeners.newSurface = pLock->m_events.newLockSurface.listen([this](const SP<CSessionLockSurface>& surface) {
        const auto PMONITOR = surface->monitor();

        const auto NEWSURFACE  = m_sessionLock->vSessionLockSurfaces.emplace_back(makeShared<SSessionLockSurface>(surface));
        NEWSURFACE->iMonitorID = PMONITOR->m_id;
        PROTO::fractional->sendScale(surface->surface(), PMONITOR->m_scale);

        NEWSURFACE->view = Desktop::View::CSessionLock::create(surface);
    });

    m_sessionLock->listeners.unlock = pLock->m_events.unlockAndDestroy.listen([this] {
        removeSendLockedTimer();
        m_sessionLock.reset();
        if (blocksUnlock()) {
            Desktop::focusState()->rawSurfaceFocus(nullptr);
        } else {
            m_sleepCover = false;
            m_events.unlock.emit();
            g_pInputManager->refocus();
        }
        updateLockOutputs();
    });

    m_sessionLock->listeners.destroy = pLock->m_events.destroyed.listen([this] {
        removeSendLockedTimer();
        m_sessionLock.reset();
        Desktop::focusState()->rawSurfaceFocus(nullptr);
        if (forcesOpaqueLock()) {
            g_pSeatManager->setPointerFocus(nullptr, {});
            g_pInputManager->resetClientCursor();
        }
        if (m_managedLock && !m_sleepCover)
            armSleepCover();
        else
            updateLockOutputs();
    });

    m_events.lock.emit();

    if (forcesOpaqueLock()) {
        maybeAcknowledgePrivateLock();
        return;
    }

    const bool NOACTIVEMONS = std::ranges::all_of(State::monitorState()->monitors(), [](const auto& m) { return !m->m_enabled || !m->m_dpmsStatus; });

    if (NOACTIVEMONS) {
        // Normally the locked event is sent after each output rendered a lock screen frame.
        // When there are no active outputs, send it right away.
        m_sessionLock->lock->sendLocked();
        m_sessionLock->hasSentLocked = true;
        return;
    }

    m_sessionLock->sendLockedTimer = makeShared<CEventLoopTimer>(
        // Clients get sent the "locked" event after they submitted a lock frame for each output.
        // If they fail to do this, we send the "locked" event after a fixed amount of time here.
        // Previously we sent denied after this timeout, but that forcefully makes the client exit and the protocol doesn't require that anyways.
        std::chrono::seconds(5),
        [](auto, auto) {
            if (!g_pSessionLockManager || g_pSessionLockManager->clientLocked() || g_pSessionLockManager->clientDenied())
                return;

            if (!g_pSessionLockManager->m_sessionLock || !g_pSessionLockManager->m_sessionLock->lock)
                return;

            LOG(Log::WARN,
                "Sending locked after a 5 second timeout. This happens when we failed to render a lock frame from the client for every output. Lockdead frames may be shown.");
            g_pSessionLockManager->m_sessionLock->lock->sendLocked();
            g_pSessionLockManager->m_sessionLock->hasSentLocked = true;
        },
        nullptr);

    g_pEventLoopManager->addTimer(m_sessionLock->sendLockedTimer);
}

void CSessionLockManager::removeSendLockedTimer() {
    if (!m_sessionLock || !m_sessionLock->sendLockedTimer)
        return;

    g_pEventLoopManager->removeTimer(m_sessionLock->sendLockedTimer);
    m_sessionLock->sendLockedTimer.reset();
}

bool CSessionLockManager::isSessionLocked() {
    return PROTO::sessionLock->isLocked();
}

WP<SSessionLockSurface> CSessionLockManager::getSessionLockSurfaceForMonitor(uint64_t id) {
    if (!m_sessionLock)
        return {};

    for (const auto& sls : m_sessionLock->vSessionLockSurfaces) {
        const auto MONITOR = sls->surface->monitor();
        if (sls->iMonitorID == id && sls->mapped && sls->surface->surface() && MONITOR && MONITOR->m_enabled)
            return sls;
    }

    return {};
}

void CSessionLockManager::onLockscreenRenderedOnMonitor(uint64_t id) {
    if (forcesOpaqueLock() || !m_sessionLock || m_sessionLock->hasSentLocked || m_sessionLock->hasSentDenied)
        return;

    m_sessionLock->lockedMonitors.emplace(id);
    const bool LOCKED =
        std::ranges::all_of(State::monitorState()->monitors(), [this](auto m) { return !m->m_enabled || !m->m_dpmsStatus || m_sessionLock->lockedMonitors.contains(m->m_id); });

    if (LOCKED && m_sessionLock->lock->good()) {
        removeSendLockedTimer();
        m_sessionLock->lock->sendLocked();
        m_sessionLock->hasSentLocked = true;
    }
}

bool CSessionLockManager::isSurfaceSessionLock(SP<CWLSurfaceResource> pSurface) {
    // TODO: this has some edge cases when it's wrong (e.g. destroyed lock but not yet surfaces)
    // but can be easily fixed when I rewrite wlr_surface

    if (!m_sessionLock)
        return false;

    for (auto const& sls : m_sessionLock->vSessionLockSurfaces) {
        if (sls->mapped && sls->pWlrSurface == pSurface)
            return true;
    }

    return false;
}

bool CSessionLockManager::anySessionLockSurfacesPresent() {
    return m_sessionLock && std::ranges::any_of(m_sessionLock->vSessionLockSurfaces, [](const auto& surf) { return surf->mapped; });
}

void CSessionLockManager::removeSessionLockSurface(SSessionLockSurface* pSLS) {
    if (!m_sessionLock)
        return;

    const auto MONITOR = pSLS->surface->monitor();
    std::erase_if(m_sessionLock->vSessionLockSurfaces, [&](const auto& other) { return pSLS == other.get(); });
    if (MONITOR && MONITOR->m_enabled)
        g_pHyprRenderer->damageMonitor(MONITOR);

    if (Desktop::focusState()->surface())
        return;

    for (auto const& sls : m_sessionLock->vSessionLockSurfaces) {
        if (!sls->mapped)
            continue;

        Desktop::focusState()->rawSurfaceFocus(sls->surface->surface());
        break;
    }
}

bool CSessionLockManager::clientLocked() {
    return m_sessionLock && m_sessionLock->hasSentLocked;
}

bool CSessionLockManager::clientDenied() {
    return m_sessionLock && m_sessionLock->hasSentDenied;
}

void CSessionLockManager::clearSessionLock() {
    m_events.unlock.emit();
    m_sessionLock = {};
}

void CSessionLockManager::forceUnlock() {
    if (m_managedLock || m_sleepCover)
        return;

    PROTO::sessionLock->forceUnlock();
    clearSessionLock();

    Desktop::focusState()->rawSurfaceFocus(nullptr);
    for (auto const& m : State::monitorState()->monitors())
        g_pHyprRenderer->damageMonitor(m);

    g_pInputManager->refocus();
}

void CSessionLockManager::forceLock() {
    PROTO::sessionLock->forceLock();
    if (m_managedLock) {
        secureSeat();
        updateLockOutputs();
    } else
        g_pSeatManager->clearRetainedClipboard();
    m_events.lock.emit();
}

bool CSessionLockManager::shallConsiderLockMissing() {
    if (!m_sessionLock)
        return true;

    static auto LOCKDEAD_SCREEN_DELAY = CConfigValue<Config::INTEGER>("misc:lockdead_screen_delay");

    return m_sessionLock->lockTimer.getMillis() > *LOCKDEAD_SCREEN_DELAY;
}
