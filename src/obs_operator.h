// MapleMultiMouse — opt-in, focus-preserving OBS operator seat
// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>

namespace om {

// A seat reserved for OBS actions never injects mouse buttons or claims
// Windows foreground focus. Its pointer remains an openMouse overlay.
bool ObsOperatorStart(HINSTANCE instance, unsigned short bridgePort);
void ObsOperatorStop();
void ObsOperatorMouse(POINT screenPoint, USHORT rawButtonFlags);

} // namespace om
