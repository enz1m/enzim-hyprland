#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <print>
#include <string>
#include <string_view>

#include <wayland-client.h>
#include <wayland.hpp>
#include <xdg-shell.hpp>
#include <hyprland-toplevel-export-v1.hpp>

#include <hyprutils/memory/Casts.hpp>
#include <hyprutils/memory/SharedPtr.hpp>

using namespace Hyprutils::Memory;

// Usage:
//   toplevel-capture-handle unmapped <app-id>   holds an xdg_toplevel that never attaches a buffer
//   toplevel-capture-handle capture <hex-addr>  requests a toplevel export by window handle
// Capture exits with EXIT_FAILED when the server answers `failed` before offering a buffer,
// EXIT_BUFFER when it offers a buffer, and EXIT_ERROR otherwise.

constexpr int EXIT_FAILED = 0;
constexpr int EXIT_BUFFER = 10;
constexpr int EXIT_ERROR  = 1;

struct SState {
    wl_display*                                       display = nullptr;
    CSharedPointer<CCWlRegistry>                      registry;
    CSharedPointer<CCWlCompositor>                    compositor;
    CSharedPointer<CCXdgWmBase>                       xdgWm;
    CSharedPointer<CCHyprlandToplevelExportManagerV1> exportMgr;

    CSharedPointer<CCWlSurface>                       surface;
    CSharedPointer<CCXdgSurface>                      xdgSurface;
    CSharedPointer<CCXdgToplevel>                     toplevel;

    CSharedPointer<CCHyprlandToplevelExportFrameV1>   frame;
    int                                               result = -1;
};

static void bindGlobals(SState& state) {
    state.registry = makeShared<CCWlRegistry>(rc<wl_proxy*>(wl_display_get_registry(state.display)));

    state.registry->setGlobal([&state](CCWlRegistry* r, uint32_t name, const char* iface, uint32_t ver) {
        const std::string_view IFACE = iface;
        auto*                  reg   = rc<wl_registry*>(r->resource());

        if (IFACE == "wl_compositor")
            state.compositor = makeShared<CCWlCompositor>(rc<wl_proxy*>(wl_registry_bind(reg, name, &wl_compositor_interface, std::min(ver, 4U))));
        else if (IFACE == "xdg_wm_base")
            state.xdgWm = makeShared<CCXdgWmBase>(rc<wl_proxy*>(wl_registry_bind(reg, name, &xdg_wm_base_interface, 1)));
        else if (IFACE == "hyprland_toplevel_export_manager_v1")
            state.exportMgr = makeShared<CCHyprlandToplevelExportManagerV1>(
                rc<wl_proxy*>(wl_registry_bind(reg, name, &hyprland_toplevel_export_manager_v1_interface, std::min(ver, 2U))));
    });
    state.registry->setGlobalRemove([](CCWlRegistry*, uint32_t) {});

    wl_display_roundtrip(state.display);
}

static int holdUnmappedToplevel(SState& state, const char* appID) {
    if (!state.compositor || !state.xdgWm)
        return EXIT_ERROR;

    state.xdgWm->setPing([](CCXdgWmBase* wm, uint32_t serial) { wm->sendPong(serial); });

    state.surface    = makeShared<CCWlSurface>(state.compositor->sendCreateSurface());
    state.xdgSurface = makeShared<CCXdgSurface>(state.xdgWm->sendGetXdgSurface(state.surface->resource()));
    state.toplevel   = makeShared<CCXdgToplevel>(state.xdgSurface->sendGetToplevel());

    state.xdgSurface->setConfigure([](CCXdgSurface* surface, uint32_t serial) { surface->sendAckConfigure(serial); });
    state.toplevel->setConfigure([](CCXdgToplevel*, int32_t, int32_t, wl_array*) {});
    state.toplevel->setClose([](CCXdgToplevel*) {});

    state.toplevel->sendSetAppId(appID);
    state.toplevel->sendSetTitle(appID);
    state.surface->sendCommit();

    while (wl_display_dispatch(state.display) >= 0) {
        ;
    }

    return 0;
}

static int captureHandle(SState& state, const char* address) {
    if (!state.exportMgr)
        return EXIT_ERROR;

    std::string_view hex = address;
    if (hex.starts_with("0x"))
        hex.remove_prefix(2);

    uint64_t handle = 0;
    try {
        handle = std::stoull(std::string{hex}, nullptr, 16);
    } catch (...) { return EXIT_ERROR; }

    state.frame = makeShared<CCHyprlandToplevelExportFrameV1>(state.exportMgr->sendCaptureToplevel(0, sc<uint32_t>(handle)));

    state.frame->setBuffer([&state](CCHyprlandToplevelExportFrameV1*, uint32_t, uint32_t w, uint32_t h, uint32_t) {
        std::println("buffer {}x{}", w, h);
        if (state.result < 0)
            state.result = EXIT_BUFFER;
    });
    state.frame->setLinuxDmabuf([](CCHyprlandToplevelExportFrameV1*, uint32_t, uint32_t, uint32_t) {});
    state.frame->setBufferDone([&state](CCHyprlandToplevelExportFrameV1*) {
        if (state.result < 0)
            state.result = EXIT_BUFFER;
    });
    state.frame->setFailed([&state](CCHyprlandToplevelExportFrameV1*) {
        std::println("failed");
        if (state.result < 0)
            state.result = EXIT_FAILED;
    });
    state.frame->setDamage([](CCHyprlandToplevelExportFrameV1*, uint32_t, uint32_t, uint32_t, uint32_t) {});
    state.frame->setFlags([](CCHyprlandToplevelExportFrameV1*, uint32_t) {});
    state.frame->setReady([](CCHyprlandToplevelExportFrameV1*, uint32_t, uint32_t, uint32_t) {});

    if (wl_display_roundtrip(state.display) < 0 || state.result < 0)
        return EXIT_ERROR;

    return state.result;
}

int main(int argc, char** argv) {
    if (argc != 3)
        return EXIT_ERROR;

    SState state;
    state.display = wl_display_connect(nullptr);
    if (!state.display)
        return EXIT_ERROR;

    bindGlobals(state);

    const std::string_view MODE = argv[1];
    int                    ret  = EXIT_ERROR;
    if (MODE == "unmapped")
        ret = holdUnmappedToplevel(state, argv[2]);
    else if (MODE == "capture")
        ret = captureHandle(state, argv[2]);

    std::fflush(stdout);
    auto* display = state.display;
    state         = {};
    wl_display_disconnect(display);
    return ret;
}
