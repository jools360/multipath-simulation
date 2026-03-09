#include "GNSSApp.h"

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);
#endif

    GNSSApp app;
    if (!app.init()) return 1;
    app.run();

#ifdef _WIN32
    timeEndPeriod(1);
#endif
    return 0;
}
