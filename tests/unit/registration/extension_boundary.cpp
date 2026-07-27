// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/binding/overload.hpp>
#include <luna/binding/supported_callable.hpp>
#include <luna/core/diagnostics/error_category.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>

#include "state/testing/test_hooks.hpp"

#include <coroutine>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "extension boundary check failed: " << Description << '\n';
}

// One representative signal, event, and delegate surface. Luna owns no
// canonical descriptor for any of them in this milestone.
class SignalHub final {
public:
  void Connect(std::function<void(int)> Listener) {
    Listeners.push_back(std::move(Listener));
  }

  void Emit(int Value) const {
    for (const auto &Listener : Listeners)
      Listener(Value);
  }

private:
  std::vector<std::function<void(int)>> Listeners;
};

using DelegateSlot = std::function<void(int)>;

// A generic template callable stands in for template auto-binding.
const auto GenericScale = [](auto Value) { return Value; };

[[nodiscard]] int Scale(int Value) {
  return Value * 2;
}

[[nodiscard]] std::string PathKind(Luna::State &Owner,
                                   const std::string &Path) {
  const auto Kind = Hooks::ObserveVmPathValueKind(Owner, Path);
  return Kind ? *Kind : std::string("<unavailable>");
}

[[nodiscard]] int StackDepth(const Luna::State &Owner) {
  const auto Depth = Hooks::ObserveRootStackDepth(Owner);
  return Depth ? *Depth : -1;
}

[[nodiscard]] bool
RefusedWithoutCanonicalType(const Luna::RegistrationResult &Result,
                            std::string_view Subject) {
  if (Result.IsSuccess() || !Result.Diagnostic())
    return false;
  if (Result.Diagnostic()->Category() != Luna::ErrorCategory::Internal)
    return false;
  const std::string &Message = Result.Diagnostic()->Message();
  return Message.find(Subject) != std::string::npos &&
         Message.find("no canonical Luna type") != std::string::npos;
}

[[nodiscard]] Luna::ModuleManifest Manifest(std::string Identity,
                                            std::string_view VersionText) {
  const auto Parsed = Luna::SemanticVersion::TryParse(VersionText);
  auto Created = Luna::ModuleManifest::TryCreate(
      std::move(Identity), Parsed ? *Parsed : Luna::SemanticVersion(), {},
      std::string(), {});
  return Created ? std::move(*Created) : Luna::ModuleManifest();
}

void CheckTypedConstraintsRefuseUnavailableCallables() {
  // Template auto-binding: an unconstrained generic callable declares no
  // signature, so no descriptor can be derived from it.
  Check(!Luna::SupportedCallableTrait<decltype(GenericScale)>::value,
        "a generic template callable is refused by the public constraint");
  Check(Luna::SupportedCallableTrait<decltype(Luna::Overload<int(int)>(
            GenericScale))>::value,
        "an explicit concrete overload selection stays the supported opt-in");

  // Coroutines and asynchronous tasks.
  Check(!Luna::SupportedReturn<std::coroutine_handle<>>,
        "a coroutine handle is no supported return type");
  Check(!Luna::SupportedReturn<std::future<int>>,
        "an asynchronous task is no supported return type");
  Check(!Luna::SupportedCallableTrait<std::coroutine_handle<> (*)()>::value,
        "a coroutine-returning callable is refused at compile time");
  Check(!Luna::SupportedCallableTrait<std::future<int> (*)()>::value,
        "an asynchronous callable is refused at compile time");
  Check(
      !Luna::SupportedCallableTrait<decltype(Luna::Overload<std::future<int>()>(
          std::declval<std::future<int> (*)()>()))>::value,
      "explicit selection cannot smuggle an asynchronous form past the "
      "constraint");

  // Delegates, signals, and events.
  Check(!Luna::SupportedParameter<DelegateSlot>,
        "a delegate is no supported parameter type");
  Check(!Luna::SupportedValue<SignalHub>,
        "a signal or event hub is no supported value type");
  Check(!Luna::SupportedCallableTrait<void (*)(DelegateSlot)>::value,
        "subscribing a delegate is refused at compile time");
  Check(!Luna::SupportedCallableTrait<void (*)(SignalHub &)>::value,
        "publishing through a signal is refused at compile time");
  Check(!Luna::SupportedCallableTrait<SignalHub (*)()>::value,
        "handing out an event source is refused at compile time");

  // The ordinary supported forms stay available, so the constraint refuses
  // the unavailable extensions rather than everything.
  Check(Luna::SupportedCallableTrait<int (*)(int)>::value,
        "an ordinary callable remains supported");
  Check(Luna::SupportedValue<int> && Luna::SupportedReturn<void>,
        "the canonical value and return domains remain unchanged");
}

void CheckUnavailableValuesAreRejectedAtRegistration() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Owner.IsReady(), "the boundary State is ready");

  const int EntryDepth = StackDepth(Owner);
  const std::uint64_t EntryGeneration = Hooks::ReflectionGeneration(Owner);

  DelegateSlot Slot = [](int) {};
  const auto Delegate = Registry.RegisterConstant("Delegate", Slot);
  const auto Signal = Registry.RegisterConstant("Signal", SignalHub());
  const auto Awaited =
      Registry.RegisterConstant("Awaited", std::coroutine_handle<>());

  Check(RefusedWithoutCanonicalType(Delegate, "Delegate"),
        "a delegate value is refused deterministically at registration time");
  Check(RefusedWithoutCanonicalType(Signal, "Signal"),
        "a signal or event value is refused deterministically");
  Check(RefusedWithoutCanonicalType(Awaited, "Awaited"),
        "a coroutine handle is refused deterministically");

  Check(PathKind(Owner, "Delegate") == "absent" &&
            PathKind(Owner, "Signal") == "absent" &&
            PathKind(Owner, "Awaited") == "absent",
        "a refused extension installs no virtual-machine artifact");
  Check(Registry.Reflection().IsEmpty(),
        "a refused extension publishes no descriptor and is never advertised");
  Check(Hooks::ReflectionGeneration(Owner) == EntryGeneration,
        "a refused extension advances no reflection generation");
  Check(Hooks::BindingCount(Owner) == 0 &&
            Hooks::PendingBindingCount(Owner) == 0,
        "a refused extension leaves no committed or pending binding");
  Check(!Hooks::HasActiveTransaction(Owner),
        "a refused extension leaves no transaction open");
  Check(StackDepth(Owner) == EntryDepth,
        "a refused extension restores the exact entry stack depth");

  // Deterministic diagnostics: the same refusal repeats identically.
  const auto Repeated = Registry.RegisterConstant("Signal", SignalHub());
  Check(Repeated.Diagnostic() && Signal.Diagnostic() &&
            Repeated.Diagnostic()->Message() == Signal.Diagnostic()->Message(),
        "repeating a refused extension reports the same diagnostic");

  // State recovery: the ordinary surface still registers and runs.
  Check(Registry.RegisterConstant("Ready", true).IsSuccess() &&
            Registry.RegisterFunction("Scale", &Scale).IsSuccess(),
        "the supported surface registers after every refusal");
  Check(Owner.Execute("assert(Ready and Scale(21) == 42)").IsSuccess(),
        "the State remains reusable after every refusal");
  Check(!Registry.Reflection().Find("Signal").IsValid() &&
            Registry.Reflection().Find("Scale").IsValid(),
        "reflection names only the published symbols");
}

void CheckUnavailableValuesInScopesPublishNothing() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);

  Luna::NamespaceBuilder Scope = Registry.RegisterNamespace("Events");
  Scope.RegisterConstant("Ready", true);
  Scope.RegisterConstant("Hub", SignalHub());
  const auto Result = Scope.Commit();

  Check(RefusedWithoutCanonicalType(Result, "Events.Hub"),
        "an unavailable scoped member refuses the whole plan");
  Check(PathKind(Owner, "Events") == "absent",
        "a refused plan creates no namespace table");
  Check(Hooks::NamespaceOwnershipCount(Owner) == 0,
        "a refused plan claims no namespace ownership");
  Check(Registry.Reflection().IsEmpty(),
        "a refused plan publishes no partial descriptor");
  Check(!Hooks::HasActiveTransaction(Owner) && StackDepth(Owner) == EntryDepth,
        "a refused plan closes its transaction and restores the stack");

  Luna::NamespaceBuilder Retried = Registry.RegisterNamespace("Events");
  Retried.RegisterConstant("Ready", true);
  Check(Retried.Commit().IsSuccess(),
        "the same scope registers once the unavailable member is dropped");
  Check(PathKind(Owner, "Events") == "table" &&
            Owner.Execute("assert(Events.Ready)").IsSuccess(),
        "the retried scope publishes exactly its supported members");
}

void CheckUnavailableValuesInModuleLoadsPublishNothing() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);

  const Luna::ModuleManifest Tasks = Manifest("studio.tasks", "1.0.0");
  Check(Tasks.IsValid(), "the boundary manifest is valid");

  const auto Result =
      Registry.RegisterModule(Tasks, [](Luna::NamespaceBuilder &Builder) {
        Luna::NamespaceBuilder Scope = Builder.RegisterNamespace("Tasks");
        Scope.RegisterConstant("Pending", 1);
        Scope.RegisterConstant("Awaited", std::coroutine_handle<>());
      });

  Check(RefusedWithoutCanonicalType(Result, "Tasks.Awaited"),
        "an unavailable member refuses the whole module load");
  Check(Hooks::LoadedModuleCount(Owner) == 0,
        "a refused load leaves no module loaded");
  Check(PathKind(Owner, "Tasks") == "absent" && Registry.Reflection().IsEmpty(),
        "a refused load publishes no namespace, export, or descriptor");
  Check(!Hooks::HasActiveTransaction(Owner) && StackDepth(Owner) == EntryDepth,
        "a refused load closes its transaction and restores the stack");

  Check(Registry.RegisterFunction("Scale", &Scale).IsSuccess() &&
            Owner.Execute("assert(Scale(4) == 8)").IsSuccess(),
        "the State remains reusable after a refused load");
}

} // namespace

int RunUnavailableExtensionBoundaryTests() {
  FailureCount = 0;
  CheckTypedConstraintsRefuseUnavailableCallables();
  CheckUnavailableValuesAreRejectedAtRegistration();
  CheckUnavailableValuesInScopesPublishNothing();
  CheckUnavailableValuesInModuleLoadsPublishNothing();
  return FailureCount == 0 ? 0 : 1;
}
