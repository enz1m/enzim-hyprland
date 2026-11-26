#pragma once

#include "../defines.hpp"
#include "../helpers/time/Timer.hpp"
#include "../helpers/signal/Signal.hpp"
#include "./eventLoop/EventLoopTimer.hpp"
#include <chrono>
#include <cstdint>
#include <unordered_set>

class CSleepCoverLogind;
class CSessionLockSurface;
class CSessionLock;
class CWLSurfaceResource;

namespace Desktop::View {
    class CSessionLock;
}

struct SSessionLockSurface {
    SSessionLockSurface(SP<CSessionLockSurface> surface_);

    WP<CSessionLockSurface>         surface;
    WP<CWLSurfaceResource>          pWlrSurface;
    SP<Desktop::View::CSessionLock> view;
    uint64_t                        iMonitorID = -1;

    bool                            mapped = false;

    struct {
        CHyprSignalListener map;
        CHyprSignalListener destroy;
        CHyprSignalListener commit;
    } listeners;
};

struct SSessionLock {
    WP<CSessionLock>                     lock;
    CTimer                               lockTimer;
    SP<CEventLoopTimer>                  sendLockedTimer;

    std::vector<SP<SSessionLockSurface>> vSessionLockSurfaces;

    struct {
        CHyprSignalListener newSurface;
        CHyprSignalListener unlock;
        CHyprSignalListener destroy;
    } listeners;

    bool                         hasSentLocked = false;
    bool                         hasSentDenied = false;
    std::unordered_set<uint64_t> lockedMonitors;
};

class CSessionLockManager {
  public:
    CSessionLockManager();
    ~CSessionLockManager();

    WP<SSessionLockSurface> getSessionLockSurfaceForMonitor(uint64_t);

    bool                    isSessionLocked();
    bool                    clientLocked();
    bool                    clientDenied();
    bool                    isSurfaceSessionLock(SP<CWLSurfaceResource>);
    bool                    anySessionLockSurfacesPresent();

    bool                    managedLock() const;
    bool                    preparingForSleep() const;
    bool                    blocksUnlock() const;
    bool                    logindHealthy() const;
    void                    setLogindHealthy(bool);
    void                    setSleepInhibitorLimit(uint64_t microseconds);
    bool                    forcesOpaqueLock();
    bool                    allowsInput(const SP<CWLSurfaceResource>&);
    bool                    sleepCoverReady() const;
    bool                    needsLocker() const;
    bool                    privateOutputFailed() const;
    void                    schedulePrivateOutputRecovery();
    void                    reserveSleepCover();
    uint64_t                sleepCoverGeneration() const;
    void                    armSleepCover();
    void                    setPreparingForSleep(bool);
    void                    maybeAcknowledgePrivateLock();

    void                    forceUnlock();
    void                    forceLock();

    void                    removeSessionLockSurface(SSessionLockSurface*);

    void                    onLockscreenRenderedOnMonitor(uint64_t id);

    bool                    shallConsiderLockMissing();

    struct {
        CSignalT<> lock;
        CSignalT<> unlock;
    } m_events;

  private:
    UP<SSessionLock>                      m_sessionLock;
    UP<CSleepCoverLogind>                 m_sleepLogind;
    const bool                            m_managedLock;
    const bool                            m_logindRequired;
    bool                                  m_startupProtection;
    bool                                  m_logindHealthy     = false;
    uint64_t                              m_coverGeneration   = 0;
    bool                                  m_sleepCover        = false;
    bool                                  m_preparingForSleep = false;
    bool                                  privateOutputsReady() const;
    SP<CEventLoopTimer>                   m_privateOutputRecovery;
    std::chrono::microseconds             m_privateRecoveryDelay = std::chrono::milliseconds(350);
    bool                                  m_privateOutputFailed  = false;
    std::chrono::steady_clock::time_point m_sleepRequestExpires;

    void                                  secureSeat();
    void                                  updateLockOutputs();

    struct {
        CHyprSignalListener newLock;
    } m_listeners;

    void onNewSessionLock(SP<CSessionLock> pWlrLock);
    void removeSendLockedTimer();
    void clearSessionLock();
};

inline UP<CSessionLockManager> g_pSessionLockManager;
