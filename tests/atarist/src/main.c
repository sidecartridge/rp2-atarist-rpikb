#include <stdio.h>
#include <sys/types.h>

#include "test_runner.h"
#include "timing_tests.h"
//================================================================
// Main program
static int run() {
#ifdef _LOG
  open_log();
#endif

  print("Atari ST IKBD Test Suite\r\n");
  print("Running timing tests...\r\n\r\n");

  run_timing_tests(FALSE);

#ifdef _LOG
  close_log();
#endif

  // press_key("All tests completed.\r\n");
  print("All tests completed.\r\n");
}

//================================================================
// Standard C entry point
int main(int argc, char *argv[]) {
  // switching to supervisor mode and execute run()
  // needed because of direct memory access for reading/writing the palette
  Supexec(&run);

  Pterm(0);
  return EXIT_SUCCESS;
}
//================================================================
