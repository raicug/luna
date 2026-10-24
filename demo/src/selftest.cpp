// Headless entry point for the demo's self-test.
//
// Same harness as the windowed build, no window and no graphics device, so the
// demo's behaviour can be checked from a terminal or from ctest.

#include "harness.hpp"

#include <cstdio>

int main() {
  // Unbuffered: if a check hangs, the last line printed is the evidence.
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  demo::Harness Harness;
  return Harness.RunSelfTest() == 0 ? 0 : 1;
}
