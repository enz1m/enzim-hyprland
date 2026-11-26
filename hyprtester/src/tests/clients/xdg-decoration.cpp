#include "../../hyprctlCompat.hpp"
#include "../../shared.hpp"
#include "../shared.hpp"
#include "build.hpp"
#include "tests.hpp"

#include <chrono>
#include <cstddef>
#include <csignal>
#include <exception>
#include <format>
#include <optional>
#include <string>
#include <thread>
#include <poll.h>
#include <unistd.h>
#include <hyprutils/os/FileDescriptor.hpp>
#include <vector>

#include <hyprutils/os/Process.hpp>

using namespace Hyprutils::Memory;
using namespace Hyprutils::OS;

class CDecorationClient {
  public:
    CDecorationClient(const std::string& request, bool interactive = false) {
        std::vector     args{request};
        CFileDescriptor input, output;
        if (interactive) {
            args.emplace_back("--interactive");
            int fds[2];
            if (pipe(fds) != 0)
                throw std::exception();
            input   = CFileDescriptor(fds[0]);
            m_input = CFileDescriptor(fds[1]);
            if (pipe(fds) != 0)
                throw std::exception();
            m_output = CFileDescriptor(fds[0]);
            output   = CFileDescriptor(fds[1]);
        }

        m_process = makeUnique<CProcess>(std::format("{}/xdg-decoration", binaryDir), args);
        if (interactive) {
            m_process->setStdinFD(input.get());
            m_process->setStdoutFD(output.get());
        }
        m_process->addEnv("WAYLAND_DISPLAY", WLDISPLAY);
        if (!m_process->runAsync())
            throw std::exception();

        for (size_t i = 0; i < 50; ++i) {
            if (!Tests::processAlive(m_process->pid()))
                throw std::exception();

            const auto CLIENTS = getFromSocket("/clients");
            if (Tests::windowCount() == 1 && CLIENTS.contains("class: xdg-decoration") && CLIENTS.contains("mapped: 1")) {
                Tests::sync();
                return;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        kill(m_process->pid(), SIGKILL);
        throw std::exception();
    }

    ~CDecorationClient() {
        if (m_process && Tests::processAlive(m_process->pid()))
            kill(m_process->pid(), SIGKILL);
        Tests::waitUntilWindowsN(0);
    }

    bool command(char command, std::string_view marker) {
        if (write(m_input.get(), &command, 1) != 1)
            return false;

        const auto DEADLINE = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < DEADLINE) {
            if (const auto END = m_pending.find(marker); END != std::string::npos) {
                m_pending.erase(0, END + marker.size());
                Tests::sync();
                return true;
            }
            pollfd fd{.fd = m_output.get(), .events = POLLIN};
            if (poll(&fd, 1, 100) <= 0)
                continue;
            char       buffer[256];
            const auto COUNT = read(fd.fd, buffer, sizeof(buffer));
            if (COUNT <= 0)
                return false;
            m_pending.append(buffer, COUNT);
        }
        return false;
    }

  private:
    CUniquePointer<CProcess> m_process;
    CFileDescriptor          m_input, m_output;
    std::string              m_pending;
};

static std::optional<std::string> tiledSize(const std::string& request) {
    try {
        CDecorationClient client(request);
        return Tests::getAttribute(getFromSocket("/activewindow"), "size");
    } catch (...) { return std::nullopt; }
}

TEST_CASE(serverSideDecorationReservesTitleBar) {
    const auto TITLEBAR = getFromSocket("/getoption decoration:titlebar:height");
    EXPECT_CONTAINS(TITLEBAR, "int: 44");

    EXPECT_OK(getFromSocket("/eval hl.config({decoration={titlebar={enabled=true}}}) return \"ok\""));
    const auto CLIENT = tiledSize("client");
    const auto SERVER = tiledSize("server");
    EXPECT_OK(getFromSocket("/eval hl.config({decoration={titlebar={enabled=false}}}) return \"ok\""));
    if (!CLIENT || !SERVER)
        FAIL_TEST("Failed to start the xdg-decoration client");

    const auto PARSE = [](const std::string& size) {
        const auto COMMA = size.find(',');
        return std::pair{std::stoi(size.substr(0, COMMA)), std::stoi(size.substr(COMMA + 1))};
    };
    const auto [CLIENTW, CLIENTH] = PARSE(*CLIENT);
    const auto [SERVERW, SERVERH] = PARSE(*SERVER);
    EXPECT(SERVERW, CLIENTW);
    EXPECT(SERVERH, CLIENTH - 44);
}

TEST_CASE(clientWithoutDecorationProtocolHasNoTitleBar) {
    EXPECT_OK(getFromSocket("/eval hl.config({decoration={titlebar={enabled=true}}}) return \"ok\""));
    const auto CLIENT = tiledSize("client");
    const auto NONE   = tiledSize("none");
    EXPECT_OK(getFromSocket("/eval hl.config({decoration={titlebar={enabled=false}}}) return \"ok\""));
    if (!CLIENT || !NONE)
        FAIL_TEST("Failed to start the xdg-decoration client");

    EXPECT(*NONE, *CLIENT);
}

TEST_CASE(titleBarIsOffByDefault) {
    const auto CLIENT = tiledSize("client");
    const auto SERVER = tiledSize("server");
    if (!CLIENT || !SERVER)
        FAIL_TEST("Failed to start the xdg-decoration client");

    EXPECT(*SERVER, *CLIENT);
}

TEST_CASE(titleBarModeFollowsSurfaceCommit) {
    CDecorationClient client("none", true);
    EXPECT_OK(getFromSocket("/eval hl.config({decoration={titlebar={enabled=true}}}) return \"ok\""));
    const auto HEIGHT = [] {
        const auto SIZE = Tests::getAttribute(getFromSocket("/activewindow"), "size");
        return std::stoi(SIZE.substr(SIZE.find(',') + 1));
    };
    const auto BEFORE = HEIGHT();

    const bool CREATED         = client.command('n', "mode: server");
    const auto AFTER_CONFIGURE = HEIGHT();
    const bool COMMITTED       = client.command('c', "processed: c");
    const auto AFTER_COMMIT    = HEIGHT();
    const bool DESTROYED       = client.command('d', "processed: d");
    const auto AFTER_DESTROY   = HEIGHT();
    const bool RESET           = client.command('c', "processed: c");
    const auto AFTER_RESET     = HEIGHT();
    EXPECT_OK(getFromSocket("/eval hl.config({decoration={titlebar={enabled=false}}}) return \"ok\""));

    EXPECT(CREATED && COMMITTED && DESTROYED && RESET, true);
    EXPECT(AFTER_CONFIGURE, BEFORE);
    EXPECT(AFTER_COMMIT, BEFORE - 44);
    EXPECT(AFTER_DESTROY, AFTER_COMMIT);
    EXPECT(AFTER_RESET, BEFORE);
}

TEST_CASE(titleBarKeepsContactOwnership) {
    CDecorationClient client("server");
    EXPECT_OK(getFromSocket("/eval hl.config({decoration={titlebar={enabled=true}},binds={drag_threshold=10},general={snap={enabled=false}}}) return \"ok\""));
    const auto FLOATED = getFromSocket("/dispatch hl.dsp.window.float({ action = 'enable' })");
    Tests::sync();
    const auto TOUCH = getFromSocket("/eval hl.plugin.test.test_title_bar_touch() return \"ok\"");
    const auto PEN   = getFromSocket("/eval hl.plugin.test.test_title_bar_pen() return \"ok\"");
    EXPECT_OK(getFromSocket("/eval hl.config({decoration={titlebar={enabled=false}},binds={drag_threshold=0},general={snap={enabled=true}}}) return \"ok\""));
    EXPECT_OK(FLOATED);
    EXPECT_OK(TOUCH);
    EXPECT_OK(PEN);
}
