#include "ClipboardPersistence.hpp"
#include <algorithm>
#include <string_view>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <ranges>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-server-core.h>

using Hyprutils::OS::CFileDescriptor;

static constexpr size_t MAX_FORMAT_BYTES    = 1024 * 1024;
static constexpr size_t MAX_IMAGE_BYTES     = 8 * MAX_FORMAT_BYTES;
static constexpr size_t MAX_TOTAL_BYTES     = 16 * MAX_FORMAT_BYTES;
static constexpr int    TRANSFER_TIMEOUT_MS = 3000;
static constexpr auto   FORMATS             = std::to_array<std::string_view>({
    "text/plain;charset=utf-8",
    "text/plain",
    "UTF8_STRING",
    "TEXT",
    "STRING",
    "text/html",
    "text/uri-list",
    "x-special/gnome-copied-files",
    "application/x-kde-cutselection",
    "image/png",
    "image/jpeg",
    "image/bmp",
});

static bool             setNonblocking(int fd) {
    const auto flags = fcntl(fd, F_GETFL);
    return flags != -1 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

static bool excludedMime(const std::string& mime) {
    auto lower = mime;
    for (auto& c : lower) {
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    }
    return lower.contains("password") || lower.contains("secret") || lower.contains("sensitive") || lower == "application/x-enzui-clipboard-restore";
}

void CClipboardPersistence::SRemoveEventSource::operator()(wl_event_source* source) const {
    wl_event_source_remove(source);
}

CClipboardPersistence::CClipboardPersistence(wl_event_loop* loop) : m_loop(loop) {
    ;
}

void CClipboardPersistence::capture(IDataSource& source) {
    const auto mimes = source.mimes();
    if (std::ranges::any_of(mimes, excludedMime))
        return;

    m_captureTimeout.reset(wl_event_loop_add_timer(
        m_loop,
        [](void* data) {
            sc<CClipboardPersistence*>(data)->finishCapture();
            return 0;
        },
        this));
    if (!m_captureTimeout || wl_event_source_timer_update(m_captureTimeout.get(), TRANSFER_TIMEOUT_MS) < 0)
        return;

    for (const auto mime : FORMATS) {
        const auto found = std::ranges::find(mimes, mime);
        if (found != mimes.end())
            startRead(source, *found, mime.starts_with("image/") ? MAX_IMAGE_BYTES : MAX_FORMAT_BYTES);
    }
}

void CClipboardPersistence::startRead(IDataSource& source, const std::string& mime, size_t limit) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0)
        return;
    CFileDescriptor reader(fds[0]), writer(fds[1]);
    if (!setNonblocking(reader.get()))
        return;

    auto& transfer = m_reads.emplace_back(SRead{.owner = *this, .mime = mime, .limit = limit, .pipe = std::move(reader), .bytes = {}, .ready = {}});
    transfer.ready.reset(wl_event_loop_add_fd(
        m_loop, transfer.pipe.get(), WL_EVENT_READABLE,
        [](int, uint32_t, void* data) {
            auto& read = *sc<SRead*>(data);
            read.owner.read(read);
            return 0;
        },
        &transfer));
    if (transfer.ready)
        source.send(mime, std::move(writer));
    else
        m_reads.pop_back();
}

void CClipboardPersistence::read(SRead& transfer) {
    std::array<uint8_t, 16384> buffer;
    const auto                 n = ::read(transfer.pipe.get(), buffer.data(), buffer.size());
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
        return;

    if (n > 0 && transfer.bytes.size() + n <= transfer.limit && m_totalBytes + n <= MAX_TOTAL_BYTES) {
        transfer.bytes.insert(transfer.bytes.end(), buffer.begin(), buffer.begin() + n);
        m_totalBytes += n;
        return;
    }

    if (n == 0 && !transfer.bytes.empty())
        m_bytes.emplace(std::move(transfer.mime), std::move(transfer.bytes));
    else
        m_totalBytes -= transfer.bytes.size();

    m_reads.remove_if([&](const auto& read) { return &read == &transfer; });
    if (m_reads.empty())
        m_captureTimeout.reset();
}

void CClipboardPersistence::finishCapture() {
    m_reads.clear();
    m_captureTimeout.reset();
}

std::vector<std::string> CClipboardPersistence::mimes() {
    return m_bytes | std::views::keys | std::ranges::to<std::vector>();
}

void CClipboardPersistence::send(const std::string& mime, CFileDescriptor fd) {
    auto        found   = m_bytes.find(mime);
    struct stat statBuf = {};
    if (found == m_bytes.end() || m_writes.size() >= 8 || fstat(fd.get(), &statBuf) < 0 || !S_ISFIFO(statBuf.st_mode) || !setNonblocking(fd.get()))
        return;

    auto& transfer = m_writes.emplace_back(SWrite{.owner = *this, .bytes = found->second, .pipe = std::move(fd), .ready = {}, .timeout = {}});
    transfer.ready.reset(wl_event_loop_add_fd(
        m_loop, transfer.pipe.get(), WL_EVENT_WRITABLE,
        [](int, uint32_t mask, void* data) {
            auto& write = *sc<SWrite*>(data);
            if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP))
                write.owner.stopWrite(write);
            else
                write.owner.write(write);
            return 0;
        },
        &transfer));
    transfer.timeout.reset(wl_event_loop_add_timer(
        m_loop,
        [](void* data) {
            auto& write = *sc<SWrite*>(data);
            write.owner.stopWrite(write);
            return 0;
        },
        &transfer));
    if (!transfer.ready || !transfer.timeout || wl_event_source_timer_update(transfer.timeout.get(), TRANSFER_TIMEOUT_MS) < 0)
        m_writes.pop_back();
}

void CClipboardPersistence::write(SWrite& transfer) {
    const auto remaining = transfer.bytes.size() - transfer.offset;
    const auto written   = ::write(transfer.pipe.get(), transfer.bytes.data() + transfer.offset, remaining);
    if (written < 0 && (errno == EAGAIN || errno == EINTR))
        return;
    if (written > 0 && sc<size_t>(written) < remaining)
        transfer.offset += written;
    else
        stopWrite(transfer);
}

void CClipboardPersistence::stopWrite(SWrite& transfer) {
    m_writes.remove_if([&](const auto& write) { return &write == &transfer; });
}

void CClipboardPersistence::accepted(const std::string&) {
    ;
}

void CClipboardPersistence::cancelled() {
    m_writes.clear();
}

void CClipboardPersistence::error(uint32_t, const std::string&) {
    ;
}
