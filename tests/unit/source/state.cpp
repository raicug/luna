// clang-format off
#include <luna/luna.hpp>

#include <string>
#include <type_traits>
#include <utility>
// clang-format on

namespace {

template <class Callable>
concept CanRegister =
    requires(Luna::BindingRegistry &Registry, Callable &&Target) {
      Registry.Register("Callable", std::forward<Callable>(Target));
    };

[[nodiscard]] bool HasCategory(const Luna::RegistrationResult &Result,
                               Luna::ErrorCategory Category) {
  return !Result.IsSuccess() && Result.Diagnostic() != nullptr &&
         Result.Diagnostic()->Category() == Category &&
         !Result.Diagnostic()->Message().empty();
}

[[nodiscard]] bool HasCategory(const Luna::ExecutionResult &Result,
                               Luna::ErrorCategory Category) {
  return !Result.IsSuccess() && Result.Diagnostic() != nullptr &&
         Result.Diagnostic()->Category() == Category &&
         !Result.Diagnostic()->Message().empty();
}

} // namespace

static_assert(!std::is_copy_constructible_v<Luna::State>);
static_assert(!std::is_copy_assignable_v<Luna::State>);
static_assert(std::is_nothrow_move_constructible_v<Luna::State>);
static_assert(std::is_nothrow_move_assignable_v<Luna::State>);
static_assert(!std::is_default_constructible_v<Luna::BindingRegistry>);
static_assert(CanRegister<int (*)(int)>);

int RunStateFacadeTests() {
  const auto Generic = [](auto Value) { return Value; };
  static_assert(!CanRegister<decltype(Generic)>);

  Luna::State Source;
  if (!Source.IsReady())
    return 1;

  auto SourceRegistry = Source.Bindings();
  Luna::State Destination(std::move(Source));
  if (Source.IsReady() || !Destination.IsReady())
    return 2;

  const auto SourceRegistration =
      SourceRegistry.Register("MovedSource", [](int Value) { return Value; });
  if (!HasCategory(SourceRegistration, Luna::ErrorCategory::StateNotReady))
    return 3;

  auto DestinationRegistry = Destination.Bindings();
  const auto DestinationRegistration = DestinationRegistry.Register(
      "Destination", [Prefix = std::string("value:")](int Value) {
        return Prefix + std::to_string(Value);
      });
  if (!DestinationRegistration.IsSuccess())
    return 4;

  const auto ReadyExecution = Destination.Execute("return 1");
  if (!ReadyExecution.IsSuccess() || ReadyExecution.Diagnostic())
    return 5;

  Luna::State Replacement;
  Replacement = std::move(Destination);
  if (Destination.IsReady() || !Replacement.IsReady())
    return 6;

  const auto MovedExecution = Destination.Execute("return 1");
  if (!HasCategory(MovedExecution, Luna::ErrorCategory::StateNotReady))
    return 7;

  const auto OldDestinationRegistration =
      DestinationRegistry.Register("OldDestination", [] {});
  if (!HasCategory(OldDestinationRegistration,
                   Luna::ErrorCategory::StateNotReady))
    return 8;

  const auto ReplacementRegistration =
      Replacement.Bindings().Register("Replacement", [] {});
  if (!ReplacementRegistration.IsSuccess())
    return 9;

  return 0;
}
