#ifndef NO_XWAYLAND

#include "XWayland.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"
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

    auto&      wm        = *g_pXWayland->m_wm;
    const auto selection = &m_selection == &wm.m_primarySelection ? HYPRATOMS["PRIMARY"] : HYPRATOMS["CLIPBOARD"];
    wm.receiveSelection(m_selection, selection, mimeAtom, XCB_TIME_CURRENT_TIME, std::move(fd));
}

void CXWM::receiveSelection(SXSelection& selection, xcb_atom_t atom, xcb_atom_t target, xcb_timestamp_t time, CFileDescriptor fd, std::function<void(bool)> done,
                            uint32_t generation) {
    auto transfer = makeUnique<SXTransfer>(selection);
    if (fd.isValid()) {
        const auto flags = fcntl(fd.get(), F_GETFL);
        if (flags != -1 && fcntl(fd.get(), F_SETFL, flags | O_NONBLOCK) != -1) {
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
        }
        if (!transfer->eventSource) {
            if (done)
                done(false);
            return;
        }
    } else if (target != HYPRATOMS["DELETE"]) {
        if (done)
            done(false);
        return;
    }

    const auto conn              = getConnection();
    transfer->incomingWindow     = xcb_generate_id(conn);
    transfer->incomingConnection = conn;
    transfer->incomingTarget     = target;
    transfer->incomingDone       = std::move(done);
    const uint32_t mask          = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, transfer->incomingWindow, m_screen->root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, m_screen->root_visual, XCB_CW_EVENT_MASK,
                      &mask);
    if (&selection == &m_dndSelection) {
        // Keep authorization and conversion in one X server transaction.
        xcb_grab_server(conn);
        if (!guardXDragRequestor(transfer->incomingWindow, generation)) {
            xcb_ungrab_server(conn);
            xcb_flush(conn);
            if (transfer->incomingDone)
                transfer->incomingDone(false);
            return;
        }
    }
    xcb_convert_selection(conn, transfer->incomingWindow, atom, target, HYPRATOMS["_WL_SELECTION"], time);
    if (&selection == &m_dndSelection) {
        xcb_ungrab_server(conn);
        const auto fail           = [&selection, requestor = transfer->incomingWindow](auto, auto) { selection.removeTransfer(requestor); };
        transfer->incomingTimeout = makeShared<CEventLoopTimer>(std::chrono::seconds(10), fail, nullptr);
        // Progress refreshes the idle timeout, so the budget bounds a steadily progressing source.
        transfer->incomingBudget = makeShared<CEventLoopTimer>(std::chrono::minutes(2), fail, nullptr);
        transfer->incomingLimit  = 1ULL << 30;
        g_pEventLoopManager->addTimer(transfer->incomingTimeout);
        g_pEventLoopManager->addTimer(transfer->incomingBudget);
    }
    xcb_flush(conn);

    selection.transfers.emplace_back(std::move(transfer));
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
