#pragma once

// clang-format off
#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>
// clang-format on

namespace Luna {

class NamespaceBuilder;

template <class Configure>
concept ModuleConfiguration = std::invocable<Configure &, NamespaceBuilder &>;

namespace Detail {

class ModuleRegistrationTarget {
public:
  ModuleRegistrationTarget() = default;
  ModuleRegistrationTarget(const ModuleRegistrationTarget &) = delete;
  ModuleRegistrationTarget &
  operator=(const ModuleRegistrationTarget &) = delete;
  virtual ~ModuleRegistrationTarget() = default;

  virtual void Invoke(NamespaceBuilder &Builder) = 0;
};

template <class Configure>
class ModuleRegistrationHolder final : public ModuleRegistrationTarget {
public:
  explicit ModuleRegistrationHolder(Configure Configuration)
      : Held(std::move(Configuration)) {}

  void Invoke(NamespaceBuilder &Builder) override { Held(Builder); }

private:
  Configure Held;
};

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

  void Invoke(NamespaceBuilder &Builder) const {
    if (Target)
      Target->Invoke(Builder);
  }

private:
  std::shared_ptr<ModuleRegistrationTarget> Target;
};

} // namespace Detail

} // namespace Luna
