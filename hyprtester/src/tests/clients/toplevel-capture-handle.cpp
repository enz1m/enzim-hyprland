#include "../../Log.hpp"
#include "../../hyprctlCompat.hpp"
#include "../shared.hpp"
#include "build.hpp"
#include "tests.hpp"

#include <chrono>
#include <csignal>
#include <format>
#include <optional>
#include <string>
#include <thread>

#include <hyprutils/os/Process.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using namespace Hyprutils::OS;
using namespace Hyprutils::Utils;

constexpr int CAPTURE_FAILED = 0;
constexpr int CAPTURE_BUFFER = 10;

static std::optional<std::string> windowAddress(const std::string& clients, const std::string& appID) {
    const auto CLASS = clients.find(std::format("\n\tclass: {}\n", appID));
    if (CLASS == std::string::npos)
        return std::nullopt;

    const auto HEADER = clients.rfind("Window ", CLASS);
    if (HEADER == std::string::npos)
        return std::nullopt;

    const auto START = HEADER + 7;
    const auto END   = clients.find(' ', START);
    if (END == std::string::npos)
        return std::nullopt;

    return clients.substr(START, END - START);
}

static int captureByHandle(const std::string& address) {
    CProcess process(std::format("{}/toplevel-capture-handle", binaryDir), {"capture", address});
    process.addEnv("WAYLAND_DISPLAY", WLDISPLAY);

    if (!process.runSync())
        return -1;

    NLog::log("{}toplevel-capture-handle {}: exit {}, {}", Colors::YELLOW, address, process.exitCode(), process.stdOut());
    return process.exitCode();
}

TEST_CASE(windowShareCaptureFailsForUnmappedWindow) {
    constexpr auto APP_ID = "hyprtester-unmapped-capture";

    CProcess       holder(std::format("{}/toplevel-capture-handle", binaryDir), {"unmapped", APP_ID});
    holder.addEnv("WAYLAND_DISPLAY", WLDISPLAY);
    if (!holder.runAsync())
        FAIL_TEST("Could not start unmapped toplevel client");

    CScopeGuard                guard = {[&]() {
        if (Tests::processAlive(holder.pid()))
            kill(holder.pid(), SIGKILL);
    }};

    std::optional<std::string> address;
    for (size_t i = 0; i < 50 && !address; ++i) {
        const auto CLIENTS = getFromSocket("a/clients");
        if (CLIENTS.contains("mapped: 0"))
            address = windowAddress(CLIENTS, APP_ID);
        if (!address)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!address)
        FAIL_TEST("Unmapped toplevel did not appear in the window list");

    ASSERT(Tests::windowCount(), 0);
    ASSERT(captureByHandle(*address), CAPTURE_FAILED);
    EXPECT_CONTAINS(getFromSocket("/version"), "Hyprland");
}

TEST_CASE(windowShareCaptureByHandleOffersBufferForMappedWindow) {
    constexpr auto APP_ID = "hyprtester-mapped-capture";

    CScopeGuard    guard = {[&]() { Tests::killAllWindows(); }};

    auto           kitty = Tests::spawnKitty(APP_ID);
    if (!kitty)
        FAIL_TEST("Could not spawn kitty with class: {}", APP_ID);

    const auto ADDRESS = windowAddress(getFromSocket("/clients"), APP_ID);
    if (!ADDRESS)
        FAIL_TEST("Mapped window address not found");

    ASSERT(captureByHandle(*ADDRESS), CAPTURE_BUFFER);
}

TEST_CASE(windowShareCaptureFailsForUnknownHandle) {
    ASSERT(captureByHandle("0x1"), CAPTURE_FAILED);
}
