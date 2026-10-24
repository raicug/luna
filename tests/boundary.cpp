// Compiles each public header in isolation and asserts the boundary rules that
// matter: a consumer only needs luna.hpp, and no luna type mentions Luau.

#include <luna/fwd.hpp>

#include <luna/value.hpp>
#include <luna/convert.hpp>
#include <luna/bound.hpp>
#include <luna/state.hpp>
#include <luna/luna.hpp>

#include <cstdio>
#include <type_traits>

// A consumer type has to live at namespace scope for its trait opt-in to be
// legal, which is the rule luna documents.
struct Widget {
  double Size = 1.0;
};

// The opt-in a consumer writes for its own class.
template <> struct luna::detail::RegisteredClassTrait<Widget> : std::true_type {};

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
  // The public types must be usable without any Luau declaration in scope.
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
