#include "../../hyprctlCompat.hpp"
#include "../shared.hpp"
#include "build.hpp"
#include "tests.hpp"

#include <hyprutils/os/Process.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using namespace Hyprutils::OS;
using namespace Hyprutils::Utils;

TEST_CASE(xdgActivationRequiresRecentInput) {
    CScopeGuard guard{[&] {
        Tests::killAllWindows();
        getFromSocket("/reload");
    }};

    OK(getFromSocket("/eval hl.config({ misc = {focus_on_activate = false}, input = {follow_mouse = 0} })"));
    CProcess process(std::format("{}/xdg-activation", binaryDir), {HIS});
    process.addEnv("WAYLAND_DISPLAY", WLDISPLAY);
    ASSERT(process.runSync(), true);
    EXPECT(process.exitCode(), 0);
}
