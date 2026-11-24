#include "ProtocolManager.hpp"

#include "../config/ConfigValue.hpp"

#include "../protocols/TearingControl.hpp"
#include "../protocols/FractionalScale.hpp"
#include "../protocols/XDGOutput.hpp"
#include "../protocols/CursorShape.hpp"
#include "../protocols/IdleInhibit.hpp"
#include "../protocols/RelativePointer.hpp"
#include "../protocols/XDGDecoration.hpp"
#include "../protocols/AlphaModifier.hpp"
#include "../protocols/GammaControl.hpp"
#include "../protocols/ForeignToplevel.hpp"
#include "../protocols/PointerGestures.hpp"
#include "../protocols/ForeignToplevelWlr.hpp"
#include "../protocols/ShortcutsInhibit.hpp"
#include "../protocols/TextInputV3.hpp"
#include "../protocols/PointerConstraints.hpp"
#include "../protocols/OutputPower.hpp"
#include "../protocols/XDGActivation.hpp"
#include "../protocols/IdleNotify.hpp"
#include "../protocols/LockNotify.hpp"
#include "../protocols/SessionLock.hpp"
#include "../protocols/InputMethodV2.hpp"
#include "../protocols/VirtualKeyboard.hpp"
#include "../protocols/VirtualPointer.hpp"
#include "../protocols/OutputManagement.hpp"
#include "../protocols/ServerDecorationKDE.hpp"
#include "../protocols/FocusGrab.hpp"
#include "../protocols/Tablet.hpp"
#include "../protocols/LayerShell.hpp"
#include "../protocols/PresentationTime.hpp"
#include "../protocols/XDGShell.hpp"
#include "../protocols/DataDeviceWlr.hpp"
#include "../protocols/PrimarySelection.hpp"
#include "../protocols/XWaylandShell.hpp"
#include "../protocols/Viewporter.hpp"
#include "../protocols/MesaDRM.hpp"
#include "../protocols/LinuxDMABUF.hpp"
#include "../protocols/DRMLease.hpp"
#include "../protocols/DRMSyncobj.hpp"
#include "../protocols/Screencopy.hpp"
#include "../protocols/ToplevelExport.hpp"
#include "../protocols/ToplevelMapping.hpp"
#include "../protocols/TextInputV1.hpp"
#include "../protocols/GlobalShortcuts.hpp"
#include "../protocols/XDGDialog.hpp"
#include "../protocols/SinglePixel.hpp"
#include "../protocols/SecurityContext.hpp"
#include "../protocols/CTMControl.hpp"
#include "../protocols/InputCapture.hpp"
#include "../protocols/HyprlandSurface.hpp"
#include "../protocols/ImageCaptureSource.hpp"
#include "../protocols/ImageCopyCapture.hpp"
#include "../protocols/BackgroundEffect.hpp"
#include "../protocols/core/Seat.hpp"
#include "../protocols/core/DataDevice.hpp"
#include "../protocols/core/Compositor.hpp"
#include "../protocols/core/Subcompositor.hpp"
#include "../protocols/core/Output.hpp"
#include "../protocols/core/Shm.hpp"
#include "../protocols/ColorManagement.hpp"
#include "../protocols/ContentType.hpp"
#include "../protocols/XDGTag.hpp"
#include "../protocols/XDGBell.hpp"
#include "../protocols/Hotkey.hpp"
#include "../protocols/ExtWorkspace.hpp"
#include "../protocols/ExtDataDevice.hpp"
#include "../protocols/PointerWarp.hpp"
#include "../protocols/Fifo.hpp"
#include "../protocols/CommitTiming.hpp"
#include "../protocols/XDGForeignV2.hpp"

#include "../output/Monitor.hpp"
#include "../event/EventBus.hpp"
#include "../render/Renderer.hpp"
#include "../Compositor.hpp"
#include "content-type-v1.hpp"

#include <aquamarine/buffer/Buffer.hpp>
#include <xf86drm.h>
#include <aquamarine/backend/Backend.hpp>
#include <hyprutils/memory/UniquePtr.hpp>

// ********************************************************************************************
// * IMPORTANT: make sure to .reset() any protocol UP's you create! (put reset in destructor) *
// * otherwise Hyprland might crash when exiting.                                             *
// ********************************************************************************************

// Retire the old wl_output global for `name` before a new one takes its map slot.
// It must NOT be destroyed while clients still hold resources bound to it: destroying
// them server-side makes each client's in-flight wl_output.release an "invalid object"
// protocol error, fatally disconnecting every bound client (mass client kill on a fast
// monitor unplug/replug, e.g. a power blip on an external screen).
static void retireOutputGlobal(const std::string& name) {
    if (!PROTO::outputs.contains(name))
        return;

    auto old = PROTO::outputs.at(name);
    PROTO::outputs.erase(name);

    if (!old->isDefunct())
        old->remove();

    if (old->hasBoundResources())
        PROTO::defunctOutputs.emplace_back(old);
}

void CProtocolManager::onMonitorModeChange(PHLMONITOR pMonitor) {
    const bool ISMIRROR = pMonitor->isMirror();

    // onModeChanged we check if the current mirror status matches the global.
    // mirrored outputs should have their global removed, as they are not physical parts of the
    // layout.

    if (ISMIRROR && PROTO::outputs.contains(pMonitor->m_name))
        PROTO::outputs.at(pMonitor->m_name)->remove();
    else if (!ISMIRROR && (!PROTO::outputs.contains(pMonitor->m_name) || PROTO::outputs.at(pMonitor->m_name)->isDefunct())) {
        retireOutputGlobal(pMonitor->m_name);
        auto p = PROTO::outputs.emplace(pMonitor->m_name,
                                        makeShared<CWLOutputProtocol>(&wl_output_interface, 4, std::format("WLOutput ({})", pMonitor->m_name), pMonitor->m_self.lock()));
        p.first->second->m_self = p.first->second;
    }

    if (PROTO::colorManagement && g_pCompositor->shouldChangePreferredImageDescription()) {
        LOG(Log::ERR, "FIXME: color management protocol is enabled, need a preferred image description id");
        PROTO::colorManagement->onImagePreferredChanged(0);
    }
}

CProtocolManager::CProtocolManager() {

    static const auto PENABLECM = CConfigValue<Config::INTEGER>("render:cm_enabled");
    static const auto PDEBUGCM  = CConfigValue<Config::INTEGER>("debug:full_cm_proto");
    static const auto PCMV1_2   = CConfigValue<Config::INTEGER>("experimental:wp_cm_1_2");
    static const auto PENABLECT = CConfigValue<Config::INTEGER>("render:commit_timing_enabled");

    // Outputs are a bit dumb, we have to agree.
    static auto P = Event::bus()->m_events.monitor.added.listen([this](PHLMONITOR M) {
        // ignore mirrored outputs. I don't think this will ever be hit as mirrors are applied after
        // this event is emitted iirc.
        if (M->isMirror())
            return;

        retireOutputGlobal(M->m_name);

        auto ref = makeShared<CWLOutputProtocol>(&wl_output_interface, 4, std::format("WLOutput ({})", M->m_name), M->m_self.lock());
        PROTO::outputs.emplace(M->m_name, ref);
        ref->m_self = ref;

        m_modeChangeListeners[M->m_name] = M->m_events.modeChanged.listen([this, M] { onMonitorModeChange(M); });
    });

    static auto P2 = Event::bus()->m_events.monitor.removed.listen([this](PHLMONITOR M) {
        if (!PROTO::outputs.contains(M->m_name))
            return;
        PROTO::outputs.at(M->m_name)->remove();
        m_modeChangeListeners.erase(M->m_name);
    });

    // Core
    PROTO::seat          = makeUnique<CWLSeatProtocol>(&wl_seat_interface, 9, "WLSeat");
    PROTO::data          = makeUnique<CWLDataDeviceProtocol>(&wl_data_device_manager_interface, 3, "WLDataDevice");
    PROTO::compositor    = makeUnique<CWLCompositorProtocol>(&wl_compositor_interface, 6, "WLCompositor");
    PROTO::subcompositor = makeUnique<CWLSubcompositorProtocol>(&wl_subcompositor_interface, 1, "WLSubcompositor");
    PROTO::shm           = makeUnique<CWLSHMProtocol>(&wl_shm_interface, 2, "WLSHM");

    // Extensions
    PROTO::viewport            = makeUnique<CViewporterProtocol>(&wp_viewporter_interface, 1, "Viewporter");
    PROTO::tearing             = makeUnique<CTearingControlProtocol>(&wp_tearing_control_manager_v1_interface, 1, "TearingControl");
    PROTO::fractional          = makeUnique<CFractionalScaleProtocol>(&wp_fractional_scale_manager_v1_interface, 1, "FractionalScale");
    PROTO::xdgOutput           = makeUnique<CXDGOutputProtocol>(&zxdg_output_manager_v1_interface, 3, "XDGOutput");
    PROTO::cursorShape         = makeUnique<CCursorShapeProtocol>(&wp_cursor_shape_manager_v1_interface, 2, "CursorShape");
    PROTO::idleInhibit         = makeUnique<CIdleInhibitProtocol>(&zwp_idle_inhibit_manager_v1_interface, 1, "IdleInhibit");
    PROTO::relativePointer     = makeUnique<CRelativePointerProtocol>(&zwp_relative_pointer_manager_v1_interface, 1, "RelativePointer");
    PROTO::xdgDecoration       = makeUnique<CXDGDecorationProtocol>(&zxdg_decoration_manager_v1_interface, 2, "XDGDecoration");
    PROTO::alphaModifier       = makeUnique<CAlphaModifierProtocol>(&wp_alpha_modifier_v1_interface, 1, "AlphaModifier");
    PROTO::gamma               = makeUnique<CGammaControlProtocol>(&zwlr_gamma_control_manager_v1_interface, 1, "GammaControl");
    PROTO::foreignToplevel     = makeUnique<CForeignToplevelProtocol>(&ext_foreign_toplevel_list_v1_interface, 1, "ForeignToplevel");
    PROTO::pointerGestures     = makeUnique<CPointerGesturesProtocol>(&zwp_pointer_gestures_v1_interface, 3, "PointerGestures");
    PROTO::foreignToplevelWlr  = makeUnique<CForeignToplevelWlrProtocol>(&zwlr_foreign_toplevel_manager_v1_interface, 3, "ForeignToplevelWlr");
    PROTO::shortcutsInhibit    = makeUnique<CKeyboardShortcutsInhibitProtocol>(&zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1, "ShortcutsInhibit");
    PROTO::textInputV1         = makeUnique<CTextInputV1Protocol>(&zwp_text_input_manager_v1_interface, 1, "TextInputV1");
    PROTO::textInputV3         = makeUnique<CTextInputV3Protocol>(&zwp_text_input_manager_v3_interface, 1, "TextInputV3");
    PROTO::constraints         = makeUnique<CPointerConstraintsProtocol>(&zwp_pointer_constraints_v1_interface, 1, "PointerConstraints");
    PROTO::outputPower         = makeUnique<COutputPowerProtocol>(&zwlr_output_power_manager_v1_interface, 1, "OutputPower");
    PROTO::activation          = makeUnique<CXDGActivationProtocol>(&xdg_activation_v1_interface, 1, "XDGActivation");
    PROTO::idle                = makeUnique<CIdleNotifyProtocol>(&ext_idle_notifier_v1_interface, 2, "IdleNotify");
    PROTO::lockNotify          = makeUnique<CLockNotifyProtocol>(&hyprland_lock_notifier_v1_interface, 1, "IdleNotify");
    PROTO::sessionLock         = makeUnique<CSessionLockProtocol>(&ext_session_lock_manager_v1_interface, 1, "SessionLock");
    PROTO::ime                 = makeUnique<CInputMethodV2Protocol>(&zwp_input_method_manager_v2_interface, 1, "IMEv2");
    PROTO::virtualKeyboard     = makeUnique<CVirtualKeyboardProtocol>(&zwp_virtual_keyboard_manager_v1_interface, 1, "VirtualKeyboard");
    PROTO::virtualPointer      = makeUnique<CVirtualPointerProtocol>(&zwlr_virtual_pointer_manager_v1_interface, 2, "VirtualPointer");
    PROTO::outputManagement    = makeUnique<COutputManagementProtocol>(&zwlr_output_manager_v1_interface, 4, "OutputManagement");
    PROTO::serverDecorationKDE = makeUnique<CServerDecorationKDEProtocol>(&org_kde_kwin_server_decoration_manager_interface, 1, "ServerDecorationKDE");
    PROTO::focusGrab           = makeUnique<CFocusGrabProtocol>(&hyprland_focus_grab_manager_v1_interface, 1, "FocusGrab");
    PROTO::tablet              = makeUnique<CTabletV2Protocol>(&zwp_tablet_manager_v2_interface, 1, "TabletV2");
    PROTO::layerShell          = makeUnique<CLayerShellProtocol>(&zwlr_layer_shell_v1_interface, 5, "LayerShell");
    PROTO::presentation        = makeUnique<CPresentationProtocol>(&wp_presentation_interface, 2, "Presentation");
    PROTO::xdgShell            = makeUnique<CXDGShellProtocol>(&xdg_wm_base_interface, 7, "XDGShell");
    PROTO::dataWlr             = makeUnique<CDataDeviceWLRProtocol>(&zwlr_data_control_manager_v1_interface, 2, "DataDeviceWlr");
    PROTO::primarySelection    = makeUnique<CPrimarySelectionProtocol>(&zwp_primary_selection_device_manager_v1_interface, 1, "PrimarySelection");
    PROTO::xwaylandShell       = makeUnique<CXWaylandShellProtocol>(&xwayland_shell_v1_interface, 1, "XWaylandShell");
    PROTO::toplevelMapping     = makeUnique<CToplevelMappingProtocol>(&hyprland_toplevel_mapping_manager_v1_interface, 1, "ToplevelMapping");
    PROTO::globalShortcuts     = makeUnique<CGlobalShortcutsProtocol>(&hyprland_global_shortcuts_manager_v1_interface, 1, "GlobalShortcuts");
    PROTO::xdgDialog           = makeUnique<CXDGDialogProtocol>(&xdg_wm_dialog_v1_interface, 1, "XDGDialog");
    PROTO::singlePixel         = makeUnique<CSinglePixelProtocol>(&wp_single_pixel_buffer_manager_v1_interface, 1, "SinglePixel");
    PROTO::securityContext     = makeUnique<CSecurityContextProtocol>(&wp_security_context_manager_v1_interface, 1, "SecurityContext");
    PROTO::ctm                 = makeUnique<CHyprlandCTMControlProtocol>(&hyprland_ctm_control_manager_v1_interface, 2, "CTMControl");
    PROTO::inputCapture        = makeUnique<CInputCaptureProtocol>(&hyprland_input_capture_manager_v1_interface, 1, "InputCapture");
    PROTO::hyprlandSurface     = makeUnique<CHyprlandSurfaceProtocol>(&hyprland_surface_manager_v1_interface, 2, "HyprlandSurface");
    PROTO::contentType         = makeUnique<CContentTypeProtocol>(&wp_content_type_manager_v1_interface, 1, "ContentType");
    PROTO::xdgTag              = makeUnique<CXDGToplevelTagProtocol>(&xdg_toplevel_tag_manager_v1_interface, 1, "XDGTag");
    PROTO::xdgBell             = makeUnique<CXDGSystemBellProtocol>(&xdg_system_bell_v1_interface, 1, "XDGBell");
    PROTO::hotkey              = makeUnique<CHotkeyProtocol>(&vicinae_hotkey_manager_v1_interface, 1, "Hotkey");
    PROTO::extWorkspace        = makeUnique<CExtWorkspaceProtocol>(&ext_workspace_manager_v1_interface, 1, "ExtWorkspace");
    PROTO::extDataDevice       = makeUnique<CExtDataDeviceProtocol>(&ext_data_control_manager_v1_interface, 1, "ExtDataDevice");
    PROTO::pointerWarp         = makeUnique<CPointerWarpProtocol>(&wp_pointer_warp_v1_interface, 1, "PointerWarp");
    PROTO::fifo                = makeUnique<CFifoProtocol>(&wp_fifo_manager_v1_interface, 1, "Fifo");
    PROTO::xdgForeignExporter  = makeUnique<CXDGForeignExporterProtocolV2>(&zxdg_exporter_v2_interface, 1, "XDGForeignExporter");
    PROTO::xdgForeignImporter  = makeUnique<CXDGForeignImporterProtocolV2>(&zxdg_importer_v2_interface, 1, "XDGForeignImporter");
    PROTO::backgroundEffect    = makeUnique<CBackgroundEffectProtocol>(&ext_background_effect_manager_v1_interface, 1, "BackgroundEffect");

    if (*PENABLECT)
        PROTO::commitTiming = makeUnique<CCommitTimingProtocol>(&wp_commit_timing_manager_v1_interface, 1, "CommitTiming");

    // Screensharing Protocols
    PROTO::screencopy         = makeUnique<CScreencopyProtocol>(&zwlr_screencopy_manager_v1_interface, 3, "Screencopy");
    PROTO::toplevelExport     = makeUnique<CToplevelExportProtocol>(&hyprland_toplevel_export_manager_v1_interface, 2, "ToplevelExport");
    PROTO::imageCaptureSource = makeUnique<CImageCaptureSourceProtocol>(); // ctor inits actual protos, output and toplevel
    PROTO::imageCopyCapture   = makeUnique<CImageCopyCaptureProtocol>(&ext_image_copy_capture_manager_v1_interface, 1, "ImageCopyCapture");

    if (*PENABLECM)
        PROTO::colorManagement = makeUnique<CColorManagementProtocol>(&wp_color_manager_v1_interface, *PCMV1_2 ? 3 : 1, "ColorManagement", *PDEBUGCM);

    // ! please read the top of this file before adding another protocol

    for (auto const& b : g_pCompositor->m_aqBackend->getImplementations()) {
        if (b->type() != Aquamarine::AQ_BACKEND_DRM)
            continue;

        auto lease = makeShared<CDRMLeaseProtocol>(&wp_drm_lease_device_v1_interface, 1, "DRMLease", b);
        if (lease->good())
            PROTO::lease.emplace(lease->getDeviceName(), lease);
        else
            lease.reset();

        if (g_pHyprRenderer->explicitSyncSupported() && !PROTO::sync) {
            if (g_pCompositor->supportsDrmSyncobjTimeline()) {
                PROTO::sync = makeUnique<CDRMSyncobjProtocol>(&wp_linux_drm_syncobj_manager_v1_interface, 1, "DRMSyncobj");
                LOG(Log::DEBUG, "DRM Syncobj Timeline support detected, enabling explicit sync protocol");
            } else
                LOG(Log::WARN, "DRM Syncobj Timeline not supported, skipping explicit sync protocol");
        }
    }

    if (!g_pHyprRenderer->getDRMFormats().empty()) {
        PROTO::mesaDRM  = makeUnique<CMesaDRMProtocol>(&wl_drm_interface, 2, "MesaDRM");
        PROTO::linuxDma = makeUnique<CLinuxDMABufV1Protocol>(&zwp_linux_dmabuf_v1_interface, 5, "LinuxDMABUF");
    } else
        LOG(Log::WARN, "ProtocolManager: Not binding linux-dmabuf and MesaDRM: DMABUF not available");
}

CProtocolManager::~CProtocolManager() {
    // this is dumb but i don't want to replace all 600 PROTO with the right thing

    // Output
    PROTO::outputs.clear();
    PROTO::defunctOutputs.clear();

    // Core
    PROTO::seat.reset();
    PROTO::data.reset();
    PROTO::compositor.reset();
    PROTO::subcompositor.reset();
    PROTO::shm.reset();

    // Extensions
    PROTO::viewport.reset();
    PROTO::tearing.reset();
    PROTO::fractional.reset();
    PROTO::xdgOutput.reset();
    PROTO::cursorShape.reset();
    PROTO::idleInhibit.reset();
    PROTO::relativePointer.reset();
    PROTO::xdgDecoration.reset();
    PROTO::alphaModifier.reset();
    PROTO::gamma.reset();
    PROTO::foreignToplevel.reset();
    PROTO::pointerGestures.reset();
    PROTO::foreignToplevelWlr.reset();
    PROTO::shortcutsInhibit.reset();
    PROTO::textInputV1.reset();
    PROTO::textInputV3.reset();
    PROTO::constraints.reset();
    PROTO::outputPower.reset();
    PROTO::activation.reset();
    PROTO::idle.reset();
    PROTO::lockNotify.reset();
    PROTO::sessionLock.reset();
    PROTO::ime.reset();
    PROTO::virtualKeyboard.reset();
    PROTO::virtualPointer.reset();
    PROTO::outputManagement.reset();
    PROTO::serverDecorationKDE.reset();
    PROTO::focusGrab.reset();
    PROTO::tablet.reset();
    PROTO::layerShell.reset();
    PROTO::presentation.reset();
    PROTO::xdgShell.reset();
    PROTO::dataWlr.reset();
    PROTO::primarySelection.reset();
    PROTO::xwaylandShell.reset();
    PROTO::screencopy.reset();
    PROTO::toplevelExport.reset();
    PROTO::toplevelMapping.reset();
    PROTO::globalShortcuts.reset();
    PROTO::xdgDialog.reset();
    PROTO::singlePixel.reset();
    PROTO::securityContext.reset();
    PROTO::ctm.reset();
    PROTO::inputCapture.reset();
    PROTO::hyprlandSurface.reset();
    PROTO::contentType.reset();
    PROTO::colorManagement.reset();
    PROTO::xdgTag.reset();
    PROTO::xdgBell.reset();
    PROTO::hotkey.reset();
    PROTO::extWorkspace.reset();
    PROTO::extDataDevice.reset();
    PROTO::pointerWarp.reset();
    PROTO::fifo.reset();
    PROTO::xdgForeignExporter.reset();
    PROTO::xdgForeignImporter.reset();
    PROTO::commitTiming.reset();
    PROTO::imageCaptureSource.reset();
    PROTO::backgroundEffect.reset();

    for (auto& [_, lease] : PROTO::lease) {
        lease.reset();
    }
    PROTO::sync.reset();
    PROTO::mesaDRM.reset();
    PROTO::linuxDma.reset();
}

bool CProtocolManager::isGlobalPrivileged(const wl_global* global) {
    if (!global)
        return false;

    for (auto& [k, v] : PROTO::outputs) {
        if (global == v->getGlobal())
            return false;
    }

    // this is a static whitelist of allowed protocols,
    // outputs are dynamic so we checked them above
    // clang-format off
    static const std::vector<wl_global*> ALLOWED_WHITELIST = {
        PROTO::seat->getGlobal(),
        PROTO::data->getGlobal(),
        PROTO::compositor->getGlobal(),
        PROTO::subcompositor->getGlobal(),
        PROTO::shm->getGlobal(),
        PROTO::viewport->getGlobal(),
        // Output names, logical positions and sizes: what wl_output already
        // tells a sandboxed client, in the form toolkits use for scaling.
        PROTO::xdgOutput->getGlobal(),
        PROTO::tearing->getGlobal(),
        PROTO::fractional->getGlobal(),
        PROTO::cursorShape->getGlobal(),
        PROTO::idleInhibit->getGlobal(),
        PROTO::relativePointer->getGlobal(),
        PROTO::xdgDecoration->getGlobal(),
        PROTO::alphaModifier->getGlobal(),
        PROTO::pointerGestures->getGlobal(),
        PROTO::shortcutsInhibit->getGlobal(),
        PROTO::textInputV1->getGlobal(),
        PROTO::textInputV3->getGlobal(),
        PROTO::constraints->getGlobal(),
        PROTO::activation->getGlobal(),
        PROTO::idle->getGlobal(),
        PROTO::serverDecorationKDE->getGlobal(),
        PROTO::tablet->getGlobal(),
        PROTO::presentation->getGlobal(),
        PROTO::xdgShell->getGlobal(),
        PROTO::xdgDialog->getGlobal(),
        PROTO::singlePixel->getGlobal(),
        PROTO::primarySelection->getGlobal(),
		PROTO::hyprlandSurface->getGlobal(),
		PROTO::xdgTag->getGlobal(),
		PROTO::xdgBell->getGlobal(),
        PROTO::fifo->getGlobal(),
        PROTO::commitTiming->getGlobal(),
        PROTO::xdgForeignExporter->getGlobal(),
        PROTO::xdgForeignImporter->getGlobal(),
        PROTO::backgroundEffect->getGlobal(),
        PROTO::sync     ? PROTO::sync->getGlobal()      : nullptr,
        PROTO::mesaDRM  ? PROTO::mesaDRM->getGlobal()   : nullptr,
        PROTO::linuxDma ? PROTO::linuxDma->getGlobal()  : nullptr,
	PROTO::colorManagement ? PROTO::colorManagement->getGlobal() : nullptr,
    };
    // clang-format on

    return std::ranges::find(ALLOWED_WHITELIST, global) == ALLOWED_WHITELIST.end();
}

// enzUI: capture and virtual-input globals for identified processes only.
//
// With ENZUI_PRIVILEGED_CLIENTS=enforce, a native client sees the screen
// capture and virtual input globals only when the compositor can bind it
// to an approved process: the registered shell (input), a capture helper
// the shell announced just before starting it (one use, its own child, the
// announced executable), a process inside a cgroup named by a root-owned
// grant, the screen-sharing portal (capture), a helper that portal starts
// itself such as the screenshot or colour picker tool (capture), or the
// remote desktop portal (input, after its own consent dialog). Executable
// identity is the inode of /proc/PID/exe, never a name; LD_PRELOAD and
// LD_AUDIT disqualify. A session lock disconnects every admitted client but
// the shell, and while locked nothing else is admitted.
//
// Compositor control requests need the same kind of identity: see
// controlAllowed.
#include "SessionLockManager.hpp"
#include "../ipc/s1/S1.hpp"
#include <sys/stat.h>
#include <dirent.h>
#include <chrono>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <charconv>
#include <utility>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <hyprutils/os/FileDescriptor.hpp>

namespace EnzuiAdmission {
    using Hyprutils::OS::CFileDescriptor;

    static const std::string& setting(const char* name) {
        static const std::unordered_map<std::string, std::string> STARTUP = [] {
            std::unordered_map<std::string, std::string> values;
            for (const auto* key :
                 {"ENZUI_PRIVILEGED_CLIENTS", "ENZUI_SHELL_EXE", "ENZUI_SHELL_LAUNCHER_PID", "ENZUI_SHELL_LAUNCHER_START", "ENZUI_SHELL_LAUNCHER_FD", "ENZUI_PORTAL_EXES",
                  "ENZUI_PORTAL_INPUT_EXES", "ENZUI_PORTAL_HELPER_EXES", "ENZUI_DEV_PRIVILEGED_EXES", "ENZUI_CONTROL_EXES", "LD_LIBRARY_PATH"})
                values.emplace(key, getenv(key) ? getenv(key) : "");
            return values;
        }();
        return STARTUP.at(name);
    }

    static bool pidfdAlive(int fd) {
        pollfd interest{.fd = fd, .events = POLLIN};
        return fd >= 0 && poll(&interest, 1, 0) == 0;
    }

    struct SPeer {
        pid_t pid = 0;
        CFileDescriptor handle;
    };

    static SPeer peer(int fd) {
        ucred credentials{};
        socklen_t size = sizeof(credentials);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) || size != sizeof(credentials) || credentials.pid <= 0 || credentials.uid != getuid())
            return {};
        int handle = -1;
        size = sizeof(handle);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERPIDFD, &handle, &size))
            return {};
        CFileDescriptor owned(handle);
        if (size != sizeof(handle) || fcntl(handle, F_SETFD, FD_CLOEXEC) || !pidfdAlive(handle))
            return {};
        return {.pid = credentials.pid, .handle = std::move(owned)};
    }

    static thread_local const SPeer* controlPeer = nullptr;

    enum eKind : uint8_t {
        KIND_CAPTURE = 1,
        KIND_INPUT   = 2,
    };

    struct SProc {
        pid_t              ppid  = 0;
        unsigned long long start = 0;
    };

    struct SIntent {
        pid_t                                 shell = 0;
        unsigned long long                    shellStart = 0;
        dev_t                                 dev   = 0;
        ino_t                                 ino   = 0;
        uint8_t                               kinds = 0;
        std::chrono::steady_clock::time_point deadline;
    };

    struct SAdmitted {
        wl_listener destroy;
        uint8_t     kinds = 0;
        int         peerHandle = -1;
        wl_event_source* exitSource = nullptr;
        wl_client*  client = nullptr;
    };

    static std::vector<SIntent>                          intents;
    static std::unordered_map<const wl_client*, SAdmitted*> admitted;
    static pid_t                                         shellPid   = 0;
    static unsigned long long                            shellStart = 0;
    static CFileDescriptor                               shellHandle;
    static dev_t                                         shellDevice = 0;
    static ino_t                                         shellInode = 0;

    static bool enforcing() {
        return setting("ENZUI_PRIVILEGED_CLIENTS") == "enforce";
    }

    static uint8_t kindOf(const wl_global* global) {
        const auto* interface = wl_global_get_interface(global);
        if (!interface || !interface->name)
            return 0;
        const std::string_view name{interface->name};
        if (name == "zwlr_screencopy_manager_v1" || name == "hyprland_toplevel_export_manager_v1" || name == "ext_image_copy_capture_manager_v1" ||
            name == "ext_output_image_capture_source_manager_v1" || name == "ext_foreign_toplevel_image_capture_source_manager_v1")
            return KIND_CAPTURE;
        if (name == "zwlr_virtual_pointer_manager_v1" || name == "zwp_virtual_keyboard_manager_v1")
            return KIND_INPUT;
        return 0;
    }

    static bool readProc(pid_t pid, SProc& out) {
        std::ifstream stream("/proc/" + std::to_string(pid) + "/stat");
        std::string   text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        const auto    close = text.rfind(')');
        if (close == std::string::npos)
            return false;
        std::istringstream fields(text.substr(close + 1));
        std::string        field;
        for (int index = 0; fields >> field; ++index) {
            if (index == 0 && (field == "Z" || field == "X"))
                return false;
            if (index == 1)
                out.ppid = std::atoi(field.c_str());
            if (index == 19) {
                out.start = std::strtoull(field.c_str(), nullptr, 10);
                return true;
            }
        }
        return false;
    }

    static bool alive(pid_t pid, unsigned long long start) {
        SProc proc;
        return pid > 0 && readProc(pid, proc) && proc.start == start;
    }

    static bool injected(pid_t pid);
    static bool exeIs(pid_t pid, const std::string& path);
    static bool inode(const std::string& path, dev_t& dev, ino_t& ino);

    template <typename T> static bool number(const std::string& text, T& value) {
        if (text.empty() || text.size() > 20)
            return false;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size() && value > 0;
    }

    struct SLauncher {
        pid_t pid = 0;
        unsigned long long start = 0;
        CFileDescriptor handle;
    };

    static const SLauncher& launcher() {
        static const SLauncher VERIFIED = [] {
            SLauncher result;
            int inherited = -1;
            if (!number(setting("ENZUI_SHELL_LAUNCHER_PID"), result.pid) || !number(setting("ENZUI_SHELL_LAUNCHER_START"), result.start) ||
                !number(setting("ENZUI_SHELL_LAUNCHER_FD"), inherited) || inherited < 3)
                return SLauncher{};
            // The inherited capability pins the supervisor itself; a numeric
            // PID/start pair cannot turn a reused process into that supervisor.
            std::ifstream info("/proc/self/fdinfo/" + std::to_string(inherited));
            std::string line;
            pid_t target = 0;
            while (std::getline(info, line)) {
                if (line.starts_with("Pid:")) {
                    std::istringstream fields(line.substr(4));
                    fields >> target;
                }
            }
            if (target != result.pid || syscall(SYS_pidfd_send_signal, inherited, 0, nullptr, 0) || !pidfdAlive(inherited))
                return SLauncher{};
            result.handle = CFileDescriptor(fcntl(inherited, F_DUPFD_CLOEXEC, 3));
            close(inherited);
            unsetenv("ENZUI_SHELL_LAUNCHER_FD");
            struct stat owner{};
            if (!result.handle.isValid() || stat(("/proc/" + std::to_string(result.pid)).c_str(), &owner) || owner.st_uid != getuid() || !alive(result.pid, result.start))
                return SLauncher{};
            return result;
        }();
        return VERIFIED;
    }

    static bool claimShell(pid_t pid, unsigned long long start, int peerHandle) {
        const auto& parent = launcher();
        SProc proc;
        if (!pidfdAlive(parent.handle.get()) || !readProc(pid, proc) || proc.start != start || proc.ppid != parent.pid || !alive(parent.pid, parent.start) ||
            injected(pid) || !pidfdAlive(peerHandle))
            return false;
        dev_t device = 0;
        ino_t image = 0;
        if (!inode("/proc/" + std::to_string(pid) + "/exe", device, image))
            return false;
        if (shellPid && pidfdAlive(shellHandle.get()))
            return pid == shellPid && start == shellStart && device == shellDevice && image == shellInode;
        if (setting("ENZUI_SHELL_EXE").empty() || !exeIs(pid, setting("ENZUI_SHELL_EXE")))
            return false;
        CFileDescriptor handle(fcntl(peerHandle, F_DUPFD_CLOEXEC, 3));
        if (!handle.isValid() || !pidfdAlive(handle.get()))
            return false;
        intents.clear();
        shellPid   = pid;
        shellStart = start;
        shellHandle = std::move(handle);
        shellDevice = device;
        shellInode = image;
        return true;
    }

    static bool inode(const std::string& path, dev_t& dev, ino_t& ino) {
        struct stat st {};
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            return false;
        dev = st.st_dev;
        ino = st.st_ino;
        return true;
    }

    static bool exeIs(pid_t pid, const std::string& path) {
        dev_t exeDev = 0, pathDev = 0;
        ino_t exeIno = 0, pathIno = 0;
        return inode("/proc/" + std::to_string(pid) + "/exe", exeDev, exeIno) && inode(path, pathDev, pathIno) && exeDev == pathDev && exeIno == pathIno;
    }

    static bool exeInList(pid_t pid, const char* variable) {
        const auto& value = setting(variable);
        if (value.empty())
            return false;
        std::stringstream list(value);
        std::string       path;
        while (std::getline(list, path, ':')) {
            if (!path.empty() && path.front() == '/' && exeIs(pid, path))
                return true;
        }
        return false;
    }

    // Loader variables that would run other code inside an identified binary:
    // any preload or audit library, or a library search path other than the
    // one the compositor itself was started with.
    static bool injected(pid_t pid) {
        std::ifstream stream("/proc/" + std::to_string(pid) + "/environ");
        std::string   entry;
        const auto    mine = std::string{"LD_LIBRARY_PATH="} + setting("LD_LIBRARY_PATH");
        while (std::getline(stream, entry, '\0')) {
            if ((entry.starts_with("LD_PRELOAD=") || entry.starts_with("LD_AUDIT=")) && entry.find('=') + 1 < entry.size())
                return true;
            if (entry.starts_with("LD_LIBRARY_PATH=") && entry != mine && entry.size() > 16)
                return true;
        }
        return !stream.eof();
    }

    static std::string cgroupOf(pid_t pid) {
        std::ifstream stream("/proc/" + std::to_string(pid) + "/cgroup");
        std::string   line;
        while (std::getline(stream, line)) {
            if (line.starts_with("0::"))
                return line.substr(3);
        }
        return "";
    }

    static bool rootOwned(const struct stat& st) {
        return st.st_uid == 0 && !(st.st_mode & (S_IWGRP | S_IWOTH));
    }

    // Grants are root-owned, singly linked files in the OS's public, root-owned
    // grant directory (the private /run/enzimos stays 0700):
    //   uid=1000
    //   cgroup=/system.slice/system-enzimos\x2ddesktop\x2dsharing.slice/enzimos-desktop-sharing@1000.service
    //   kinds=capture,input
    static uint8_t granted(pid_t pid, uid_t uid) {
        static const char* DIRECTORY = "/run/enzimos-compositor-grants";
        struct stat        directory {};
        if (lstat(DIRECTORY, &directory) != 0 || !S_ISDIR(directory.st_mode) || !rootOwned(directory))
            return 0;
        const auto group = cgroupOf(pid);
        if (group.empty())
            return 0;
        uint8_t kinds = 0;
        DIR*    listing = opendir(DIRECTORY);
        if (!listing)
            return 0;
        for (int count = 0; const auto* entry = readdir(listing); ++count) {
            const std::string name{entry->d_name};
            if (count > 64)
                break;
            if (!name.ends_with(".grant"))
                continue;
            const auto  path = std::string{DIRECTORY} + "/" + name;
            struct stat file {};
            if (lstat(path.c_str(), &file) != 0 || !S_ISREG(file.st_mode) || !rootOwned(file) || file.st_nlink != 1 || file.st_size > 4096)
                continue;
            std::ifstream stream(path);
            std::string   line, cgroup;
            long long     grantUid   = -1;
            uint8_t       grantKinds = 0;
            while (std::getline(stream, line)) {
                if (line.starts_with("uid="))
                    grantUid = std::atoll(line.c_str() + 4);
                else if (line.starts_with("cgroup="))
                    cgroup = line.substr(7);
                else if (line.starts_with("kinds=")) {
                    if (line.find("capture") != std::string::npos)
                        grantKinds |= KIND_CAPTURE;
                    if (line.find("input") != std::string::npos)
                        grantKinds |= KIND_INPUT;
                }
            }
            if (grantUid == (long long)uid && cgroup.starts_with("/system.slice/") && (group == cgroup || group.starts_with(cgroup + "/")))
                kinds |= grantKinds;
        }
        closedir(listing);
        return kinds;
    }

    static void forget(wl_listener* listener, void*) {
        SAdmitted* entry = wl_container_of(listener, entry, destroy);
        if (entry->exitSource)
            wl_event_source_remove(entry->exitSource);
        if (entry->peerHandle >= 0)
            close(entry->peerHandle);
        std::erase_if(admitted, [entry](const auto& pair) { return pair.second == entry; });
        wl_list_remove(&listener->link);
        delete entry;
    }

    static int peerExited(int, uint32_t, void* data) {
        wl_client_destroy(static_cast<SAdmitted*>(data)->client);
        return 0;
    }

    static uint8_t decide(const wl_client* client) {
        if (const auto known = admitted.find(client); known != admitted.end())
            return pidfdAlive(known->second->peerHandle) ? known->second->kinds : 0;
        auto owner = peer(wl_client_get_fd(const_cast<wl_client*>(client)));
        const auto pid = owner.pid;
        SProc proc;
        if (pid <= 0 || !readProc(pid, proc))
            return 0;
        uint8_t kinds = 0;
        if (claimShell(pid, proc.start, owner.handle.get()))
            kinds |= KIND_INPUT;
        if (!injected(pid)) {
            if (exeInList(pid, "ENZUI_PORTAL_EXES"))
                kinds |= KIND_CAPTURE;
            if (exeInList(pid, "ENZUI_DEV_PRIVILEGED_EXES"))
                kinds |= KIND_CAPTURE | KIND_INPUT;
            if (exeInList(pid, "ENZUI_PORTAL_INPUT_EXES"))
                kinds |= KIND_INPUT;
            if (exeInList(pid, "ENZUI_PORTAL_HELPER_EXES") && exeInList(proc.ppid, "ENZUI_PORTAL_EXES") && !injected(proc.ppid))
                kinds |= KIND_CAPTURE;
            const auto now = std::chrono::steady_clock::now();
            std::erase_if(intents, [now](const auto& intent) {
                return now > intent.deadline || intent.shell != shellPid || intent.shellStart != shellStart || !pidfdAlive(shellHandle.get());
            });
            dev_t exeDev = 0;
            ino_t exeIno = 0;
            if (inode("/proc/" + std::to_string(pid) + "/exe", exeDev, exeIno)) {
                for (auto it = intents.begin(); it != intents.end(); ++it) {
                    if (proc.ppid == it->shell && exeDev == it->dev && exeIno == it->ino) {
                        kinds |= it->kinds;
                        intents.erase(it);
                        break;
                    }
                }
            }
        }
        kinds |= granted(pid, getuid());
        SProc finalProc;
        if (!pidfdAlive(owner.handle.get()) || !readProc(pid, finalProc) || finalProc.start != proc.start || finalProc.ppid != proc.ppid)
            return 0;
        if (kinds) {
            auto* entry            = new SAdmitted;
            entry->kinds           = kinds;
            entry->client          = const_cast<wl_client*>(client);
            entry->peerHandle      = fcntl(owner.handle.get(), F_DUPFD_CLOEXEC, 3);
            if (entry->peerHandle >= 0)
                entry->exitSource = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, entry->peerHandle, WL_EVENT_READABLE, peerExited, entry);
            if (!entry->exitSource) {
                if (entry->peerHandle >= 0)
                    close(entry->peerHandle);
                delete entry;
                return 0;
            }
            entry->destroy.notify  = forget;
            wl_client_add_destroy_listener(const_cast<wl_client*>(client), &entry->destroy);
            admitted[client] = entry;
            LOG(Log::DEBUG, "enzUI admission: pid {} admitted for {}", pid, (int)kinds);
        }
        return kinds;
    }

    static bool isShell(const wl_client* client) {
        auto owner = peer(wl_client_get_fd(const_cast<wl_client*>(client)));
        SProc proc;
        return owner.pid > 0 && readProc(owner.pid, proc) && claimShell(owner.pid, proc.start, owner.handle.get());
    }

    static bool unchanged(const SPeer& owner, const SProc& proc) {
        SProc current;
        return pidfdAlive(owner.handle.get()) && readProc(owner.pid, current) && current.start == proc.start && current.ppid == proc.ppid;
    }

    // The supervisor's own control commands come from itself or its descendants
    // outside the shell's and the compositor's process trees.
    static bool supervised(pid_t pid, SProc proc) {
        const auto& parent = launcher();
        if (!parent.pid || !pidfdAlive(parent.handle.get()) || !alive(parent.pid, parent.start))
            return false;
        if (pid == parent.pid)
            return proc.start == parent.start;
        for (int depth = 0; depth < 4; ++depth) {
            if (proc.ppid == parent.pid)
                return true;
            const pid_t ancestor = proc.ppid;
            SProc       next;
            if (ancestor <= 1 || ancestor == shellPid || ancestor == getpid() || !readProc(ancestor, next) || next.start > proc.start ||
                exeIs(ancestor, setting("ENZUI_SHELL_EXE")))
                return false;
            proc = next;
        }
        return false;
    }

    static bool controlAllowed(const SPeer& owner) {
        SProc proc;
        if (owner.pid <= 0 || !readProc(owner.pid, proc))
            return false;
        const bool allowed = claimShell(owner.pid, proc.start, owner.handle.get()) || supervised(owner.pid, proc) ||
            (!injected(owner.pid) && (exeInList(owner.pid, "ENZUI_CONTROL_EXES") || exeInList(owner.pid, "ENZUI_DEV_PRIVILEGED_EXES")));
        return allowed && unchanged(owner, proc);
    }

    static std::string command(const IPC::Socket1::SRequest& request) {
        const pid_t pid = request.pid;
        SProc       proc;
        if (!controlPeer || controlPeer->pid != pid || !readProc(pid, proc) || !claimShell(pid, proc.start, controlPeer->handle.get()))
            return "error: not the registered shell";
        std::istringstream words(request.command);
        std::string        verb, action;
        words >> verb >> action;
        if (action != "intent")
            return "error: unknown request";
        std::string kindsText, path;
        long long   ttl = 0;
        words >> kindsText >> path >> ttl;
        SIntent intent;
        intent.shell      = pid;
        intent.shellStart = proc.start;
        if (kindsText == "capture" || kindsText == "capture,input")
            intent.kinds |= KIND_CAPTURE;
        if (kindsText == "input" || kindsText == "capture,input")
            intent.kinds |= KIND_INPUT;
        std::string extra;
        if (!intent.kinds || path.empty() || path.front() != '/' || !inode(path, intent.dev, intent.ino) || ttl < 100 || ttl > 10000 || intents.size() >= 16 || words >> extra)
            return "error: invalid intent";
        if (!pidfdAlive(controlPeer->handle.get()) || !pidfdAlive(launcher().handle.get()))
            return "error: peer exited";
        intent.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ttl);
        intents.push_back(intent);
        return "ok";
    }
}

IPC::Socket1::SResponse enzuiDispatchControlPeer(int fd, std::string request, pid_t pid,
    const std::function<IPC::Socket1::SResponse(std::string&&, pid_t)>& handler) {
    if (!EnzuiAdmission::enforcing())
        return handler(std::move(request), pid);
    auto owner = EnzuiAdmission::peer(fd);
    // Privileged handlers decide synchronously; deferred replies never retain
    // this borrowed context or authenticate another request through it.
    struct SScope {
        const EnzuiAdmission::SPeer* previous;
        ~SScope() { EnzuiAdmission::controlPeer = previous; }
    } scope{std::exchange(EnzuiAdmission::controlPeer, &owner)};
    return handler(std::move(request), owner.pid == pid ? pid : 0);
}

bool enzuiAllowsGlobal(const wl_client* client, const wl_global* global) {
    if (!EnzuiAdmission::enforcing())
        return true;
    const auto kind = EnzuiAdmission::kindOf(global);
    if (!kind)
        return true;
    if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked() && !EnzuiAdmission::isShell(client))
        return false;
    return EnzuiAdmission::decide(client) & kind;
}

bool enzuiAllowsControl() {
    if (!EnzuiAdmission::enforcing() || !EnzuiAdmission::controlPeer)
        return true;
    if (EnzuiAdmission::controlAllowed(*EnzuiAdmission::controlPeer))
        return true;
    LOG(Log::DEBUG, "enzUI admission: refused a control request from pid {}", EnzuiAdmission::controlPeer->pid);
    return false;
}

void enzuiRevokePrivileged() {
    if (!EnzuiAdmission::enforcing())
        return;
    std::vector<wl_client*> revoked;
    for (const auto& [client, _] : EnzuiAdmission::admitted) {
        if (!EnzuiAdmission::isShell(client))
            revoked.push_back(const_cast<wl_client*>(client));
    }
    for (auto* client : revoked) {
        LOG(Log::DEBUG, "enzUI admission: session locked, disconnecting a capture or input client");
        wl_client_destroy(client);
    }
}

void enzuiRegisterAdmissionCommand() {
    if (!EnzuiAdmission::enforcing() || !IPC::Socket1::sock())
        return;
    (void)EnzuiAdmission::launcher();
    IPC::Socket1::sock()->registerCommand(IPC::Socket1::SCommand{
        .name    = "enzuiadmit",
        .match   = IPC::Socket1::COMMAND_MATCH_PREFIX,
        .handler = [](const IPC::Socket1::SRequest& request) -> IPC::Socket1::SResponse { return EnzuiAdmission::command(request); },
    });
}
