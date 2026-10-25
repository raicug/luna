#include "harness.hpp"

#include <cstdio>

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  demo::Harness Harness;
  return Harness.RunSelfTest() == 0 ? 0 : 1;
}
