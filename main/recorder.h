#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Start the recorder's tasks. The card is not touched until the first
/// recording, so this costs nothing at boot.
void recorder_init(void);

/// Start a recording, or finish the one in progress. Non-blocking: mounting
/// the card, opening the file and closing it again all happen on the
/// recorder's own tasks, so the state callback is what says whether
/// anything actually happened.
void recorder_toggle(void);

/// True from the click that starts a recording until its file is closed.
bool recorder_is_active(void);

/// Called when a recording really starts, and again once the file has been
/// closed. Runs from the recorder task, not the caller's.
void recorder_set_state_cb(void (*cb)(bool recording));

/// Finish any recording in progress and wait for its file to be closed, up
/// to `timeout_ms`. For the paths that are about to cut the power.
void recorder_flush(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
