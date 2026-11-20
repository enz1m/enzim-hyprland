#include "EISInputManager.hpp"

#include "InputManager.hpp"
#include "../../Compositor.hpp"
#include "../../devices/IKeyboard.hpp"
#include "../../devices/IPointer.hpp"
#include "../../devices/ITouch.hpp"
#include "../../event/EventBus.hpp"
#include "../../state/MonitorState.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <libeis.h>
#include <linux/input-event-codes.h>
#include <sys/socket.h>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <wayland-server-core.h>

namespace {
    class CEISPointer final : public IPointer {
      public:
        static SP<CEISPointer> create() {
            auto pointer          = SP<CEISPointer>(new CEISPointer());
            pointer->m_self       = pointer;
            pointer->m_deviceName = "enzremote-eis-pointer";
            return pointer;
        }

        bool isVirtual() const override {
            return true;
        }

        SP<Aquamarine::IPointer> aq() override {
            return nullptr;
        }

        void motion(uint32_t timeMs, const Vector2D& delta) {
            m_pointerEvents.motion.emit(SMotionEvent{.timeMs = timeMs, .delta = delta, .unaccel = delta, .device = m_self.lock()});
        }

        void motionAbsolute(uint32_t timeMs, const Vector2D& position) {
            m_pointerEvents.motionAbsolute.emit(SMotionAbsoluteEvent{.timeMs = timeMs, .absolute = position, .device = m_self.lock()});
        }

        void button(uint32_t timeMs, uint32_t button, bool pressed) {
            m_pointerEvents.button.emit(SButtonEvent{
                .timeMs = timeMs,
                .button = button,
                .state  = pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED,
            });
        }

        void axis(uint32_t timeMs, wl_pointer_axis axis, double delta, int32_t discrete = 0) {
            m_pointerEvents.axis.emit(SAxisEvent{
                .timeMs        = timeMs,
                .source        = discrete == 0 ? WL_POINTER_AXIS_SOURCE_CONTINUOUS : WL_POINTER_AXIS_SOURCE_WHEEL,
                .axis          = axis,
                .delta         = delta,
                .deltaDiscrete = discrete,
            });
        }

        void frame() {
            m_pointerEvents.frame.emit();
        }

        void destroy() {
            m_events.destroy.emit();
        }

      private:
        CEISPointer() = default;
    };

    class CEISKeyboard final : public IKeyboard {
      public:
        static SP<CEISKeyboard> create() {
            auto keyboard             = SP<CEISKeyboard>(new CEISKeyboard());
            keyboard->m_self          = keyboard;
            keyboard->m_deviceName    = "enzremote-eis-keyboard";
            keyboard->m_allowBinds    = false;
            keyboard->m_forcedNoBinds = true;
            keyboard->m_shareStates   = false;
            return keyboard;
        }

        bool isVirtual() const override {
            return true;
        }

        SP<Aquamarine::IKeyboard> aq() override {
            return nullptr;
        }

        void key(uint32_t timeMs, uint32_t keycode, bool pressed) {
            const auto changed = updatePressed(keycode, pressed);
            m_keyboardEvents.key.emit(SKeyEvent{
                .timeMs  = timeMs,
                .keycode = keycode,
                .state   = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED,
            });
            if (changed) {
                updateXkbStateWithKey(keycode + 8, pressed);
                updateModifiersState();
                m_keyboardEvents.modifiers.emit(SModifiersEvent{
                    .depressed = m_modifiersState.depressed,
                    .latched   = m_modifiersState.latched,
                    .locked    = m_modifiersState.locked,
                    .group     = m_modifiersState.group,
                });
            }
        }

        void destroy() {
            m_events.destroy.emit();
        }

      private:
        CEISKeyboard() = default;
    };

    class CEISTouch final : public ITouch {
      public:
        static SP<CEISTouch> create() {
            auto touch          = SP<CEISTouch>(new CEISTouch());
            touch->m_self       = touch;
            touch->m_deviceName = "enzremote-eis-touch";
            return touch;
        }

        bool isVirtual() const override {
            return true;
        }

        SP<Aquamarine::ITouch> aq() override {
            return nullptr;
        }

        void down(uint32_t timeMs, int32_t id, const Vector2D& position) {
            m_touchEvents.down.emit(SDownEvent{.timeMs = timeMs, .touchID = id, .pos = position, .device = m_self.lock()});
        }

        void up(uint32_t timeMs, int32_t id) {
            m_touchEvents.up.emit(SUpEvent{.timeMs = timeMs, .touchID = id});
        }

        void motion(uint32_t timeMs, int32_t id, const Vector2D& position) {
            m_touchEvents.motion.emit(SMotionEvent{.timeMs = timeMs, .touchID = id, .pos = position});
        }

        void cancel(uint32_t timeMs, int32_t id) {
            m_touchEvents.cancel.emit(SCancelEvent{.timeMs = timeMs, .touchID = id});
        }

        void frame() {
            m_touchEvents.frame.emit();
        }

        void destroy() {
            m_events.destroy.emit();
        }

      private:
        CEISTouch() = default;
    };

    uint32_t eventTimeMs(eis_event* event) {
        return static_cast<uint32_t>(eis_event_get_time(event) / 1000);
    }

    Vector2D normalizedPosition(eis_event* event, bool touch) {
        const auto device = eis_event_get_device(event);
        const auto region = eis_device_get_region(device, 0);
        if (!region)
            return {};

        const auto x      = touch ? eis_event_touch_get_x(event) : eis_event_pointer_get_absolute_x(event);
        const auto y      = touch ? eis_event_touch_get_y(event) : eis_event_pointer_get_absolute_y(event);
        const auto width  = std::max(1U, eis_region_get_width(region));
        const auto height = std::max(1U, eis_region_get_height(region));
        return {std::clamp((x - eis_region_get_x(region)) / width, 0.0, 1.0), std::clamp((y - eis_region_get_y(region)) / height, 0.0, 1.0)};
    }
} // namespace

struct CEISInputManager::SImpl {
    struct SClient {
        eis_client*                  client       = nullptr;
        eis_seat*                    seat         = nullptr;
        eis_device*                  device       = nullptr;
        uint32_t                     capabilities = 0;
        uint32_t                     regionWidth  = 0;
        uint32_t                     regionHeight = 0;

        SP<CEISPointer>              pointer;
        SP<CEISKeyboard>             keyboard;
        SP<CEISTouch>                touch;

        std::unordered_set<uint32_t> pressedButtons;
        std::unordered_set<uint32_t> pressedKeys;
        std::unordered_set<int32_t>  activeTouches;

        ~SClient() {
            if (device) {
                eis_device_remove(device);
                eis_device_unref(device);
            }
            if (seat) {
                eis_seat_remove(seat);
                eis_seat_unref(seat);
            }
            if (pointer)
                pointer->destroy();
            if (keyboard)
                keyboard->destroy();
            if (touch)
                touch->destroy();
        }

        void neutralize(uint32_t timeMs) {
            if (pointer) {
                for (const auto button : pressedButtons)
                    pointer->button(timeMs, button, false);
                pointer->frame();
            }
            if (keyboard) {
                for (const auto key : pressedKeys)
                    keyboard->key(timeMs, key, false);
            }
            if (touch) {
                for (const auto id : activeTouches)
                    touch->cancel(timeMs, id);
                touch->frame();
            }
            pressedButtons.clear();
            pressedKeys.clear();
            activeTouches.clear();
        }
    };

    eis*                                                      context       = nullptr;
    wl_event_source*                                          eventSource   = nullptr;
    wl_event_source*                                          controlSource = nullptr;
    int                                                       controlFd     = -1;
    std::unordered_map<eis_client*, std::unique_ptr<SClient>> clients;
    std::string                                               mappingId;

    std::pair<uint32_t, uint32_t>                             viewport() const {
        const auto& monitors = State::monitorState()->monitors();
        const auto  monitor  = monitors.empty() ? nullptr : monitors.front();
        return {
            static_cast<uint32_t>(monitor ? std::max(1.0, monitor->m_size.x) : 1.0),
            static_cast<uint32_t>(monitor ? std::max(1.0, monitor->m_size.y) : 1.0),
        };
    }

    void configureDevice(SClient* client) {
        if (client->device) {
            client->neutralize(static_cast<uint32_t>(eis_now(context) / 1000));
            eis_device_remove(client->device);
            eis_device_unref(client->device);
        }

        if (!(client->capabilities & (EIS_DEVICE_CAP_POINTER | EIS_DEVICE_CAP_POINTER_ABSOLUTE)) && client->pointer) {
            client->pointer->destroy();
            client->pointer.reset();
        }
        if (!(client->capabilities & EIS_DEVICE_CAP_KEYBOARD) && client->keyboard) {
            client->keyboard->destroy();
            client->keyboard.reset();
        }
        if (!(client->capabilities & EIS_DEVICE_CAP_TOUCH) && client->touch) {
            client->touch->destroy();
            client->touch.reset();
        }
        if (client->capabilities == 0) {
            client->device       = nullptr;
            client->regionWidth  = 0;
            client->regionHeight = 0;
            return;
        }

        client->device = eis_seat_new_device(client->seat);
        eis_device_configure_name(client->device, "enzremote EIS input");
        const auto configure = [&](eis_device_capability capability) {
            if (client->capabilities & capability)
                eis_device_configure_capability(client->device, capability);
        };
        configure(EIS_DEVICE_CAP_POINTER);
        configure(EIS_DEVICE_CAP_POINTER_ABSOLUTE);
        configure(EIS_DEVICE_CAP_BUTTON);
        configure(EIS_DEVICE_CAP_SCROLL);
        configure(EIS_DEVICE_CAP_KEYBOARD);
        configure(EIS_DEVICE_CAP_TOUCH);

        if (client->capabilities & (EIS_DEVICE_CAP_POINTER_ABSOLUTE | EIS_DEVICE_CAP_TOUCH)) {
            const auto [width, height] = viewport();
            auto region                = eis_device_new_region(client->device);
            eis_region_set_size(region, width, height);
            eis_region_set_mapping_id(region, mappingId.c_str());
            eis_region_add(region);
            eis_region_unref(region);
            client->regionWidth  = width;
            client->regionHeight = height;
        }

        eis_device_add(client->device);
        eis_device_resume(client->device);

        if (!client->pointer && (client->capabilities & (EIS_DEVICE_CAP_POINTER | EIS_DEVICE_CAP_POINTER_ABSOLUTE))) {
            client->pointer = CEISPointer::create();
            g_pInputManager->newMouse(client->pointer);
        }
        if (!client->keyboard && (client->capabilities & EIS_DEVICE_CAP_KEYBOARD)) {
            client->keyboard = CEISKeyboard::create();
            g_pInputManager->newKeyboard(client->keyboard);
        }
        if (!client->touch && (client->capabilities & EIS_DEVICE_CAP_TOUCH)) {
            client->touch = CEISTouch::create();
            g_pInputManager->newTouchDevice(client->touch);
        }
    }

    void reconfigureDevices() {
        const auto [width, height] = viewport();
        for (auto& [_, client] : clients) {
            if (client->device && (client->regionWidth != width || client->regionHeight != height))
                configureDevice(client.get());
        }
    }

    SClient* clientFor(eis_event* event) {
        eis_client* client = eis_event_get_client(event);
        if (!client) {
            if (const auto device = eis_event_get_device(event); device)
                client = eis_device_get_client(device);
            else if (const auto seat = eis_event_get_seat(event); seat)
                client = eis_seat_get_client(seat);
        }
        const auto found = clients.find(client);
        return found == clients.end() ? nullptr : found->second.get();
    }
};

CEISInputManager::CEISInputManager() : m_impl(std::make_unique<SImpl>()) {
    const auto controlFdText = getenv("ENZREMOTE_EIS_CONTROL_FD");
    const auto socketPath    = getenv("ENZREMOTE_EIS_SOCKET");
    if ((!controlFdText || !*controlFdText) && (!socketPath || !*socketPath))
        return;

    m_impl->mappingId = getenv("ENZREMOTE_EIS_MAPPING_ID") ? getenv("ENZREMOTE_EIS_MAPPING_ID") : "enzremote-output";
    m_impl->context   = eis_new(this);
    if (!m_impl->context) {
        LOG(Log::ERR, "[enzremote] Failed to create the EIS context");
        return;
    }

    if (controlFdText && *controlFdText) {
        char* end         = nullptr;
        errno             = 0;
        const auto parsed = strtol(controlFdText, &end, 10);
        if (errno != 0 || !end || *end != '\0' || parsed < 0 || parsed > INT_MAX || eis_setup_backend_fd(m_impl->context) < 0) {
            LOG(Log::ERR, "[enzremote] Invalid EIS control descriptor: {}", controlFdText);
            m_impl->context = eis_unref(m_impl->context);
            return;
        }
        m_impl->controlFd = static_cast<int>(parsed);
        const auto flags  = fcntl(m_impl->controlFd, F_GETFD);
        if (flags >= 0)
            fcntl(m_impl->controlFd, F_SETFD, flags | FD_CLOEXEC);
        m_impl->controlSource = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, m_impl->controlFd, WL_EVENT_READABLE, onControl, this);
        if (!m_impl->controlSource) {
            LOG(Log::ERR, "[enzremote] Failed to register the EIS control source");
            close(m_impl->controlFd);
            m_impl->controlFd = -1;
            m_impl->context   = eis_unref(m_impl->context);
            return;
        }
    } else if (eis_setup_backend_socket(m_impl->context, socketPath) < 0) {
        LOG(Log::ERR, "[enzremote] Failed to create EIS socket at {}", socketPath);
        if (m_impl->context)
            m_impl->context = eis_unref(m_impl->context);
        return;
    }

    m_impl->eventSource = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, eis_get_fd(m_impl->context), WL_EVENT_READABLE, onEvent, this);
    if (!m_impl->eventSource) {
        LOG(Log::ERR, "[enzremote] Failed to register the EIS event source");
        m_impl->context = eis_unref(m_impl->context);
        return;
    }

    if (m_impl->controlFd >= 0)
        LOG(Log::INFO, "[enzremote] EIS input endpoint using inherited control fd {}", m_impl->controlFd);
    else
        LOG(Log::INFO, "[enzremote] EIS input endpoint listening at {}", socketPath);
    m_monitorLayoutChanged = Event::bus()->m_events.monitor.layoutChanged.listen([this] { m_impl->reconfigureDevices(); });
}

CEISInputManager::~CEISInputManager() {
    m_impl->clients.clear();
    if (m_impl->controlSource)
        wl_event_source_remove(m_impl->controlSource);
    if (m_impl->eventSource)
        wl_event_source_remove(m_impl->eventSource);
    if (m_impl->context)
        m_impl->context = eis_unref(m_impl->context);
    if (m_impl->controlFd >= 0)
        close(m_impl->controlFd);
}

bool CEISInputManager::active() const {
    return m_impl->context != nullptr;
}

int CEISInputManager::onEvent(int, uint32_t, void* data) {
    static_cast<CEISInputManager*>(data)->dispatch();
    return 0;
}

int CEISInputManager::onControl(int, uint32_t, void* data) {
    static_cast<CEISInputManager*>(data)->dispatchControl();
    return 0;
}

void CEISInputManager::dispatchControl() {
    char       requests[16];
    const auto count = recv(m_impl->controlFd, requests, sizeof(requests), 0);
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        return;
    if (count <= 0) {
        wl_event_source_remove(m_impl->controlSource);
        m_impl->controlSource = nullptr;
        close(m_impl->controlFd);
        m_impl->controlFd = -1;
        return;
    }

    for (ssize_t index = 0; index < count; ++index) {
        if (requests[index] != 'E')
            continue;
        const auto clientFd = eis_backend_fd_add_client(m_impl->context);
        if (clientFd < 0) {
            LOG(Log::ERR, "[enzremote] Failed to create an EIS client descriptor: {}", clientFd);
            continue;
        }

        char   payload = 'E';
        iovec  iov     = {.iov_base = &payload, .iov_len = 1};
        char   control[CMSG_SPACE(sizeof(int))]{};
        msghdr message         = {};
        message.msg_iov        = &iov;
        message.msg_iovlen     = 1;
        message.msg_control    = control;
        message.msg_controllen = sizeof(control);
        const auto header      = CMSG_FIRSTHDR(&message);
        header->cmsg_level     = SOL_SOCKET;
        header->cmsg_type      = SCM_RIGHTS;
        header->cmsg_len       = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header), &clientFd, sizeof(clientFd));
        if (sendmsg(m_impl->controlFd, &message, MSG_NOSIGNAL) != 1)
            LOG(Log::ERR, "[enzremote] Failed to transfer an EIS client descriptor");
        close(clientFd);
    }
}

void CEISInputManager::dispatch() {
    eis_dispatch(m_impl->context);
    while (auto event = eis_get_event(m_impl->context)) {
        const auto type   = eis_event_get_type(event);
        auto       client = m_impl->clientFor(event);
        const auto timeMs = eventTimeMs(event);

        switch (type) {
            case EIS_EVENT_CLIENT_CONNECT: {
                auto eisClient = eis_event_get_client(event);
                if (!eis_client_is_sender(eisClient)) {
                    LOG(Log::WARN, "[enzremote] Rejecting non-sender EIS client");
                    eis_client_disconnect(eisClient);
                    break;
                }

                eis_client_connect(eisClient);
                auto state    = std::make_unique<SImpl::SClient>();
                state->client = eisClient;
                state->seat   = eis_client_new_seat(eisClient, "enzremote");
                for (const auto capability :
                     {EIS_DEVICE_CAP_POINTER, EIS_DEVICE_CAP_POINTER_ABSOLUTE, EIS_DEVICE_CAP_BUTTON, EIS_DEVICE_CAP_SCROLL, EIS_DEVICE_CAP_KEYBOARD, EIS_DEVICE_CAP_TOUCH})
                    eis_seat_configure_capability(state->seat, capability);
                eis_seat_add(state->seat);
                m_impl->clients.emplace(eisClient, std::move(state));
                LOG(Log::INFO, "[enzremote] Accepted EIS sender {}", eis_client_get_name(eisClient));
                break;
            }
            case EIS_EVENT_CLIENT_DISCONNECT:
                if (client) {
                    client->neutralize(timeMs);
                    m_impl->clients.erase(client->client);
                }
                break;
            case EIS_EVENT_SEAT_BIND: {
                if (!client)
                    break;
                client->capabilities = 0;
                for (const auto capability :
                     {EIS_DEVICE_CAP_POINTER, EIS_DEVICE_CAP_POINTER_ABSOLUTE, EIS_DEVICE_CAP_BUTTON, EIS_DEVICE_CAP_SCROLL, EIS_DEVICE_CAP_KEYBOARD, EIS_DEVICE_CAP_TOUCH}) {
                    if (eis_event_seat_has_capability(event, capability))
                        client->capabilities |= capability;
                }
                m_impl->configureDevice(client);
                break;
            }
            case EIS_EVENT_DEVICE_STOP_EMULATING:
                if (client)
                    client->neutralize(timeMs);
                break;
            case EIS_EVENT_DEVICE_CLOSED:
                if (client) {
                    client->neutralize(timeMs);
                    if (client->device && eis_event_get_device(event) == client->device) {
                        eis_device_remove(client->device);
                        eis_device_unref(client->device);
                        client->device = nullptr;
                    }
                }
                break;
            case EIS_EVENT_POINTER_MOTION:
                if (client && client->pointer)
                    client->pointer->motion(timeMs, {eis_event_pointer_get_dx(event), eis_event_pointer_get_dy(event)});
                break;
            case EIS_EVENT_POINTER_MOTION_ABSOLUTE:
                if (client && client->pointer)
                    client->pointer->motionAbsolute(timeMs, normalizedPosition(event, false));
                break;
            case EIS_EVENT_BUTTON_BUTTON:
                if (client && client->pointer) {
                    const auto button  = eis_event_button_get_button(event);
                    const auto pressed = eis_event_button_get_is_press(event);
                    if (pressed)
                        client->pressedButtons.insert(button);
                    else
                        client->pressedButtons.erase(button);
                    client->pointer->button(timeMs, button, pressed);
                }
                break;
            case EIS_EVENT_SCROLL_DELTA:
                if (client && client->pointer) {
                    const auto dx = eis_event_scroll_get_dx(event);
                    const auto dy = eis_event_scroll_get_dy(event);
                    if (dx != 0.0)
                        client->pointer->axis(timeMs, WL_POINTER_AXIS_HORIZONTAL_SCROLL, dx);
                    if (dy != 0.0)
                        client->pointer->axis(timeMs, WL_POINTER_AXIS_VERTICAL_SCROLL, dy);
                }
                break;
            case EIS_EVENT_SCROLL_DISCRETE:
                if (client && client->pointer) {
                    const auto dx = eis_event_scroll_get_discrete_dx(event);
                    const auto dy = eis_event_scroll_get_discrete_dy(event);
                    if (dx != 0)
                        client->pointer->axis(timeMs, WL_POINTER_AXIS_HORIZONTAL_SCROLL, dx / 120.0, dx / 120);
                    if (dy != 0)
                        client->pointer->axis(timeMs, WL_POINTER_AXIS_VERTICAL_SCROLL, dy / 120.0, dy / 120);
                }
                break;
            case EIS_EVENT_KEYBOARD_KEY:
                if (client && client->keyboard) {
                    const auto key     = eis_event_keyboard_get_key(event);
                    const auto pressed = eis_event_keyboard_get_key_is_press(event);
                    if (pressed)
                        client->pressedKeys.insert(key);
                    else
                        client->pressedKeys.erase(key);
                    client->keyboard->key(timeMs, key, pressed);
                }
                break;
            case EIS_EVENT_TOUCH_DOWN:
                if (client && client->touch) {
                    const auto id = static_cast<int32_t>(eis_event_touch_get_id(event));
                    client->activeTouches.insert(id);
                    client->touch->down(timeMs, id, normalizedPosition(event, true));
                }
                break;
            case EIS_EVENT_TOUCH_MOTION:
                if (client && client->touch)
                    client->touch->motion(timeMs, static_cast<int32_t>(eis_event_touch_get_id(event)), normalizedPosition(event, true));
                break;
            case EIS_EVENT_TOUCH_UP:
                if (client && client->touch) {
                    const auto id = static_cast<int32_t>(eis_event_touch_get_id(event));
                    client->activeTouches.erase(id);
                    if (eis_event_touch_get_is_cancel(event))
                        client->touch->cancel(timeMs, id);
                    else
                        client->touch->up(timeMs, id);
                }
                break;
            case EIS_EVENT_FRAME:
                if (client) {
                    if (client->pointer)
                        client->pointer->frame();
                    if (client->touch)
                        client->touch->frame();
                }
                break;
            default: break;
        }
        eis_event_unref(event);
    }
}
