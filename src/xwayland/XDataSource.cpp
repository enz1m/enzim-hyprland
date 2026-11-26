#ifndef NO_XWAYLAND

#include "XWayland.hpp"
#include "../defines.hpp"
#include "../Compositor.hpp"
#include "XDataSource.hpp"

#include <fcntl.h>
using namespace Hyprutils::OS;

CXDataSource::CXDataSource(SXSelection& sel_) : m_selection(sel_) {
    xcb_get_property_cookie_t cookie = xcb_get_property(g_pXWayland->m_wm->getConnection(),
                                                        1, // delete
                                                        m_selection.window, HYPRATOMS["_WL_SELECTION"], XCB_GET_PROPERTY_TYPE_ANY, 0, 4096);

    xcb_get_property_reply_t* reply = xcb_get_property_reply(g_pXWayland->m_wm->getConnection(), cookie, nullptr);
    if (!reply)
        return;

    if (reply->type != XCB_ATOM_ATOM) {
        free(reply); // NOLINT(cppcoreguidelines-no-malloc)
        return;
    }

    auto value = sc<xcb_atom_t*>(xcb_get_property_value(reply));
    for (uint32_t i = 0; i < reply->value_len; i++) {
        if (value[i] == HYPRATOMS["UTF8_STRING"])
            m_mimeTypes.emplace_back("text/plain;charset=utf-8");
        else if (value[i] == HYPRATOMS["TEXT"])
            m_mimeTypes.emplace_back("text/plain");
        else if (value[i] != HYPRATOMS["TARGETS"] && value[i] != HYPRATOMS["TIMESTAMP"]) {

            auto type = g_pXWayland->m_wm->mimeFromAtom(value[i]);

            if (type == "INVALID")
                continue;

            m_mimeTypes.push_back(type);
        } else
            continue;

        m_mimeAtoms.push_back(value[i]);
    }

    free(reply); // NOLINT(cppcoreguidelines-no-malloc)
}

std::vector<std::string> CXDataSource::mimes() {
    return m_mimeTypes;
}

void CXDataSource::send(const std::string& mime, CFileDescriptor fd) {
    xcb_atom_t mimeAtom = XCB_ATOM_NONE;
    for (size_t i = 0; i < m_mimeTypes.size(); ++i) {
        if (m_mimeTypes[i] == mime) {
            mimeAtom = m_mimeAtoms[i];
            break;
        }
    }

    if (!mimeAtom) {
        LOG(Log::ERR, "[XDataSource] mime atom not found");
        return;
    }

    LOG(Log::DEBUG, "[XDataSource] send with mime {} to fd {}", mime, fd.get());

    const auto flags = fcntl(fd.get(), F_GETFL);
    if (flags == -1 || fcntl(fd.get(), F_SETFL, flags | O_NONBLOCK) == -1)
        return;

    auto transfer         = makeUnique<SXTransfer>(m_selection);
    transfer->wlFD        = std::move(fd);
    transfer->eventSource = wl_event_loop_add_fd(
        g_pCompositor->m_wlEventLoop, transfer->wlFD.get(), 0,
        [](int, uint32_t mask, void* data) {
            auto* transfer = sc<SXTransfer*>(data);
            if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
                transfer->selection.removeTransfer(transfer->incomingWindow);
            else if (mask & WL_EVENT_WRITABLE)
                transfer->writeToWayland();
            return 0;
        },
        transfer.get());
    if (!transfer->eventSource)
        return;

    transfer->incomingWindow = xcb_generate_id(g_pXWayland->m_wm->getConnection());
    const uint32_t MASK      = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_create_window(g_pXWayland->m_wm->getConnection(), XCB_COPY_FROM_PARENT, transfer->incomingWindow, g_pXWayland->m_wm->m_screen->root, 0, 0, 10, 10, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, g_pXWayland->m_wm->m_screen->root_visual, XCB_CW_EVENT_MASK, &MASK);

    xcb_atom_t selection_atom = HYPRATOMS["CLIPBOARD"];
    if (&m_selection == &g_pXWayland->m_wm->m_primarySelection)
        selection_atom = HYPRATOMS["PRIMARY"];
    else if (&m_selection == &g_pXWayland->m_wm->m_dndSelection)
        selection_atom = HYPRATOMS["XdndSelection"];

    xcb_convert_selection(g_pXWayland->m_wm->getConnection(), transfer->incomingWindow, selection_atom, mimeAtom, HYPRATOMS["_WL_SELECTION"], XCB_TIME_CURRENT_TIME);

    xcb_flush(g_pXWayland->m_wm->getConnection());

    m_selection.transfers.emplace_back(std::move(transfer));
}

void CXDataSource::accepted(const std::string& mime) {
    LOG(Log::DEBUG, "[XDataSource] accepted is a stub");
}

void CXDataSource::cancelled() {
    LOG(Log::DEBUG, "[XDataSource] cancelled is a stub");
}

void CXDataSource::error(uint32_t code, const std::string& msg) {
    LOG(Log::DEBUG, "[XDataSource] error is a stub: err {}: {}", code, msg);
}

eDataSourceType CXDataSource::type() {
    return DATA_SOURCE_TYPE_X11;
}

#endif
