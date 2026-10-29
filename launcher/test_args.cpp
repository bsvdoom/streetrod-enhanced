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
    o.retry = true;
    Check(Has(GameArguments(o), "--retry"), "retry on: --retry");
    o.demo = true;
    Check(Has(GameArguments(o), "--retry") && Has(GameArguments(o), "demo"),
          "retry and demo: both passed (the game ignores --retry in demo mode)");
    return failures == 0 ? 0 : 1;
}
