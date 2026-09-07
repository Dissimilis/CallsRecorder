#pragma once
#include <cstdint>
#include <string>

// Decides when an auto-recording should start, when the meeting looks over,
// and when to stop. Pure logic, fed one observation per tick, so it can be
// tested without audio devices.
//
// Bias: better to record too much than to cut a call short. So:
//  - start as soon as a call app holds the mic;
//  - hard-stop only after the mic session has been gone for a while;
//  - when the mic session lingers but the call *looks* over (long silence,
//    or the app's meeting window is gone), ask the user first and give a
//    grace period; a click extends, silence stops.
class CallDetector {
public:
    struct Input {
        uint64_t nowMs = 0;
        bool valid = true;          // false = probe failed; tick is ignored
        bool recording = false;     // recorder currently running
        bool autoStarted = false;   // ...and it was this detector that started it
        bool micActive = false;     // call app holds an active capture session
        bool renderActive = false;  // same app has an active render session
        float renderPeak = -1;      // app render peak 0..1 (-1 = unknown)
        float recLevel = -1;        // recorder's own recent peak 0..1 (-1 = unknown)
        int meetingWindow = -1;     // 1 visible, 0 absent, -1 no heuristic
    };

    enum class Action { None, Start, PromptEnd, CancelPrompt, Stop };

    struct Tuning {
        uint64_t hardStopMs = 15000;     // mic session gone this long -> stop
        uint64_t silenceMs = 120000;     // no audio either way this long -> "seems over"
        uint64_t windowGoneMs = 20000;   // meeting window absent this long -> "seems over"
        uint64_t graceMs = 60000;        // time to click "keep recording"
        uint64_t extendMs = 15 * 60000;  // one click buys this much
        float silenceLevel = 0.02f;      // peak below this counts as silence
    };

    CallDetector() = default;
    explicit CallDetector(const Tuning& t) : t_(t) {}

    Action tick(const Input& in);

    // User asked to keep recording (balloon click / menu). Suppresses
    // soft stops for tuning.extendMs.
    void extend(uint64_t nowMs);

    // Called by the UI when a recording is stopped manually, so we don't
    // restart until the current call is gone.
    void manualStop() { rearm_ = true; gen_++; }
    void reset();

    bool promptPending() const { return promptAt_ != 0; }
    // Bumped whenever the user intervenes (extend / manual stop / reset), so
    // a start/stop decided before that can be recognised as stale.
    unsigned generation() const { return gen_; }
    uint64_t extendedUntil() const { return extendUntil_; }
    const wchar_t* reason() const { return reason_; }

private:
    Tuning t_;
    bool rearm_ = false;
    unsigned gen_ = 0;
    uint64_t micGoneSince_ = 0;
    uint64_t silentSince_ = 0;
    uint64_t windowGoneSince_ = 0;
    uint64_t promptAt_ = 0;
    uint64_t extendUntil_ = 0;
    const wchar_t* reason_ = L"";
};
