#pragma once

// The private callback boundary of one module registration.
//
// `RegisterModule` and `ProvideModule` accept any consumer callable that
// configures one transaction-attached `NamespaceBuilder`. The callable is
// erased into one Luna-owned value here, so the private module loader can hold
// a definition without knowing the consumer's type and without a macro, a Luau
// type, or a stack operation ever appearing in this boundary.
//
// A module callback is invoked while the module's outermost registration
// transaction is open. Everything it registers joins that transaction, and
// nothing it throws may cross this boundary: the loader contains the exception,
// poisons the attempt, and restores the exact pre-load State.

// clang-format off
#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>
// clang-format on

namespace Luna {

class NamespaceBuilder;

// One consumer callable that configures a module's scoped registration.
template <class Configure>
concept ModuleConfiguration = std::invocable<Configure &, NamespaceBuilder &>;

namespace Detail {

// The erased interface of one module registration callback.
class ModuleRegistrationTarget {
public:
  ModuleRegistrationTarget() = default;
  ModuleRegistrationTarget(const ModuleRegistrationTarget &) = delete;
  ModuleRegistrationTarget &
  operator=(const ModuleRegistrationTarget &) = delete;
  virtual ~ModuleRegistrationTarget() = default;

  virtual void Invoke(NamespaceBuilder &Builder) = 0;
};

// One concrete consumer callable held by value.
template <class Configure>
class ModuleRegistrationHolder final : public ModuleRegistrationTarget {
public:
  explicit ModuleRegistrationHolder(Configure Configuration)
      : Held(std::move(Configuration)) {}

  void Invoke(NamespaceBuilder &Builder) override { Held(Builder); }

private:
  Configure Held;
};

// One erased module registration callback. A default-constructed value carries
// no callback at all, which the loader reports as a deterministic failure
// instead of loading a module that registers nothing.
class ModuleRegistration final {
public:
  ModuleRegistration() = default;

  template <class Configure>
    requires ModuleConfiguration<Configure>
  [[nodiscard]] static ModuleRegistration Create(Configure &&Configuration) {
    using Held = std::decay_t<Configure>;
    ModuleRegistration Erased;
    Erased.Target = std::make_shared<ModuleRegistrationHolder<Held>>(
        Held(std::forward<Configure>(Configuration)));
    return Erased;
  }

  [[nodiscard]] bool IsValid() const noexcept { return Target != nullptr; }

  // Invokes the consumer callback with one transaction-attached builder. The
  // loader calls this behind its own protected boundary.
  void Invoke(NamespaceBuilder &Builder) const {
    if (Target)
      Target->Invoke(Builder);
  }

private:
  std::shared_ptr<ModuleRegistrationTarget> Target;
};

} // namespace Detail

} // namespace Luna
