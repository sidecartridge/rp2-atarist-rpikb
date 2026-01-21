#ifndef NATIVELOOP_H
#define NATIVELOOP_H

void enter_configuration_mode(void);
void run_native_keyboard_mode(void (*reset_sequence_cb)(void));

#endif  // NATIVELOOP_H
