// clang-format off
#include <luna/luna.hpp>

#include <coroutine>
#include <functional>
#include <future>
#include <string>
#include <type_traits>
#include <utility>
// clang-format on

#if defined(LUNA_REGISTER) || defined(LUNA_BIND) || defined(LUNA_FUNCTION) ||  \
    defined(LUNA_CLASS) || defined(LUNA_ENUM) || defined(LUNA_MODULE) ||       \
    defined(LUNA_PROPERTY) || defined(LUNA_ANNOTATE) ||                        \
    defined(LUNA_REFLECT) || defined(LUNA_COROUTINE) || defined(LUNA_ASYNC) || \
    defined(LUNA_SIGNAL) || defined(LUNA_EVENT) || defined(LUNA_PROFILE)
#error "Luna's public headers must define no registration or annotation macro."
#endif

namespace {

// Representative unavailable extension surfaces.
class SignalHub final {
public:
  void Connect(std::function<void(int)>) {}
  void Emit(int) const {}
};

using DelegateSlot = std::function<void(int)>;

const auto GenericScale = [](auto Value) { return Value; };

[[nodiscard]] int ConsumerScale(int Value) {
  return Value * 2;
}

// Template auto-binding stays unavailable: an unconstrained generic callable
// declares no signature, so no canonical descriptor exists for it. Explicit
// concrete selection remains the supported opt-in.
static_assert(!Luna::SupportedCallable<decltype(GenericScale)>,
              "A generic template callable must be refused at compile time.");
static_assert(
    Luna::SupportedCallable<decltype(Luna::Overload<int(int)>(GenericScale))>,
    "An explicit concrete overload selection must remain supported.");

// Coroutines and asynchronous tasks stay unavailable.
static_assert(!Luna::SupportedReturn<std::coroutine_handle<>> &&
                  !Luna::SupportedReturn<std::future<int>>,
              "Suspended work must declare no supported return type.");
static_assert(!Luna::SupportedCallable<std::coroutine_handle<> (*)()> &&
                  !Luna::SupportedCallable<std::future<int> (*)()>,
              "Coroutine and asynchronous callables must be refused at "
              "compile time.");
static_assert(
    !Luna::SupportedCallable<decltype(Luna::Overload<std::future<int>()>(
        std::declval<std::future<int> (*)()>()))>,
    "Explicit selection must not smuggle an asynchronous form past the "
    "public constraint.");

// Delegates, signals, and events stay unavailable.
static_assert(!Luna::SupportedParameter<DelegateSlot> &&
                  !Luna::SupportedValue<SignalHub>,
              "A delegate, signal, or event must declare no supported value.");
static_assert(!Luna::SupportedCallable<void (*)(DelegateSlot)> &&
                  !Luna::SupportedCallable<void (*)(SignalHub &)> &&
                  !Luna::SupportedCallable<SignalHub (*)()>,
              "Subscribing, publishing, and handing out an event source must "
              "be refused at compile time.");

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
struct ExposesProfiling : std::false_type {};
template <typename T>
struct ExposesProfiling<T, std::void_t<decltype(std::declval<T &>().Profile())>>
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
              "No public declaration may advertise coroutine or asynchronous "
              "invocation.");
static_assert(!AdvertisedAnywhere<ExposesSubscribe>() &&
                  !AdvertisedAnywhere<ExposesEmit>(),
              "No public declaration may advertise delegates, signals, or "
              "events.");
static_assert(!AdvertisedAnywhere<ExposesProfiling>() &&
                  !AdvertisedAnywhere<ExposesAnnotations>(),
              "No public declaration may advertise profiling, IDE services, "
              "or annotation helpers.");

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

  // An unavailable value is refused through the ordinary typed outcome.
  [[maybe_unused]] const Luna::RegistrationResult Refused =
      Registry.RegisterConstant("ExtensionHub", SignalHub());
  [[maybe_unused]] const bool Rejected =
      !Refused.IsSuccess() && Refused.Diagnostic() != nullptr;

  [[maybe_unused]] const Luna::ReflectionSnapshot Published =
      Registry.Reflection();
  [[maybe_unused]] const bool NotAdvertised =
      !Published.Find("ExtensionHub").IsValid();
}
