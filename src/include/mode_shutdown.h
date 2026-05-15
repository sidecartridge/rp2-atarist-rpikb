#ifndef MODE_SHUTDOWN_H
#define MODE_SHUTDOWN_H

// Identifies which mode loop is currently active. The active mode loop calls
// mode_shutdown_set_active() once its peripherals are up; jump_to_booster_app()
// then calls mode_shutdown_for_jump() to invoke the matching teardown before
// swapping the VTOR to the booster app. The dispatcher must be called with
// CPU interrupts already masked (save_and_disable_interrupts) so any
// in-flight peripheral IRQ cannot dispatch into stale state mid-teardown.
typedef enum {
  MODE_SHUTDOWN_NONE = 0,
  MODE_SHUTDOWN_NATIVE,
  MODE_SHUTDOWN_USB,
  MODE_SHUTDOWN_BT,
} mode_shutdown_kind_t;

void mode_shutdown_set_active(mode_shutdown_kind_t kind);
void mode_shutdown_for_jump(void);

#endif  // MODE_SHUTDOWN_H
