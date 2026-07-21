#pragma once
#include "common.h"

class Recorder;

// Opens (or brings to front) the debug window: recording status, elapsed
// time, a live level waveform, and the recent log. Safe to call repeatedly.
void DebugWindow_Show(HINSTANCE inst, Recorder* rec);
