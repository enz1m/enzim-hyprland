#include "TextInput.hpp"
#include "InputManager.hpp"
#include "../../protocols/TextInputV1.hpp"
#include "../../desktop/state/FocusState.hpp"
#include "../../protocols/TextInputV3.hpp"
#include "../../protocols/InputMethodV2.hpp"
#include "../../protocols/core/Compositor.hpp"

CTextInput::CTextInput(WP<CTextInputV1> ti) : m_v1Input(ti) {
    initCallbacks();
}

CTextInput::CTextInput(WP<CTextInputV3> ti) : m_v3Input(ti) {
    initCallbacks();
}

void CTextInput::initCallbacks() {
    if (isV3()) {
        const auto INPUT = m_v3Input.lock();

        m_listeners.enable  = INPUT->m_events.enable.listen([this] { onEnabled(); });
        m_listeners.disable = INPUT->m_events.disable.listen([this] { onDisabled(); });
        m_listeners.commit  = INPUT->m_events.onCommit.listen([this] { onCommit(); });
        m_listeners.reset   = INPUT->m_events.reset.listen([this] { onReset(); });
        m_listeners.destroy = INPUT->m_events.destroy.listen([this] { destroy(); });

        if (Desktop::focusState()->surface() && Desktop::focusState()->surface()->client() == INPUT->client())
            enter(Desktop::focusState()->surface());
    } else {
        const auto INPUT = m_v1Input.lock();

        m_listeners.enable  = INPUT->m_events.enable.listen([this](const auto& surface) { onEnabled(surface); });
        m_listeners.disable = INPUT->m_events.disable.listen([this] { onDisabled(); });
        m_listeners.commit  = INPUT->m_events.onCommit.listen([this] { onCommit(); });
        m_listeners.reset   = INPUT->m_events.reset.listen([this] { onReset(); });
        m_listeners.destroy = INPUT->m_events.destroy.listen([this] { destroy(); });
    }
}

void CTextInput::destroy() {
    m_listeners.surfaceUnmap.reset();
    m_listeners.surfaceDestroy.reset();

    g_pInputManager->m_relay.removeTextInput(this);

    if (!g_pInputManager->m_relay.getFocusedTextInput())
        g_pInputManager->m_relay.deactivateIME(nullptr, false);
}

void CTextInput::onEnabled(SP<CWLSurfaceResource> surfV1) {
    LOG(Log::DEBUG, "TI ENABLE");

    m_committedText = false;
    m_hasPreedit    = false;

    if (!isV3()) {
        if (g_pInputManager->m_relay.m_inputMethod.expired() || Desktop::focusState()->surface() != surfV1 || !m_v1Input->m_active)
            return;

        enter(surfV1);
    }

    if (focusedSurface() && focusedSurface() == Desktop::focusState()->surface())
        g_pInputManager->m_relay.activateIME(this);
}

void CTextInput::onDisabled() {
    m_committedText = false;
    m_hasPreedit    = false;

    if (!isV3())
        leave();

    if (!focusedSurface())
        return;

    const auto PFOCUSEDTI = g_pInputManager->m_relay.getFocusedTextInput();
    if (!PFOCUSEDTI || PFOCUSEDTI != this)
        return;

    g_pInputManager->m_relay.deactivateIME(this);
}

void CTextInput::onReset() {
    m_committedText = false;
    m_hasPreedit    = false;

    if (g_pInputManager->m_relay.m_inputMethod.expired())
        return;

    if (!focusedSurface())
        return;

    const auto PFOCUSEDTI = g_pInputManager->m_relay.getFocusedTextInput();
    if (!PFOCUSEDTI || PFOCUSEDTI != this)
        return;

    g_pInputManager->m_relay.deactivateIME(this, false);
    g_pInputManager->m_relay.activateIME(this);
}

void CTextInput::onCommit() {
    if (g_pInputManager->m_relay.m_inputMethod.expired()) {
        //   LOG(Log::WARN,  "Committing TextInput on no IME!");
        return;
    }

    if (!(isV3() ? m_v3Input->m_current.enabled.value : m_v1Input->m_active)) {
        LOG(Log::WARN, "Disabled TextInput commit?");
        return;
    }

    g_pInputManager->m_relay.commitIMEState(this);
}

void CTextInput::setFocusedSurface(SP<CWLSurfaceResource> pSurface) {
    if (pSurface == m_focusedSurface)
        return;

    m_focusedSurface = pSurface;

    if (!pSurface) {
        m_listeners.surfaceUnmap.reset();
        m_listeners.surfaceDestroy.reset();
        return;
    }

    // Kept after the surface leaves, so the deactivation that follows an unlock is still private.
    m_lockSurface = g_pSessionLockManager && g_pSessionLockManager->isSurfaceSessionLock(pSurface);

    m_listeners.surfaceUnmap.reset();
    m_listeners.surfaceDestroy.reset();

    m_listeners.surfaceUnmap = pSurface->m_events.unmap.listen([this] {
        LOG(Log::DEBUG, "Unmap TI owner1");

        if (m_enterLocks)
            m_enterLocks--;
        m_focusedSurface.reset();
        m_listeners.surfaceUnmap.reset();
        m_listeners.surfaceDestroy.reset();

        if (isV3() && !m_v3Input.expired() && m_v3Input->m_current.enabled.value) {
            m_v3Input->m_pending.enabled.value            = false;
            m_v3Input->m_pending.enabled.isDisablePending = false;
            m_v3Input->m_pending.enabled.isEnablePending  = false;
            m_v3Input->m_current.enabled.value            = false;
        }

        if (!g_pInputManager->m_relay.getFocusedTextInput())
            g_pInputManager->m_relay.deactivateIME(this);
    });

    m_listeners.surfaceDestroy = pSurface->m_events.destroy.listen([this] {
        LOG(Log::DEBUG, "Destroy TI owner1");

        if (m_enterLocks)
            m_enterLocks--;
        m_focusedSurface.reset();
        m_listeners.surfaceUnmap.reset();
        m_listeners.surfaceDestroy.reset();

        if (isV3() && !m_v3Input.expired() && m_v3Input->m_current.enabled.value) {
            m_v3Input->m_pending.enabled.value            = false;
            m_v3Input->m_pending.enabled.isDisablePending = false;
            m_v3Input->m_pending.enabled.isEnablePending  = false;
            m_v3Input->m_current.enabled.value            = false;
        }

        if (!g_pInputManager->m_relay.getFocusedTextInput())
            g_pInputManager->m_relay.deactivateIME(this);
    });
}

bool CTextInput::isV3() {
    return m_v3Input && !m_v1Input;
}

void CTextInput::enter(SP<CWLSurfaceResource> pSurface) {
    if (!pSurface || !pSurface->m_mapped)
        return;

    if (pSurface == focusedSurface())
        return;

    if (focusedSurface())
        leave();

    m_enterLocks++;
    if (m_enterLocks != 1) {
        LOG(Log::ERR, "BUG THIS: TextInput has != 1 locks in enter");
        leave();
        m_enterLocks = 1;
    }

    if (isV3())
        m_v3Input->enter(pSurface);
    else {
        m_v1Input->enter(pSurface);
    }

    setFocusedSurface(pSurface);
}

void CTextInput::leave() {
    if (!focusedSurface())
        return;

    m_enterLocks--;
    if (m_enterLocks != 0) {
        LOG(Log::ERR, "BUG THIS: TextInput has != 0 locks in leave");
        m_enterLocks = 0;
    }

    m_committedText = false;
    m_hasPreedit    = false;

    if (isV3())
        m_v3Input->leave(focusedSurface());
    else
        m_v1Input->leave();

    setFocusedSurface(nullptr);

    g_pInputManager->m_relay.deactivateIME(this);
}

SP<CWLSurfaceResource> CTextInput::focusedSurface() {
    return m_focusedSurface.lock();
}

bool CTextInput::onLockSurface() const {
    return m_lockSurface;
}

wl_client* CTextInput::client() {
    return isV3() ? m_v3Input->client() : m_v1Input->client();
}

void CTextInput::commitStateToIME(SP<CInputMethodV2> ime) {
    if (isV3() && !m_v3Input.expired()) {
        const auto INPUT = m_v3Input.lock();

        if (INPUT->m_current.surrounding.updated)
            ime->surroundingText(INPUT->m_current.surrounding.text, INPUT->m_current.surrounding.cursor, INPUT->m_current.surrounding.anchor);

        ime->textChangeCause(INPUT->m_current.cause);

        if (INPUT->m_current.contentType.updated)
            ime->textContentType(INPUT->m_current.contentType.hint, INPUT->m_current.contentType.purpose);
    } else if (!m_v1Input.expired()) {
        const auto INPUT = m_v1Input.lock();

        if (INPUT->m_pendingSurrounding.isPending)
            ime->surroundingText(INPUT->m_pendingSurrounding.text, INPUT->m_pendingSurrounding.cursor, INPUT->m_pendingSurrounding.anchor);

        ime->textChangeCause(ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD);

        if (m_v1Input->m_pendingContentType.isPending)
            ime->textContentType(sc<zwpTextInputV3ContentHint>(INPUT->m_pendingContentType.hint), sc<zwpTextInputV3ContentPurpose>(INPUT->m_pendingContentType.purpose));
    }

    g_pInputManager->m_relay.updateAllPopups();

    ime->done();
}

void CTextInput::updateIMEState(SP<CInputMethodV2> ime) {
    if (isV3()) {
        const auto INPUT = m_v3Input.lock();

        m_hasPreedit = ime->m_current.preeditString.committed && !ime->m_current.preeditString.string.empty();
        if (ime->m_current.preeditString.committed)
            INPUT->preeditString(ime->m_current.preeditString.string, ime->m_current.preeditString.begin, ime->m_current.preeditString.end);

        if (ime->m_current.committedString.committed)
            INPUT->commitString(ime->m_current.committedString.string);

        if (ime->m_current.deleteSurrounding.committed)
            INPUT->deleteSurroundingText(ime->m_current.deleteSurrounding.before, ime->m_current.deleteSurrounding.after);

        INPUT->sendDone();
    } else {
        const auto INPUT = m_v1Input.lock();

        if (ime->m_current.preeditString.committed) {
            INPUT->preeditCursor(ime->m_current.preeditString.begin);
            INPUT->preeditStyling(0, std::string(ime->m_current.preeditString.string).length(), ZWP_TEXT_INPUT_V1_PREEDIT_STYLE_HIGHLIGHT);
            INPUT->preeditString(m_v1Input->m_serial, ime->m_current.preeditString.string.c_str(), "");
        } else {
            INPUT->preeditCursor(0);
            INPUT->preeditStyling(0, 0, ZWP_TEXT_INPUT_V1_PREEDIT_STYLE_HIGHLIGHT);
            INPUT->preeditString(m_v1Input->m_serial, "", "");
        }

        if (ime->m_current.committedString.committed)
            INPUT->commitString(m_v1Input->m_serial, ime->m_current.committedString.string.c_str());

        if (ime->m_current.deleteSurrounding.committed) {
            INPUT->deleteSurroundingText(std::string(ime->m_current.preeditString.string).length() - ime->m_current.deleteSurrounding.before,
                                         ime->m_current.deleteSurrounding.after + ime->m_current.deleteSurrounding.before);

            if (ime->m_current.preeditString.committed)
                INPUT->commitString(m_v1Input->m_serial, ime->m_current.preeditString.string.c_str());
        }
    }
}

static bool followsWord(std::string_view before) {
    if (before.empty())
        return false;

    if (std::string_view{" \t\n\r\v\f([{\"'"}.contains(before.back()))
        return false;

    static constexpr std::string_view SEPARATORS[] = {
        "«",      "¿",      "¡",      "\u0085", "\u00a0", "\u1680", "\u2000", "\u2001", "\u2002", "\u2003", "\u2004",
        "\u2005", "\u2006", "\u2007", "\u2008", "\u2009", "\u200a", "\u2028", "\u2029", "\u202f", "\u205f", "\u3000",
    };
    return std::ranges::none_of(SEPARATORS, [before](auto separator) { return before.ends_with(separator); });
}

bool CTextInput::commitText(std::string_view text, bool spaceBefore) {
    if (m_hasPreedit)
        return false;

    const auto  INPUT       = m_v3Input.lock();
    const auto& SURROUNDING = INPUT->m_current.surrounding;
    const auto  CURSOR      = std::min(SURROUNDING.cursor, SURROUNDING.anchor);
    const bool  SPACE       = spaceBefore && (SURROUNDING.updated ? followsWord(std::string_view{SURROUNDING.text}.substr(0, CURSOR)) : m_committedText);

    INPUT->commitString(SPACE ? std::string{" "} + std::string{text} : std::string{text});
    INPUT->sendDone();
    m_committedText = true;
    return true;
}

void CTextInput::clearPreedit() {
    if (!m_hasPreedit)
        return;

    m_hasPreedit = false;
    if (auto input = m_v3Input.lock()) {
        input->preeditString("", 0, 0);
        input->sendDone();
    }
}

bool CTextInput::hasCursorRectangle() {
    return !isV3() || m_v3Input->m_current.box.updated;
}

CBox CTextInput::cursorBox() {
    return CBox{isV3() ? m_v3Input->m_current.box.cursorBox : m_v1Input->m_cursorRectangle};
}

bool CTextInput::isEnabled() {
    return isV3() ? m_v3Input->m_current.enabled.value : true;
}

SP<CTextInputV3> CTextInput::writingInput() {
    return isV3() && isEnabled() && !onLockSurface() ? m_v3Input.lock() : nullptr;
}
