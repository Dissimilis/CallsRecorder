# CallsRecorder

A tiny Windows tray app that records your calls to MP3.

I wanted something dead simple for recording Meet/Teams/Zoom calls: an app that sits in the tray, grabs both my mic and the other side, and writes an MP3 I can find later. No accounts, no cloud, no Electron. This is that app. One static exe, about 440 KB, no installer, nothing to configure.

## Usage

Run `CallsRecorder.exe`. A small ring icon appears in the tray.

The exe is not code-signed, so Windows SmartScreen will complain the first time you run a downloaded copy. Click "More info", then "Run anyway", or build it from source yourself (see below).

Right-click it for the menu (start/stop, open recordings folder, change folder, exit), or just double-click the icon to toggle recording. While recording, the icon turns into a red dot and the tooltip shows elapsed time.

Recordings go to your Downloads folder by default, named like `2026-07-21 14-32-05.mp3`. You can pick a different folder from the menu and it will be remembered.

## The interesting part: device selection

The annoying thing about recording calls is that the call rarely uses the device you'd guess. You have a laptop mic, a webcam mic, a Bluetooth headset, and the call is on whichever one Zoom happened to pick.

CallsRecorder doesn't make you choose. It looks at which process currently has an active audio session on a microphone. If that process is a known call app (Teams, Zoom, Discord, Slack, Webex, or a browser, which covers Meet), it records that mic, and the speaker that same app is playing through. If no call is detected it falls back to the Windows default communication devices.

It re-checks every couple of seconds while recording, so if you plug in a headset mid-call or switch devices in the app's settings, the recording follows along. The MP3 stays intact; you just get a sub-second gap at the switch point.

The known app list is a plain array in `src/DeviceTracker.cpp` if yours is missing.

## Recording details

Both streams are mixed into a single 128 kbps, 48 kHz stereo MP3, encoded by the MP3 encoder that ships with Windows (Media Foundation), so there are no codec DLLs to bundle. The file is written progressively during the call. If the app or the machine dies mid-recording, the file on disk is still playable up to the last few seconds.

Since the speaker side is captured with device loopback, anything else playing through that device during a recording ends up in the file too. Mute your notification sounds if that bothers you.

Settings and a small diagnostic log live in `%APPDATA%\CallsRecorder\`.

## Building

You need MinGW-w64 (g++ and windres on PATH). Then:

```powershell
.\build.ps1
```

That's the whole build. It produces a fully static exe with no runtime dependencies. Audio is WASAPI, encoding is Media Foundation, both part of Windows. MSVC should work too with adjusted flags, but I haven't set that up.

## Source layout

| File | What it does |
|---|---|
| `src/main.cpp` | Tray icon, menu, message loop |
| `src/Recorder.*` | Recording thread: pick devices, capture, mix, write |
| `src/DeviceTracker.*` | Figures out which mic/speaker the call is using |
| `src/AudioCapture.*` | WASAPI capture (mic or loopback), converted to 48 kHz stereo |
| `src/Mp3Writer.*` | Progressive MP3 via Media Foundation sink writer |
| `src/Settings.*` | Remembers the storage folder |

Two hidden flags help with debugging: `--record N` records N seconds without the tray UI and exits, `--devices` logs which devices would be picked right now. Both log to `%APPDATA%\CallsRecorder\CallsRecorder.log`.

## A word on legality

Laws on recording calls vary a lot between countries (and US states). In many places you must tell the other side you're recording. That's on you, not the app.

## License

MIT, see [LICENSE](LICENSE).
