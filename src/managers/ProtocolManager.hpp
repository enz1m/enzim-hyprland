#pragma once

#include "../defines.hpp"
#include "../helpers/memory/Memory.hpp"
#include "../helpers/signal/Signal.hpp"
#include <unordered_map>
#include <string_view>

class CProtocolManager {
  public:
    CProtocolManager();
    ~CProtocolManager();

    bool isGlobalPrivileged(const wl_global* global);

  private:
    std::unordered_map<std::string, CHyprSignalListener> m_modeChangeListeners;

    void                                                 onMonitorModeChange(PHLMONITOR pMonitor);
};

inline UP<CProtocolManager> g_pProtocolManager;

inline constexpr std::string_view ENZUI_ASSISTANT_INDICATOR = "enzui-assistant-control";
bool                              enzuiAllowsAssistantIndicator(const wl_client* client);
bool                              enzuiRevokeAssistantControl();
bool                              enzuiAllowsVirtualPointerEvents(const wl_client* client);
bool                              enzuiAssistantInputEnabled();
bool                              enzuiAllowsVirtualKeyboardEvents(const wl_client* client);
