// The lifecycle-facing public API, compiled in a consumer that links only
// `Luna::Luna`: creating, moving, and destroying a State, loading and providing
// modules, declaring and invalidating the lifetime of a borrowed object, and
// retaining one reflection snapshot across a State's whole life all compile
// without a Luau include path, declaration, pointer, macro, or link.
//
// Dispatch indirection is entirely private. Nothing here can name a dispatch
// slot, a dispatch generation, a dispatch table, or a retention; nothing here
// can reach the virtual machine the State owns; and every lifecycle-facing
// declaration answers with an ordinary Luna or standard-library value.

// clang-format off
#include <luna/luna.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
// clang-format on

namespace {

// Whether one consumer-facing type exposes any accessor a lifecycle operation
// would need if dispatch storage or the machine itself were public. Each of
// these must stay unavailable: a consumer names symbols and canonical paths,
// and Luna alone names the storage behind them.
template <typename T, typename = void>
struct ExposesDispatchStorage : std::false_type {};
template <typename T>
struct ExposesDispatchStorage<
    T, std::void_t<decltype(std::declval<T &>().Dispatch())>> : std::true_type {
};

template <typename T, typename = void>
struct ExposesMachine : std::false_type {};
template <typename T>
struct ExposesMachine<T, std::void_t<decltype(std::declval<T &>().Machine())>>
    : std::true_type {};

template <typename T, typename = void>
struct ExposesVirtualMachine : std::false_type {};
template <typename T>
struct ExposesVirtualMachine<
    T, std::void_t<decltype(std::declval<T &>().VirtualMachine())>>
    : std::true_type {};

template <typename T, typename = void>
struct ExposesHandle : std::false_type {};
template <typename T>
struct ExposesHandle<T, std::void_t<decltype(std::declval<T &>().Handle())>>
    : std::true_type {};

template <typename T, typename = void> struct ExposesSlot : std::false_type {};
template <typename T>
struct ExposesSlot<T, std::void_t<decltype(std::declval<T &>().Slot())>>
    : std::true_type {};

static_assert(!ExposesDispatchStorage<Luna::State>::value &&
                  !ExposesDispatchStorage<Luna::BindingRegistry>::value &&
                  !ExposesDispatchStorage<Luna::NamespaceBuilder>::value,
              "No public declaration may expose dispatch storage.");
static_assert(!ExposesSlot<Luna::State>::value &&
                  !ExposesSlot<Luna::BindingRegistry>::value &&
                  !ExposesSlot<Luna::NamespaceBuilder>::value,
              "No public declaration may expose a dispatch slot.");
static_assert(!ExposesMachine<Luna::State>::value &&
                  !ExposesVirtualMachine<Luna::State>::value &&
                  !ExposesHandle<Luna::State>::value,
              "State must remain the sole owner of a machine it never hands "
              "out.");
static_assert(!ExposesMachine<Luna::BindingRegistry>::value &&
                  !ExposesVirtualMachine<Luna::BindingRegistry>::value &&
                  !ExposesHandle<Luna::BindingRegistry>::value,
              "A registry must never hand out direct machine access.");

// Everything a lifecycle-facing consumer operation answers with is an ordinary
// Luna value: an outcome, a snapshot, or a standard-library type.
static_assert(
    std::is_same_v<
        decltype(std::declval<Luna::BindingRegistry &>().RegisterModule(
            std::declval<Luna::ModuleManifest>(),
            std::declval<void (&)(Luna::NamespaceBuilder &)>())),
        Luna::RegistrationResult>,
    "Module loading must keep answering one ordinary Luna outcome.");
static_assert(
    std::is_same_v<
        decltype(std::declval<Luna::BindingRegistry &>().ProvideModule(
            std::declval<Luna::ModuleManifest>(),
            std::declval<void (&)(Luna::NamespaceBuilder &)>())),
        Luna::RegistrationResult>,
    "Providing a module definition must keep answering one Luna outcome.");
static_assert(
    std::is_same_v<
        decltype(std::declval<const Luna::BindingRegistry &>().Reflection()),
        Luna::ReflectionSnapshot>,
    "A lifecycle-facing snapshot must stay one owning consumer value.");
static_assert(std::is_copy_constructible_v<Luna::ReflectionSnapshot>,
              "A retained snapshot must outlive the State that published it.");

// The one lifetime statement a consumer makes about a borrowed native object.
// It is a Luna-owned counter, never an address, never a machine reference, and
// never a dispatch generation.
static_assert(std::is_copy_constructible_v<Luna::LifetimeHandle> &&
                  std::is_nothrow_move_constructible_v<Luna::LifetimeHandle>,
              "A lifetime handle must remain an ordinary copyable value.");
static_assert(
    std::is_same_v<
        decltype(std::declval<const Luna::LifetimeHandle &>().Generation()),
        std::uint64_t>,
    "A declared lifetime must report a plain Luna-local counter.");
static_assert(
    std::is_same_v<
        decltype(std::declval<Luna::LifetimeHandle &>().Invalidate()), void>,
    "Invalidation must remain one plain consumer operation.");
static_assert(
    std::is_same_v<
        decltype(std::declval<const Luna::LifetimeHandle &>().RefersToSame(
            std::declval<const Luna::LifetimeHandle &>())),
        bool>,
    "Two lifetime handles must stay comparable without any Luna internal.");
static_assert(std::is_nothrow_move_constructible_v<Luna::State> &&
                  std::is_nothrow_move_assignable_v<Luna::State>,
              "State ownership transfer must stay a nothrow consumer move.");

struct ConsumerProbe final {
  int Charge = 5;

  [[nodiscard]] int Level() const { return Charge * 3; }
};

[[nodiscard]] int ConsumerScale(int Value) { return Value * 2; }

void ConfigureConsumerUnits(Luna::NamespaceBuilder &Builder) {
  Luna::NamespaceBuilder Units = Builder.RegisterNamespace("LifecycleUnits");
  static_cast<void>(Units.RegisterConstant("Scale", 3));
  static_cast<void>(Units.RegisterFunction("Scale2", &ConsumerScale));
}

void ConfigureConsumerTools(Luna::NamespaceBuilder &Builder) {
  Luna::NamespaceBuilder Tools = Builder.RegisterNamespace("LifecycleTools");
  static_cast<void>(Tools.RegisterFunction("Scale", &ConsumerScale));
  Luna::ClassBuilder<ConsumerProbe> Probe = Tools.RegisterClass<ConsumerProbe>(
      "Probe", Luna::StableTypeKey("consumer.lifecycle.Probe"));
  static_cast<void>(Probe.Constructor<>()
                        .Field("Charge", &ConsumerProbe::Charge)
                        .Property("Level", &ConsumerProbe::Level));
}

[[nodiscard]] std::optional<Luna::ModuleManifest>
ConsumerManifest(std::string Identity, std::string_view VersionText) {
  return Luna::ModuleManifest::TryCreate(
      std::move(Identity),
      Luna::SemanticVersion::TryParse(VersionText)
          .value_or(Luna::SemanticVersion()),
      {}, std::string(), {});
}

} // namespace

// One consumer that exercises the whole lifecycle-facing surface: a State is
// created, populated through modules and scoped builders, executed against,
// moved, and destroyed, while one lifetime statement and one retained snapshot
// outlive it - all with Luna's public headers alone.
void VerifyLifecycleConsumerBoundaryCompiles() {
  Luna::LifetimeHandle Borrowed;
  Luna::ReflectionSnapshot Retained;

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();

    [[maybe_unused]] const Luna::RegistrationResult Root =
        Registry.RegisterFunction("LifecycleScale", &ConsumerScale);

    if (const std::optional<Luna::ModuleManifest> Units =
            ConsumerManifest("consumer.lifecycle.units", "1.2.0")) {
      [[maybe_unused]] const Luna::RegistrationResult Provided =
          Registry.ProvideModule(*Units, &ConfigureConsumerUnits);
    }
    if (const std::optional<Luna::ModuleManifest> Tools =
            ConsumerManifest("consumer.lifecycle.tools", "2.0.1")) {
      [[maybe_unused]] const Luna::RegistrationResult Loaded =
          Registry.RegisterModule(*Tools, &ConfigureConsumerTools);
    }

    [[maybe_unused]] const Luna::ExecutionResult Executed =
        Owner.Execute("return LifecycleScale(21)");

    // A borrowed object's lifetime is declared, compared, and ended entirely
    // through Luna values; nothing here names storage of any kind.
    const Luna::LifetimeHandle Copied = Borrowed;
    [[maybe_unused]] const bool SameLifetime = Borrowed.RefersToSame(Copied) &&
                                               Borrowed.IsDeclared() &&
                                               Borrowed.IsValid();
    [[maybe_unused]] const std::uint64_t Live = Borrowed.Generation();
    [[maybe_unused]] const bool Undeclared =
        !Luna::LifetimeHandle::Undeclared().IsDeclared();

    Retained = Registry.Reflection();

    // Transferring ownership of the machine is an ordinary consumer move, and
    // the moved-to State keeps answering the same lifecycle questions.
    Luna::State Moved = std::move(Owner);
    [[maybe_unused]] const bool Ready = Moved.IsReady();
    [[maybe_unused]] const Luna::ExecutionResult Again =
        Moved.Execute("return LifecycleScale(20)");
  }

  // The State, its machine, and every closure it installed are gone; the
  // lifetime statement and the snapshot are still ordinary consumer values.
  Borrowed.Invalidate();
  Borrowed.Invalidate();
  [[maybe_unused]] const bool Ended = !Borrowed.IsValid();
  [[maybe_unused]] const std::size_t Callables =
      Retained.Symbols(Luna::SymbolKind::OverloadSet).Size();
  [[maybe_unused]] const std::size_t Modules = Retained.Modules().Size();
  [[maybe_unused]] const bool Reflected =
      Retained.Find("LifecycleScale").IsValid();
}
