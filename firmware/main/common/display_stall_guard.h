#pragma once

#include <cstdint>

class CustomLcdDisplay;

enum class RuntimeGuardPhase : uint32_t {
    Boot = 0,
    Initializing,
    Running,
    PreparingSleep,
    CommittingSleep,
};

// Start an independent liveness task.  It intentionally starts before board
// initialization and never takes application, UI, or display locks.
void StartDisplayStallGuard();

// Main-task progress and state published to the independent guard.
void RuntimeGuardNoteProgress(RuntimeGuardPhase phase);
void RuntimeGuardSetScheduledWake(bool scheduled_wake);
void RuntimeGuardRegisterDisplay(CustomLcdDisplay* display);

// A guard recovery uses esp_restart(). main.cc consumes its RTC-retained record
// so the recovery boot is not bounced through the generic software-reset path.
bool ConsumeDisplayStallRecoveryBoot();
