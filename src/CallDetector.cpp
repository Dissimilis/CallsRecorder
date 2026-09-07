#include "CallDetector.h"

void CallDetector::reset() {
    gen_++;
    micGoneSince_ = silentSince_ = windowGoneSince_ = promptAt_ = extendUntil_ = 0;
    reason_ = L"";
}

void CallDetector::extend(uint64_t nowMs) {
    gen_++;
    extendUntil_ = nowMs + t_.extendMs;
    promptAt_ = 0;
    silentSince_ = windowGoneSince_ = 0;
}

static uint64_t Since(uint64_t& since, bool cond, uint64_t now) {
    if (!cond) { since = 0; return 0; }
    if (!since) since = now;
    return now - since;
}

CallDetector::Action CallDetector::tick(const Input& in) {
    const uint64_t now = in.nowMs ? in.nowMs : 1;
    if (!in.valid) return Action::None; // couldn't observe; never treat that as "call gone"

    if (!in.recording) {
        micGoneSince_ = silentSince_ = windowGoneSince_ = promptAt_ = 0;
        if (!in.micActive) rearm_ = false;
        else if (!rearm_) { reason_ = L"call app holds mic"; return Action::Start; }
        return Action::None;
    }
    if (!in.autoStarted) return Action::None; // manual recordings are the user's business

    // Hard stop: the app let go of the mic and stayed away.
    if (Since(micGoneSince_, !in.micActive, now) >= t_.hardStopMs) {
        reset();
        reason_ = L"mic session gone";
        rearm_ = false; // mic already gone; next call may start at once
        return Action::Stop;
    }
    if (!in.micActive) return Action::None; // gone briefly; wait, don't judge silence

    // Soft evidence, only meaningful while the app still holds the mic.
    bool quiet = true;
    if (in.recLevel >= 0 && in.recLevel >= t_.silenceLevel) quiet = false;
    if (in.renderPeak >= 0 && in.renderPeak >= t_.silenceLevel) quiet = false;
    if (in.recLevel < 0 && in.renderPeak < 0) quiet = false; // no meter -> can't judge
    bool silentLong = Since(silentSince_, quiet, now) >= t_.silenceMs;
    // Window absence alone is too weak (minimised/undetected windows); it
    // only counts while nobody is talking either.
    bool windowGone = Since(windowGoneSince_, in.meetingWindow == 0 && quiet, now) >= t_.windowGoneMs;

    bool looksOver = silentLong || windowGone;
    bool extended = extendUntil_ && now < extendUntil_;

    if (promptAt_) {
        if (!looksOver) { promptAt_ = 0; reason_ = L"activity resumed"; return Action::CancelPrompt; }
        if (now - promptAt_ >= t_.graceMs) {
            reset();
            reason_ = windowGone ? L"meeting window gone, no reply" : L"long silence, no reply";
            rearm_ = true; // app still holds the mic; don't restart until it lets go
            return Action::Stop;
        }
        return Action::None;
    }
    if (looksOver && !extended) {
        promptAt_ = now;
        reason_ = windowGone ? L"meeting window gone" : L"long silence";
        return Action::PromptEnd;
    }
    return Action::None;
}
