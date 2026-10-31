// Test of the launcher's command line (tests/retry/t6_launcher.sh): GameArguments() for a few options.
// Exit 0 = all assertions hold.
#include <cstdio>

#include "game.h"

static int failures = 0;

static bool Has(const std::vector<wxString>& args, const wxString& a) {
    for (const wxString& s : args)
        if (s == a) return true;
    return false;
}

static void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

int main() {
    GameOptions o;
    o.program = "streetrod-enhanced";
    o.gameDir = "Game";
    Check(!Has(GameArguments(o), "--retry"), "default options: no --retry");
    Check(!Has(GameArguments(o), "--vsync"), "default options: no --vsync switch");
    Check(!Has(GameArguments(o), "--fps"), "default options: no --fps switch");
    Check(!Has(GameArguments(o), "--draw-distance"), "default options: no --draw-distance switch");
    Check(!Has(GameArguments(o), "--hd"), "default options: no --hd switch");
    Check(!Has(GameArguments(o), "--sky-scroll"), "default options: no --sky-scroll switch");
    o.retry = true;
    Check(Has(GameArguments(o), "--retry"), "retry on: --retry");
    o.demo = true;
    Check(Has(GameArguments(o), "--retry") && Has(GameArguments(o), "demo"),
          "retry and demo: both passed (the game ignores --retry in demo mode)");

    GameOptions o2;
    o2.program = "streetrod-enhanced";
    o2.gameDir = "Game";
    o2.vsync = false;
    Check(Has(GameArguments(o2), "--vsync") && Has(GameArguments(o2), "off"), "vsync off: --vsync off");
    o2.fps = 144;
    Check(Has(GameArguments(o2), "--fps") && Has(GameArguments(o2), "144"), "fps 144: --fps 144");
    o2.fps = 0;
    Check(Has(GameArguments(o2), "--fps") && Has(GameArguments(o2), "0"), "fps uncapped: --fps 0");
    o2.drawDistance = 120;
    Check(Has(GameArguments(o2), "--draw-distance") && Has(GameArguments(o2), "120"),
          "draw distance 120: --draw-distance 120");
    o2.hd = false;
    Check(Has(GameArguments(o2), "--hd") && Has(GameArguments(o2), "off"), "hd off: --hd off");
    o2.skyScroll = 100;
    Check(Has(GameArguments(o2), "--sky-scroll") && Has(GameArguments(o2), "100"), "sky scroll: --sky-scroll 100");

    return failures == 0 ? 0 : 1;
}
