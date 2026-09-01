#pragma once

// Start the display-stall fail-safe after Application::Initialize() has
// completed and the board/display objects are ready.
void StartDisplayStallGuard();

// A stall recovery uses esp_restart(). main.cc consumes this RTC-retained flag
// so that recovery boot is not bounced through the normal software-reset
// deep-sleep workaround and accidentally reclassified as a scheduler timer wake.
bool ConsumeDisplayStallRecoveryBoot();
