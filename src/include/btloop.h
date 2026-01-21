#ifndef BTLOOP_H
#define BTLOOP_H

#include <stdint.h>

int main_bt_bluepad32(int prev_reset_state, int prev_config_state,
                      void (*handle_rx)(void),
                      void (*reset_sequence_cb)(void));

#endif  // BTLOOP_H
