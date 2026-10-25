#include <luna/fwd.hpp>

#include <luna/value.hpp>
#include <luna/convert.hpp>
#include <luna/bound.hpp>
#include <luna/state.hpp>
#include <luna/signal.hpp>
#include <luna/luna.hpp>

#include <cstdio>
#include <type_traits>

struct Widget {
  double Size = 1.0;
};

template <>
struct luna::detail::RegisteredClassTrait<Widget> : std::true_type {};

namespace {
int Failures = 0;

void Check(bool Condition, const char *What) {
  if (!Condition) {
    std::printf("  FAIL  %s\n", What);
    ++Failures;
  }
}
} // namespace

int main() {
  static_assert(std::is_move_constructible_v<luna::State>);
  static_assert(!std::is_copy_constructible_v<luna::State>);
  static_assert(std::is_default_constructible_v<luna::OwnedValue>);
  static_assert(std::is_default_constructible_v<luna::ValuePack>);
  static_assert(std::is_default_constructible_v<luna::ArgumentPack>);
  luna::State State;
  Check(State.IsReady(), "a state is ready");
  luna::OwnedValue Table = luna::OwnedValue::Table();
  Table.SetField("Name", luna::OwnedValue::Text("hi"));
  Table.Append(luna::OwnedValue::Number(2.0));
  Check(Table.IsTable(), "a table is a table");
  luna::ValuePack Pack;
  Pack.Append(luna::OwnedValue::Number(1.0)).Append(luna::OwnedValue::Nil());
  Check(Pack.Size() == 2, "a pack holds its values");
  luna::LifetimeHandle Lifetime;
  Check(Lifetime.Alive(), "a lifetime starts alive");
  Lifetime.Invalidate();
  Check(!Lifetime.Alive(), "a lifetime can be invalidated");
  luna::BindingRegistry Registry = State.Bindings();
  Check(Registry.Owner() != nullptr, "a registry is attached");
  const luna::SymbolRange Symbols = State.Symbols();
  (void)Symbols.Size();
  std::printf("%s\n", Failures == 0 ? "boundary PASS" : "boundary FAIL");
  return Failures == 0 ? 0 : 1;
}
