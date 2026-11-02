#include <luna/luna.hpp>

int main() {
  luna::State state;
  if (!state.IsReady())
    return 1;
  if (!state.Bindings()
           .Register("Add", [](double a, double b) { return a + b; })
           .IsSuccess())
    return 1;
  return state.Execute("assert(Add(20, 22) == 42)").IsSuccess() ? 0 : 1;
}
