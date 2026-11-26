#pragma once

#include "../protocols/types/DataDevice.hpp"
#include <list>
#include <map>
#include <memory>

struct wl_event_loop;
struct wl_event_source;

class CClipboardPersistence : public IDataSource {
  public:
    explicit CClipboardPersistence(wl_event_loop* loop);

    void                     capture(IDataSource& source);
    void                     finishCapture();
    std::vector<std::string> mimes() override;
    void                     send(const std::string& mime, Hyprutils::OS::CFileDescriptor fd) override;
    void                     accepted(const std::string&) override;
    void                     cancelled() override;
    void                     error(uint32_t, const std::string&) override;

  private:
    struct SRemoveEventSource {
        void operator()(wl_event_source* source) const;
    };
    using EventSource = std::unique_ptr<wl_event_source, SRemoveEventSource>;

    struct SRead {
        CClipboardPersistence&         owner;
        std::string                    mime;
        size_t                         limit;
        Hyprutils::OS::CFileDescriptor pipe;
        std::vector<uint8_t>           bytes;
        EventSource                    ready;
    };

    struct SWrite {
        CClipboardPersistence&         owner;
        const std::vector<uint8_t>&    bytes;
        Hyprutils::OS::CFileDescriptor pipe;
        size_t                         offset = 0;
        EventSource                    ready;
        EventSource                    timeout;
    };

    void                                        startRead(IDataSource& source, const std::string& mime, size_t limit);
    void                                        read(SRead& transfer);
    void                                        write(SWrite& transfer);
    void                                        stopWrite(SWrite& transfer);

    wl_event_loop*                              m_loop;
    std::list<SRead>                            m_reads;
    std::list<SWrite>                           m_writes;
    std::map<std::string, std::vector<uint8_t>> m_bytes;
    size_t                                      m_totalBytes = 0;
    EventSource                                 m_captureTimeout;
};
