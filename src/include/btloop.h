#ifndef BTLOOP_H
#define BTLOOP_H

#include <stdint.h>

int main_bt_bluepad32(int prev_reset_state, int prev_config_state,
                      void (*handle_rx)(void),
                      void (*reset_sequence_cb)(void));

// Teardown for the BT mode loop, called from jump_to_booster_app() via
// mode_shutdown_for_jump() when the device is about to swap the VTOR to the
// booster app. MUST be called with CPU interrupts already masked
// (save_and_disable_interrupts) — cyw43_arch_deinit() nulls the BTstack run
// loop mid-call, and any CYW43 IRQ that races that step asserts in
// btstack_run_loop_poll_data_sources_from_irq.
void btloop_shutdown_for_jump(void);

#endif  // BTLOOP_H
