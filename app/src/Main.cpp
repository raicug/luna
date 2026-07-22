// clang-format off
#include <Luna/Runtime.hpp>

#include <cstdlib>
#include <iostream>
// clang-format on

int main() {
  const Luna::Runtime Runtime;

  if (!Runtime.IsReady())
    return EXIT_FAILURE;

  std::cout << "LUNA runtime ready\n";
  return EXIT_SUCCESS;
}
