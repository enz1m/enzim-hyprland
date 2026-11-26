#pragma once

#include "WaylandProtocol.hpp"
#include "enzui-text-commit-v1.hpp"

class CEnzuiTextCommitProtocol : public IWaylandProtocol {
  public:
    using IWaylandProtocol::IWaylandProtocol;
    void bindManager(wl_client* client, void* data, uint32_t version, uint32_t id) override;

  private:
    void                                remove(CEnzuiTextCommitV1* resource);
    std::vector<UP<CEnzuiTextCommitV1>> m_resources;
};

namespace PROTO {
    inline UP<CEnzuiTextCommitProtocol> enzuiTextCommit;
}
