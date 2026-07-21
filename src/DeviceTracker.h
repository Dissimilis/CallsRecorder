#pragma once
#include "common.h"

// Which mic + speaker is the call using right now?
struct DevicePick {
    std::wstring micId;     // WASAPI endpoint id ("" = none available)
    std::wstring renderId;
    std::wstring app;       // exe name of the detected call app ("" if fallback)
    bool tracked = false;   // true = found a live call app session; false = comm defaults
};

namespace DeviceTracker {
// Scans active audio sessions on all capture devices for a known call app
// (Teams, Zoom, browsers, ...). If found, picks that mic plus the render
// device the same app is playing through. Otherwise falls back to the
// Windows default communications devices.
DevicePick Pick();
}
