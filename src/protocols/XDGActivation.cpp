#include "XDGActivation.hpp"
#include "../managers/TokenManager.hpp"
#include "../managers/SeatManager.hpp"
#include "../managers/SessionLockManager.hpp"
#include "../Compositor.hpp"
#include "../desktop/state/FocusState.hpp"
#include "core/Compositor.hpp"
#include "core/Seat.hpp"
#include <algorithm>

using namespace std::chrono_literals;

static constexpr auto ACTIVATION_INPUT_MAX_AGE = 1s;
static constexpr auto ACTIVATION_TOKEN_MAX_AGE = 3s;

static bool           containsSurface(SP<CWLSurfaceResource> root, SP<CWLSurfaceResource> surface) {
    return root && surface && (root == surface || root->findFirstPreorder([&surface](const auto& other) { return other == surface; }));
}

CXDGActivationToken::CXDGActivationToken(SP<CXdgActivationTokenV1> resource_) : m_resource(resource_) {
    if UNLIKELY (!resource_->resource())
        return;

    m_resource->setDestroy([this](CXdgActivationTokenV1* r) { PROTO::activation->destroyToken(this); });
    m_resource->setOnDestroy([this](CXdgActivationTokenV1* r) { PROTO::activation->destroyToken(this); });

    m_resource->setSetSerial([this](CXdgActivationTokenV1* r, uint32_t serial, wl_resource* seat) {
        if (m_committed)
            return;

        m_serial = serial;
        m_seat   = CWLSeatResource::fromResource(seat);
    });

    m_resource->setSetSurface([this](CXdgActivationTokenV1* r, wl_resource* surface) {
        if (!m_committed)
            m_surface = CWLSurfaceResource::fromResource(surface);
    });

    m_resource->setCommit([this](CXdgActivationTokenV1* r) {
        // TODO: should we send a protocol error of already_used here
        // if it was used? the protocol spec doesn't say _when_ it should be sent...
        if UNLIKELY (m_committed) {
            LOG(Log::WARN, "possible protocol error, two commits from one token. Ignoring.");
            return;
        }

        m_committed = true;
        m_token     = g_pTokenManager->registerNewToken({}, std::chrono::months{12});

        const auto SEAT    = m_seat.lock();
        const auto SURFACE = m_surface.lock();
        if (SEAT && SURFACE && SEAT->client() == r->client() && SURFACE->client() == r->client() && PROTO::activation->consumeInputSerial(SEAT, m_serial, SURFACE)) {
            PROTO::activation->m_userToken  = m_token;
            PROTO::activation->m_userSource = SURFACE;
        }

        m_resource->sendDone(m_token.c_str());

        PROTO::activation->m_sentTokens.push_back({m_token, m_resource->client()});

        auto count = std::ranges::count_if(PROTO::activation->m_sentTokens, [this](const auto& other) { return other.client == m_resource->client(); });

        if UNLIKELY (count > 10) {
            // remove first token. Too many, dear app.
            for (auto i = PROTO::activation->m_sentTokens.begin(); i != PROTO::activation->m_sentTokens.end(); ++i) {
                if (i->client == m_resource->client()) {
                    PROTO::activation->m_sentTokens.erase(i);
                    break;
                }
            }
        }
    });
}

CXDGActivationToken::~CXDGActivationToken() {
    if (m_committed)
        g_pTokenManager->removeToken(g_pTokenManager->getToken(m_token));
}

bool CXDGActivationToken::good() {
    return m_resource->resource();
}

CXDGActivationProtocol::CXDGActivationProtocol(const wl_interface* iface, const int& ver, const std::string& name) : IWaylandProtocol(iface, ver, name) {
    ;
}

void CXDGActivationProtocol::bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) {
    const auto RESOURCE = m_managers.emplace_back(makeUnique<CXdgActivationV1>(client, ver, id)).get();
    RESOURCE->setOnDestroy([this](CXdgActivationV1* p) { this->onManagerResourceDestroy(p->resource()); });

    RESOURCE->setDestroy([this](CXdgActivationV1* pMgr) { this->onManagerResourceDestroy(pMgr->resource()); });
    RESOURCE->setGetActivationToken([this](CXdgActivationV1* pMgr, uint32_t id) { this->onGetToken(pMgr, id); });
    RESOURCE->setActivate([this](CXdgActivationV1* pMgr, const char* token, wl_resource* surface) {
        auto TOKEN = std::ranges::find_if(m_sentTokens, [token](const auto& t) { return t.token == token; });

        if UNLIKELY (TOKEN == m_sentTokens.end()) {
            LOG(Log::WARN, "Ignoring activation with an unknown token");
            return;
        }

        const bool USER_TOKEN = TOKEN->token == m_userToken;
        const bool TRUSTED    = USER_TOKEN && Time::steadyNow() - m_inputTime <= ACTIVATION_TOKEN_MAX_AGE && containsSurface(m_userSource.lock(), Desktop::focusState()->surface());
        if (USER_TOKEN)
            invalidateInput();
        m_sentTokens.erase(TOKEN);

        SP<CWLSurfaceResource> surf    = CWLSurfaceResource::fromResource(surface);
        const auto             PWINDOW = Desktop::viewState()->query().type(Desktop::View::VIEW_TYPE_WINDOW).surface(surf).runWindow();

        if UNLIKELY (!PWINDOW) {
            LOG(Log::WARN, "Ignoring activation of a non-window or gone surface");
            return;
        }

        if (TRUSTED)
            PWINDOW->activateFromUserInput();
        else
            PWINDOW->activate();
    });
}

void CXDGActivationProtocol::invalidateInput() {
    m_inputSerials.clear();
    m_userToken.clear();
    m_userSource.reset();
}

void CXDGActivationProtocol::recordInputSerial(SP<CWLSeatResource> seat, uint32_t serial, SP<CWLSurfaceResource> surface) {
    if (!seat || !serial || !surface || g_pSessionLockManager->isSessionLocked() || m_inputSerials.size() >= MAX_SERIAL_STORE_LEN)
        return;

    if (m_inputSerials.empty())
        m_inputTime = Time::steadyNow();

    m_inputSerials.push_back({serial, seat, surface});
}

bool CXDGActivationProtocol::consumeInputSerial(SP<CWLSeatResource> seat, uint32_t serial, SP<CWLSurfaceResource> surface) {
    if (m_inputSerials.empty() || Time::steadyNow() - m_inputTime > ACTIVATION_INPUT_MAX_AGE || !containsSurface(surface, g_pSeatManager->m_state.keyboardFocus.lock()) ||
        !containsSurface(surface, Desktop::focusState()->surface()))
        return false;

    const bool MATCHES =
        std::ranges::any_of(m_inputSerials, [&](const auto& input) { return serial == input.serial && input.seat == seat && containsSurface(surface, input.surface.lock()); });
    // Multiple seat bindings may receive different serials for the same physical event.
    if (MATCHES)
        m_inputSerials.clear();

    return MATCHES;
}

void CXDGActivationProtocol::onManagerResourceDestroy(wl_resource* res) {
    std::erase_if(m_managers, [&](const auto& other) { return other->resource() == res; });
}

void CXDGActivationProtocol::destroyToken(CXDGActivationToken* token) {
    std::erase_if(m_tokens, [&](const auto& other) { return other.get() == token; });
}

void CXDGActivationProtocol::onGetToken(CXdgActivationV1* pMgr, uint32_t id) {
    const auto CLIENT   = pMgr->client();
    const auto RESOURCE = m_tokens.emplace_back(makeUnique<CXDGActivationToken>(makeShared<CXdgActivationTokenV1>(CLIENT, pMgr->version(), id))).get();

    if UNLIKELY (!RESOURCE->good()) {
        pMgr->noMemory();
        m_tokens.pop_back();
        return;
    }
}
