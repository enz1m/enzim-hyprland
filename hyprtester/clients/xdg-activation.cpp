#include "../src/hyprctlCompat.hpp"
#include "../src/shared.hpp"

#include <chrono>
#include <cstdint>
#include <format>
#include <linux/input-event-codes.h>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

#include <wayland-client.h>
#include <xdg-activation-v1.hpp>
#include <xdg-shell.hpp>

#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/os/FileDescriptor.hpp>

using namespace std::chrono_literals;
using namespace Hyprutils::Memory;
using namespace Hyprutils::OS;

static void require(bool condition, const char* failure) {
    if (!condition)
        throw std::runtime_error(failure);
}

class CActivationClient {
  public:
    explicit CActivationClient(const char* appID);
    ~CActivationClient();

    void        press();
    std::string mint(wl_seat* seat, uint32_t serial);
    void        redeem(const std::string& token);

    wl_seat*    m_seat      = nullptr;
    wl_seat*    m_otherSeat = nullptr;
    uint32_t    m_enter     = 0;
    uint32_t    m_press     = 0;

  private:
    void                              bind(wl_registry* registry, uint32_t name, const char* interface);
    void                              roundtrip();

    wl_display*                       m_display    = nullptr;
    wl_compositor*                    m_compositor = nullptr;
    wl_shm*                           m_shm        = nullptr;
    wl_surface*                       m_surface    = nullptr;
    CSharedPointer<CCXdgWmBase>       m_shell;
    CSharedPointer<CCXdgSurface>      m_xdgSurface;
    CSharedPointer<CCXdgToplevel>     m_window;
    CSharedPointer<CCXdgActivationV1> m_activation;
};

CActivationClient::CActivationClient(const char* appID) {
    m_display = wl_display_connect(nullptr);
    require(m_display, "could not connect");
    const wl_registry_listener registryListener = {
        .global = [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) { static_cast<CActivationClient*>(data)->bind(registry, name, interface); },
        .global_remove = [](void*, wl_registry*, uint32_t) {},
    };
    auto* registry = wl_display_get_registry(m_display);
    wl_registry_add_listener(registry, &registryListener, this);
    roundtrip();
    wl_registry_destroy(registry);
    require(m_compositor && m_shm && m_seat && m_shell && m_activation, "required protocols missing");

    static const wl_keyboard_listener keyboardListener = {
        .keymap = [](void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) { close(fd); },
        .enter  = [](void* data, wl_keyboard*, uint32_t serial, wl_surface*, wl_array*) { static_cast<CActivationClient*>(data)->m_enter = serial; },
        .leave  = [](void*, wl_keyboard*, uint32_t, wl_surface*) {},
        .key =
            [](void* data, wl_keyboard*, uint32_t serial, uint32_t, uint32_t key, uint32_t state) {
                if (key == KEY_F24 && state == WL_KEYBOARD_KEY_STATE_PRESSED)
                    static_cast<CActivationClient*>(data)->m_press = serial;
            },
        .modifiers   = [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {},
        .repeat_info = [](void*, wl_keyboard*, int32_t, int32_t) {},
    };
    wl_keyboard_add_listener(wl_seat_get_keyboard(m_seat), &keyboardListener, this);

    constexpr int         BUFFER_SIZE = 256 * 256 * 4;
    const CFileDescriptor fd(memfd_create("xdg-activation-test", MFD_CLOEXEC));
    require(fd.isValid() && ftruncate(fd.get(), BUFFER_SIZE) == 0, "could not create buffer");
    auto* pool   = wl_shm_create_pool(m_shm, fd.get(), BUFFER_SIZE);
    auto* buffer = wl_shm_pool_create_buffer(pool, 0, 256, 256, 256 * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);

    m_surface    = wl_compositor_create_surface(m_compositor);
    m_xdgSurface = makeShared<CCXdgSurface>(m_shell->sendGetXdgSurface(reinterpret_cast<wl_proxy*>(m_surface)));
    m_window     = makeShared<CCXdgToplevel>(m_xdgSurface->sendGetToplevel());
    m_shell->setPing([](CCXdgWmBase* shell, uint32_t serial) { shell->sendPong(serial); });
    m_window->sendSetAppId(appID);
    m_xdgSurface->setConfigure([this, buffer](CCXdgSurface* surface, uint32_t serial) {
        surface->sendAckConfigure(serial);
        wl_surface_attach(m_surface, buffer, 0, 0);
        wl_surface_commit(m_surface);
    });
    wl_surface_commit(m_surface);
    roundtrip();
    roundtrip();
}

CActivationClient::~CActivationClient() {
    m_window.reset();
    m_xdgSurface.reset();
    m_shell.reset();
    m_activation.reset();
    wl_display_disconnect(m_display);
}

void CActivationClient::bind(wl_registry* registry, uint32_t name, const char* interface) {
    const std::string_view IFACE = interface;
    if (IFACE == "wl_compositor")
        m_compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 1));
    else if (IFACE == "wl_shm")
        m_shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    else if (IFACE == "wl_seat") {
        m_seat      = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
        m_otherSeat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
    } else if (IFACE == "xdg_wm_base")
        m_shell = makeShared<CCXdgWmBase>(static_cast<wl_proxy*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1)));
    else if (IFACE == "xdg_activation_v1")
        m_activation = makeShared<CCXdgActivationV1>(static_cast<wl_proxy*>(wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1)));
}

void CActivationClient::roundtrip() {
    require(wl_display_roundtrip(m_display) >= 0, "Wayland roundtrip failed");
}

void CActivationClient::press() {
    const auto PREVIOUS = m_press;
    require(getFromSocket(std::format("/eval hl.plugin.test.keybind(1, 0, {})", KEY_F24 + 8)) == "ok", "key press failed");
    require(getFromSocket(std::format("/eval hl.plugin.test.keybind(0, 0, {})", KEY_F24 + 8)) == "ok", "key release failed");
    roundtrip();
    require(m_press && m_press != PREVIOUS, "no key press received on the source window");
}

std::string CActivationClient::mint(wl_seat* seat, uint32_t serial) {
    const auto  REQUEST = makeShared<CCXdgActivationTokenV1>(m_activation->sendGetActivationToken());
    std::string token;
    REQUEST->setDone([&token](CCXdgActivationTokenV1*, const char* value) { token = value; });
    REQUEST->sendSetSurface(reinterpret_cast<wl_proxy*>(m_surface));
    if (seat)
        REQUEST->sendSetSerial(serial, reinterpret_cast<wl_proxy*>(seat));
    REQUEST->sendCommit();
    roundtrip();
    REQUEST->sendDestroy();
    roundtrip();
    require(!token.empty(), "no activation token received");
    return token;
}

void CActivationClient::redeem(const std::string& token) {
    m_activation->sendActivate(token.c_str(), reinterpret_cast<wl_proxy*>(m_surface));
    roundtrip();
}

static bool focused(const char* appID) {
    return getFromSocket("/activewindow").contains(std::format("class: {}\n", appID));
}

int main(int argc, char** argv) try {
    require(argc == 2, "expected compositor instance signature");
    HIS = argv[1];

    CActivationClient target("activation-target");
    CActivationClient source("activation-source");
    require(getFromSocket("/dispatch hl.dsp.focus({window = 'class:activation-source'})") == "ok", "could not focus source");

    target.redeem(source.mint(nullptr, 0));
    require(focused("activation-source"), "activation without a serial moved focus");

    require(source.m_enter != 0, "source did not receive keyboard enter");
    target.redeem(source.mint(source.m_seat, source.m_enter));
    require(focused("activation-source"), "keyboard enter authorized activation");

    source.press();
    target.redeem(source.mint(source.m_otherSeat, source.m_press));
    require(focused("activation-source"), "wrong seat resource authorized activation");

    const auto TOKEN = source.mint(source.m_seat, source.m_press);
    target.redeem(source.mint(source.m_seat, source.m_press));
    require(focused("activation-source"), "one key press authorized two tokens");
    target.redeem(TOKEN);
    require(focused("activation-target"), "recent key press did not authorize cross-client activation");

    require(getFromSocket("/dispatch hl.dsp.focus({window = 'class:activation-source'})") == "ok", "could not refocus source");
    target.redeem(TOKEN);
    require(focused("activation-source"), "redeeming a spent token moved focus");

    source.press();
    const auto SUPERSEDED = source.mint(source.m_seat, source.m_press);
    source.press();
    target.redeem(SUPERSEDED);
    require(focused("activation-source"), "newer input did not supersede the minted token");

    std::this_thread::sleep_for(1100ms);
    target.redeem(source.mint(source.m_seat, source.m_press));
    require(focused("activation-source"), "expired input authorized activation");

    require(getFromSocket("/eval hl.window_rule({match = {class = 'activation-target'}, focus_on_activate = false})") == "ok", "could not set target rule");
    source.press();
    target.redeem(source.mint(source.m_seat, source.m_press));
    require(focused("activation-source"), "user activation bypassed explicit target rule");

    return 0;
} catch (const std::exception& error) {
    std::println(stderr, "{}", error.what());
    return 1;
}
