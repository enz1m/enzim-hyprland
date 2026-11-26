#include "EnzuiTextCommit.hpp"
#include "../managers/input/InputManager.hpp"

bool enzuiAllowsTextCommit(const wl_client* client);

void CEnzuiTextCommitProtocol::bindManager(wl_client* client, void* data, uint32_t version, uint32_t id) {
    const auto RESOURCE = m_resources.emplace_back(makeUnique<CEnzuiTextCommitV1>(client, version, id)).get();
    if (!RESOURCE->resource()) {
        m_resources.pop_back();
        wl_client_post_no_memory(client);
        return;
    }
    RESOURCE->setDestroy([this](auto* resource) { remove(resource); });
    RESOURCE->setOnDestroy([this](auto* resource) { remove(resource); });
    RESOURCE->setCommit([](auto* resource, const char* text, uint32_t spaceBefore) {
        const std::string_view WORDS{text};
        const bool             SENT =
            enzuiAllowsTextCommit(resource->client()) && !WORDS.empty() && WORDS.size() <= 4000 && spaceBefore <= 1 && g_pInputManager->m_relay.commitText(WORDS, spaceBefore == 1);
        resource->sendResult(SENT);
    });
}

void CEnzuiTextCommitProtocol::remove(CEnzuiTextCommitV1* resource) {
    std::erase_if(m_resources, [resource](const auto& other) { return other.get() == resource; });
}
