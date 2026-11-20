#pragma once

#include "../../macros.hpp"
#include "../../helpers/signal/Signal.hpp"

#include <memory>

struct eis;
struct eis_event;
struct wl_event_source;

class CEISInputManager {
  public:
    CEISInputManager();
    ~CEISInputManager();

    bool active() const;

  private:
    struct SImpl;

    static int             onEvent(int fd, uint32_t mask, void* data);
    static int             onControl(int fd, uint32_t mask, void* data);
    void                   dispatch();
    void                   dispatchControl();

    std::unique_ptr<SImpl> m_impl;
    CHyprSignalListener    m_monitorLayoutChanged;
};

inline UP<CEISInputManager> g_pEISInputManager;
