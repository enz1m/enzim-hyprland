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
#include <vector>

#include <hyprutils/os/Process.hpp>

using namespace Hyprutils::Memory;
using namespace Hyprutils::OS;

class CDecorationClient {
  public:
    CDecorationClient(const std::string& request) {
        m_process = makeUnique<CProcess>(std::format("{}/xdg-decoration", binaryDir), std::vector<std::string>{request});
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

  private:
    CUniquePointer<CProcess> m_process;
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
