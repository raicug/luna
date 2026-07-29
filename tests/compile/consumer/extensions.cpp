// clang-format off
#include <luna/luna.hpp>

#include <coroutine>
#include <functional>
#include <future>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
// clang-format on

#if defined(LUNA_REGISTER) || defined(LUNA_BIND) || defined(LUNA_FUNCTION) ||  \
    defined(LUNA_CLASS) || defined(LUNA_ENUM) || defined(LUNA_MODULE) ||       \
    defined(LUNA_PROPERTY) || defined(LUNA_ANNOTATE) ||                        \
    defined(LUNA_REFLECT) || defined(LUNA_COROUTINE) || defined(LUNA_ASYNC) || \
    defined(LUNA_SIGNAL) || defined(LUNA_EVENT) || defined(LUNA_PROFILE)
#error "Luna's public headers must define no registration or annotation macro."
#endif

namespace {

// A native event source built from the supported delegate surface. Nothing
// here needs a macro, a Luau declaration, or a parallel callback system.
class SignalHub final {
public:
  [[nodiscard]] int Connect(Luna::Delegate<void(int)> Listener) {
    return Damage.Subscribe(std::move(Listener));
  }

  [[nodiscard]] bool Disconnect(int Token) { return Damage.Unsubscribe(Token); }

  [[nodiscard]] int Publish(int Amount) {
    return static_cast<int>(Damage.Emit(Amount).Delivered);
  }

private:
  Luna::Signal<void(int)> Damage;
};

using DelegateSlot = std::function<void(int)>;

SignalHub ConsumerHub;

[[nodiscard]] int ConsumerConnect(Luna::Delegate<void(int)> Listener) {
  return ConsumerHub.Connect(std::move(Listener));
}

[[nodiscard]] int ConsumerConnectSlot(DelegateSlot Listener) {
  static_cast<void>(Listener);
  return 0;
}

[[nodiscard]] int ConsumerPublish(int Amount) {
  return ConsumerHub.Publish(Amount);
}

const auto GenericScale = [](auto Value) { return Value; };

[[nodiscard]] int ConsumerScale(int Value) {
  return Value * 2;
}

// One asynchronous form: the callable starts the work, hands Luna the task,
// and the host settles it later without touching Luna's internals.
[[nodiscard]] Luna::AsyncTask<int> ConsumerScaleLater(int Value) {
  Luna::AsyncCompletionSource<int> Source;
  Luna::AsyncTask<int> Pending = Source.Task();
  static_cast<void>(Source.Complete(Value * 2));
  return Pending;
}

// Template auto-binding stays unavailable: an unconstrained generic callable
// declares no signature, so no canonical descriptor exists for it. Explicit
// concrete selection remains the supported opt-in.
static_assert(!Luna::SupportedCallable<decltype(GenericScale)>,
              "A generic template callable must be refused at compile time.");
static_assert(
    Luna::SupportedCallable<decltype(Luna::Overload<int(int)>(GenericScale))>,
    "An explicit concrete overload selection must remain supported.");

// Coroutine and asynchronous invocation is available: work that finishes
// after the call that started it declares a supported return type, and the
// ordinary registration path accepts it.
static_assert(Luna::SupportedReturn<std::future<int>> &&
                  Luna::SupportedReturn<Luna::AsyncTask<int>> &&
                  Luna::SupportedReturn<Luna::AsyncTask<void>> &&
                  Luna::SupportedReturn<Luna::AsyncTask<Luna::ReturnPack>>,
              "Suspended work must declare a supported return type.");
static_assert(
    Luna::SupportedCallable<std::future<int> (*)()> &&
        Luna::SupportedCallable<Luna::AsyncTask<std::string> (*)(int)>,
    "Coroutine and asynchronous callables must be accepted at "
    "compile time.");
static_assert(
    Luna::SupportedCallable<decltype(Luna::Overload<std::future<int>()>(
        std::declval<std::future<int> (*)()>()))>,
    "Explicit selection must carry an asynchronous form through the same "
    "public constraint.");

// The asynchronous domain stays exactly as wide as its awaited values. A raw
// coroutine handle names no result, so it remains refused, and an awaited
// value outside the canonical domain stays refused too.
static_assert(!Luna::SupportedReturn<std::coroutine_handle<>> &&
                  !Luna::SupportedReturn<std::future<SignalHub>> &&
                  !Luna::SupportedReturn<Luna::AsyncTask<unsigned int>>,
              "Suspended work must still declare a canonical awaited value.");

// Delegates, signals, and events are available: a subscribed handler is one
// canonical delegate parameter, so subscribing and emitting are ordinary
// callables. An event source itself stays native, never a bound value.
static_assert(
    Luna::SupportedParameter<DelegateSlot> &&
        Luna::SupportedParameter<Luna::Delegate<void(int)>> &&
        Luna::SupportedParameter<const Luna::Delegate<bool(std::string)> &>,
    "A delegate parameter must accept one subscribed handler.");
static_assert(Luna::SupportedDelegate<Luna::Delegate<void(int)>> &&
                  Luna::SupportedDelegate<DelegateSlot> &&
                  !Luna::SupportedDelegate<int>,
              "The delegate domain must name exactly the handler forms.");
static_assert(Luna::SupportedCallable<void (*)(DelegateSlot)> &&
                  Luna::SupportedCallable<int (*)(Luna::Delegate<void(int)>)> &&
                  Luna::SupportedCallable<decltype(&ConsumerPublish)>,
              "Subscribing and emitting must be accepted at compile time.");
static_assert(!Luna::SupportedValue<SignalHub> &&
                  !Luna::SupportedCallable<void (*)(SignalHub &)> &&
                  !Luna::SupportedCallable<SignalHub (*)()>,
              "An event source stays native; only its subscribe and emit "
              "callables cross the boundary.");
static_assert(!Luna::SupportedParameter<Luna::Delegate<void(unsigned int)>> &&
                  !Luna::SupportedParameter<std::function<SignalHub(int)>>,
              "A delegate outside the canonical value domain must be refused "
              "at compile time.");

// The supported surface stays available, so the constraints refuse the
// unavailable extensions rather than everything.
static_assert(Luna::SupportedCallable<int (*)(int)> &&
                  Luna::SupportedValue<int> && Luna::SupportedReturn<void>,
              "Ordinary callables, values, and returns must stay supported.");

template <typename T, typename = void>
struct ExposesCoroutine : std::false_type {};
template <typename T>
struct ExposesCoroutine<T,
                        std::void_t<decltype(std::declval<T &>().Coroutine())>>
    : std::true_type {};

template <typename T, typename = void> struct ExposesAsync : std::false_type {};
template <typename T>
struct ExposesAsync<T, std::void_t<decltype(std::declval<T &>().Async())>>
    : std::true_type {};

template <typename T, typename = void> struct ExposesAwait : std::false_type {};
template <typename T>
struct ExposesAwait<T, std::void_t<decltype(std::declval<T &>().Await())>>
    : std::true_type {};

template <typename T, typename = void>
struct ExposesSubscribe : std::false_type {};
template <typename T>
struct ExposesSubscribe<T,
                        std::void_t<decltype(std::declval<T &>().Subscribe())>>
    : std::true_type {};

template <typename T, typename = void> struct ExposesEmit : std::false_type {};
template <typename T>
struct ExposesEmit<T, std::void_t<decltype(std::declval<T &>().Emit())>>
    : std::true_type {};

template <typename T, typename = void>
struct ExposesAnnotations : std::false_type {};
template <typename T>
struct ExposesAnnotations<T,
                          std::void_t<decltype(std::declval<T &>().Annotate())>>
    : std::true_type {};

template <template <typename, typename> class Detector>
[[nodiscard]] constexpr bool AdvertisedAnywhere() noexcept {
  return Detector<Luna::State, void>::value ||
         Detector<Luna::BindingRegistry, void>::value ||
         Detector<Luna::NamespaceBuilder, void>::value ||
         Detector<Luna::ReflectionSnapshot, void>::value;
}

// Asynchronous delivery reuses the ordinary registration, reflection, and
// dispatch surface, so no parallel coroutine or awaiting API exists.
static_assert(!AdvertisedAnywhere<ExposesCoroutine>() &&
                  !AdvertisedAnywhere<ExposesAsync>() &&
                  !AdvertisedAnywhere<ExposesAwait>(),
              "Asynchronous invocation must not add a parallel public API.");
// Subscribing and emitting reuse the ordinary registration, reflection, and
// dispatch surface, so Luna adds no parallel event API of its own.
static_assert(!AdvertisedAnywhere<ExposesSubscribe>() &&
                  !AdvertisedAnywhere<ExposesEmit>(),
              "Delegates, signals, and events must stay ordinary reflected "
              "callables rather than a parallel public API.");
static_assert(!AdvertisedAnywhere<ExposesAnnotations>(),
              "No public declaration may advertise annotation helpers.");

// IDE, autocomplete, debug-UI, and profiling consumers are available. They
// consume only public snapshots, generated artifacts, canonical SymbolId
// and TypeId values, and the profiling hook, without changing invocation
// semantics or introducing a second metadata schema.
static_assert(std::is_default_constructible_v<Luna::ProfilingEvent> &&
                  std::is_copy_constructible_v<Luna::ProfilingEvent>,
              "A profiling event is an ordinary owning consumer value.");
static_assert(
    std::is_same_v<decltype(Luna::ProfilingEvent::Symbol), Luna::SymbolId> &&
        std::is_same_v<decltype(Luna::ProfilingEvent::ReceiverType),
                       Luna::TypeId>,
    "Profiling identity is the same canonical SymbolId and TypeId reflection "
    "uses.");
static_assert(std::is_default_constructible_v<Luna::ProfilingHook>,
              "A profiling hook is an ordinary consumer callable.");

} // namespace

void VerifyUnavailableExtensionBoundaryCompiles() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  // Only the available forms are declarable, and they need no macro.
  [[maybe_unused]] const Luna::RegistrationResult Ordinary =
      Registry.RegisterFunction("ExtensionScale", &ConsumerScale);
  [[maybe_unused]] const Luna::RegistrationResult Selected =
      Registry.RegisterFunction("ExtensionSelected",
                                Luna::Overload<int(int)>(GenericScale));
  [[maybe_unused]] const Luna::RegistrationResult Suspending =
      Registry.RegisterFunction("ExtensionScaleLater", &ConsumerScaleLater);
  [[maybe_unused]] const bool ReflectsAsynchronousDelivery =
      Registry.Reflection()
          .Symbols(Luna::SymbolKind::FunctionCandidate)
          .At(0)
          .IsAsynchronous();

  // Subscribing, unsubscribing, and emitting register like any other
  // callable, and reflection describes the delegate parameter canonically.
  [[maybe_unused]] const Luna::RegistrationResult Subscribing =
      Registry.RegisterFunction("ExtensionConnect", &ConsumerConnect);
  [[maybe_unused]] const Luna::RegistrationResult SubscribingSlot =
      Registry.RegisterFunction("ExtensionConnectSlot", &ConsumerConnectSlot);
  [[maybe_unused]] const Luna::RegistrationResult Emitting =
      Registry.RegisterFunction("ExtensionPublish", &ConsumerPublish);
  [[maybe_unused]] const bool ReflectsDelegateParameter =
      Registry.Reflection().Find("ExtensionConnect").IsValid();

  // The event source itself is still refused through the typed outcome.
  [[maybe_unused]] const Luna::RegistrationResult Refused =
      Registry.RegisterConstant("ExtensionHub", SignalHub());
  [[maybe_unused]] const bool Rejected =
      !Refused.IsSuccess() && Refused.Diagnostic() != nullptr;

  [[maybe_unused]] const Luna::ReflectionSnapshot Published =
      Registry.Reflection();
  [[maybe_unused]] const bool NotAdvertised =
      !Published.Find("ExtensionHub").IsValid();

  // A profiling or debug-UI hook installs and clears through the ordinary
  // typed outcome, and reports only the canonical identity a snapshot would
  // already publish.
  std::vector<Luna::ProfilingEvent> ExtensionEvents;
  [[maybe_unused]] const Luna::RegistrationResult HookInstalled =
      Registry.InstallProfilingHook(
          [&ExtensionEvents](const Luna::ProfilingEvent &Event) {
            ExtensionEvents.push_back(Event);
          });
  [[maybe_unused]] const Luna::ExecutionResult Executed =
      Owner.Execute("ExtensionScale(1)");
  [[maybe_unused]] const Luna::RegistrationResult HookCleared =
      Registry.ClearProfilingHook();
}
