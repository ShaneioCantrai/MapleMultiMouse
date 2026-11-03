# MapleMultiMouse: OBS operator mode (experimental)

This MIT-licensed fork of [openMouse](https://github.com/alstonmendonca/openMouse)
adds a **focus-preserving, dedicated OBS operator mouse**. The feature is
opt-in and defaults to disabled.

## What is implemented

* Seat 0 keeps its normal Windows cursor, game controls and foreground focus.
* When reserved, seat 1 moves its independent overlay cursor, but **none of its
  clicks are injected into Windows**, so they cannot activate the game or OBS.
* A small click-through, non-activating HUD floats on the secondary display
  (or primary if only one). Seat 1 can click Scene A, Scene B, Toggle microphone.
* A local Node.js bridge forwards the three commands to OBS WebSocket v5.
  Its network work never runs on the Windows raw-input thread.
* The original emergency key Ctrl+Alt+Shift+Q and janitor are preserved.

This is an OBS-specific operator surface, **not** fully independent Windows
application focus. The original developer documented why general independent
physical keyboards cannot be safely implemented with only user-mode hooks.
Keyboard steering is forcibly disabled in OBS mode, and any second physical
keyboard still types into whichever Windows app has focus. Do not type with it
while someone is gaming. No promise of arbitrary OBS GUI editing.

## Setup, offline first

1. On the OBS computer, enable OBS Studio **Tools > WebSocket Server Settings**.
   The default port is 4455. Enable authentication; obtain its password.
2. Install Node.js 22+ if not present. Under `obs-bridge`, copy
   `.env.example` to `.env`. Fill in password, scene A/B names and the audio
   input name exactly as they appear in OBS.
3. In PowerShell start the local bridge:

   ```powershell
   cd D:\Dev\MapleMultiMouse\obs-bridge
   node --env-file=.env server.js
   ```

   It listens **only on 127.0.0.1:4456**. No firewall port opening.
   It will report connection status; the HUD says OBS connected once ready.
4. Build from repository root: `build.cmd MapleMultiMouse-test.exe`.
5. Run the upstream viability tests `MapleMultiMouse-test.exe --probe` and
   `MapleMultiMouse-test.exe --probe-echo`, then `--identify` to bind
   the two physical mice accurately.
6. Edit `%APPDATA%\openMouse\openmouse.ini`:

   ```ini
   [general]
   seats=2
   capture_keyboards=0

   [obs]
   operator_seat=1
   bridge_port=4456
   ```

   Preserve the `[mouse]` paths written by `--identify`. Seat 0 is normal
   and seat 1 becomes OBS-only.
7. Test with a 60-second automatic exit:
   `MapleMultiMouse-test.exe --seconds=60`. The second cursor clicks the
   on-screen buttons without selecting or focusing the underlying windows.

**Emergency release:** Ctrl+Alt+Shift+Q on either keyboard. If a cursor stays
hidden, use `MapleMultiMouse-test.exe --restore-cursor`.

## Security and limitations

The bridge is loopback-only, exposes three allowlisted actions, accepts only
JSON POSTs and rejects Origin-bearing requests to reduce localhost CSRF risk.
The OBS password is read from the process environment, **never from source
control**. Do not publish `obs-bridge/.env`.

The HUD may not display over exclusive fullscreen games. Borderless windowed
mode is recommended. Any change of OBS scenes or mic state must be tested with
a dummy OBS profile before a live stream. No software has been activated on
MOMMAPIE as part of this development build.

## Tests

```powershell
node --test obs-bridge\test.js
build.cmd MapleMultiMouse-test.exe
```

## Outstanding work

Real two-mouse acceptance on MOMMAPIE, native automatic scene labels and
volume faders, per-display HUD placement controls, and separate authenticated
remote operator keyboard input. Fully independent physical keyboard routing
would require separate OS-level isolation and remains out of scope for v0.1.
