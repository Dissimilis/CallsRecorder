#pragma once
#include "common.h"

// Which mic + speaker is the call using right now?
struct DevicePick {
    std::wstring micId;     // WASAPI endpoint id ("" = none available)
    std::wstring renderId;
    std::wstring app;       // exe name of the detected call app ("" if fallback)
    bool tracked = false;   // true = found a live call app session; false = comm defaults
};

// Everything the call detector can observe about a call app in one scan.
struct CallProbe {
    bool valid = false;         // false = audio enumeration failed; ignore this probe
    bool micActive = false;     // a known call app has an active capture session
    std::wstring app;           // exe of that app ("" if none)
    float micPeak = -1;         // peak meter of the app's capture session (-1 = n/a)
    bool renderActive = false;  // the same app has an active render session
    float renderPeak = -1;      // max peak over the app's render sessions (-1 = n/a)
    int meetingWindow = -1;     // 1 = app's meeting window visible, 0 = not, -1 = no heuristic
};

namespace DeviceTracker {
// Scans active audio sessions on all capture devices for a known call app
// (Teams, Zoom, browsers, ...). If found, picks that mic plus the render
// device the same app is playing through. Otherwise falls back to the
// Windows default communications devices.
DevicePick Pick();

// Full signal scan for auto-record detection (see CallProbe).
CallProbe Probe();
}
