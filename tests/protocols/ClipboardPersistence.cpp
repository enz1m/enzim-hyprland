#include <managers/ClipboardPersistence.hpp>

#include <gtest/gtest.h>
#include <unistd.h>
#include <wayland-server-core.h>

using Hyprutils::OS::CFileDescriptor;

class CClipboardOffer : public IDataSource {
  public:
    std::map<std::string, std::string> data;
    std::vector<std::string>           requests;
    std::string                        stalledMime;
    CFileDescriptor                    stalled;

    std::vector<std::string>           mimes() override {
        std::vector<std::string> mimes;
        for (const auto& [mime, bytes] : data)
            mimes.push_back(mime);
        return mimes;
    }

    void send(const std::string& mime, CFileDescriptor fd) override {
        requests.push_back(mime);
        if (mime == stalledMime) {
            stalled = std::move(fd);
            return;
        }
        const auto& payload = data.at(mime);
        EXPECT_EQ(::write(fd.get(), payload.data(), payload.size()), payload.size());
    }

    void accepted(const std::string&) override {
        ;
    }
    void cancelled() override {
        ;
    }
    void error(uint32_t, const std::string&) override {
        ;
    }
};

class ClipboardPersistence : public testing::Test {
  protected:
    std::unique_ptr<wl_event_loop, decltype(&wl_event_loop_destroy)> loop{wl_event_loop_create(), wl_event_loop_destroy};
    CClipboardPersistence                                            cache{loop.get()};

    void                                                             pump() {
        ASSERT_EQ(wl_event_loop_dispatch(loop.get(), 0), 0);
        ASSERT_EQ(wl_event_loop_dispatch(loop.get(), 0), 0);
    }

    std::string paste(const std::string& mime) {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
            ADD_FAILURE() << "pipe2 failed";
            return {};
        }
        CFileDescriptor reader(fds[0]);
        cache.send(mime, CFileDescriptor(fds[1]));
        pump();
        char       buffer[256];
        const auto count = read(reader.get(), buffer, sizeof(buffer));
        return count > 0 ? std::string(buffer, count) : std::string{};
    }
};

TEST_F(ClipboardPersistence, servesAllCompletedFormatsAndDropsPendingOnOwnerExit) {
    CClipboardOffer offer;
    offer.data = {
        {"text/plain", "fixture plain"}, {"text/html", "<b>fixture</b>"},     {"text/uri-list", "file:///tmp/clipboard-fixture\r\n"},
        {"image/png", "\x89PNG"},        {"application/x-custom", "ignored"}, {"image/jpeg", "pending"},
    };
    offer.stalledMime = "image/jpeg";

    cache.capture(offer);
    pump();
    cache.finishCapture();

    EXPECT_EQ(cache.mimes(), (std::vector<std::string>{"image/png", "text/html", "text/plain", "text/uri-list"}));
    for (const auto& mime : cache.mimes())
        EXPECT_EQ(paste(mime), offer.data.at(mime));
}

TEST_F(ClipboardPersistence, doesNotReadPasswordManagerOffers) {
    CClipboardOffer offer;
    offer.data = {{"text/plain", "fixture"}, {"x-kde-passwordManagerHint", "secret"}};

    cache.capture(offer);
    pump();

    EXPECT_TRUE(offer.requests.empty());
    EXPECT_TRUE(cache.mimes().empty());
}
