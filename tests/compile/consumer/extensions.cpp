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

[[nodiscard]] int ConsumerScale(int Value) { return Value * 2; }

[[nodiscard]] Luna::AsyncTask<int> ConsumerScaleLater(int Value) {
  Luna::AsyncCompletionSource<int> Source;
  Luna::AsyncTask<int> Pending = Source.Task();
  static_cast<void>(Source.Complete(Value * 2));
  return Pending;
}

static_assert(!Luna::SupportedCallable<decltype(GenericScale)>,
              "A generic template callable must be refused at compile time.");
static_assert(
    Luna::SupportedCallable<decltype(Luna::Overload<int(int)>(GenericScale))>,
    "An explicit concrete overload selection must remain supported.");

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

static_assert(!Luna::SupportedReturn<std::coroutine_handle<>> &&
                  !Luna::SupportedReturn<std::future<SignalHub>> &&
                  !Luna::SupportedReturn<Luna::AsyncTask<unsigned int>>,
              "Suspended work must still declare a canonical awaited value.");

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

static_assert(!AdvertisedAnywhere<ExposesCoroutine>() &&
                  !AdvertisedAnywhere<ExposesAsync>() &&
                  !AdvertisedAnywhere<ExposesAwait>(),
              "Asynchronous invocation must not add a parallel public API.");

static_assert(!AdvertisedAnywhere<ExposesSubscribe>() &&
                  !AdvertisedAnywhere<ExposesEmit>(),
              "Delegates, signals, and events must stay ordinary reflected "
              "callables rather than a parallel public API.");
static_assert(!AdvertisedAnywhere<ExposesAnnotations>(),
              "No public declaration may advertise annotation helpers.");

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

  [[maybe_unused]] const Luna::RegistrationResult Subscribing =
      Registry.RegisterFunction("ExtensionConnect", &ConsumerConnect);
  [[maybe_unused]] const Luna::RegistrationResult SubscribingSlot =
      Registry.RegisterFunction("ExtensionConnectSlot", &ConsumerConnectSlot);
  [[maybe_unused]] const Luna::RegistrationResult Emitting =
      Registry.RegisterFunction("ExtensionPublish", &ConsumerPublish);
  [[maybe_unused]] const bool ReflectsDelegateParameter =
      Registry.Reflection().Find("ExtensionConnect").IsValid();

  [[maybe_unused]] const Luna::RegistrationResult Refused =
      Registry.RegisterConstant("ExtensionHub", SignalHub());
  [[maybe_unused]] const bool Rejected =
      !Refused.IsSuccess() && Refused.Diagnostic() != nullptr;

  [[maybe_unused]] const Luna::ReflectionSnapshot Published =
      Registry.Reflection();
  [[maybe_unused]] const bool NotAdvertised =
      !Published.Find("ExtensionHub").IsValid();

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
