#pragma once

// clang-format off
#include <luna/type/stable_type_key.hpp>

#include <utility>
// clang-format on

namespace Luna {

class ReceiverMetadata final {
public:
  ReceiverMetadata() = default;

  [[nodiscard]] static ReceiverMetadata ForInstance(StableTypeKey Class,
                                                    bool ReadsOnly) {
    ReceiverMetadata Declared;
    Declared.ClassValue = std::move(Class);
    Declared.ReadsOnlyValue = ReadsOnly;
    return Declared;
  }

  [[nodiscard]] const StableTypeKey &Class() const noexcept {
    return ClassValue;
  }

  [[nodiscard]] bool IsConst() const noexcept { return ReadsOnlyValue; }

  [[nodiscard]] bool RequiresMutation() const noexcept {
    return !ReadsOnlyValue;
  }

  [[nodiscard]] bool IsDeclared() const noexcept {
    return ClassValue.IsValid();
  }

private:
  StableTypeKey ClassValue;
  bool ReadsOnlyValue = false;
};

class InstanceReceiver final {
public:
  InstanceReceiver() = default;

  [[nodiscard]] static InstanceReceiver Validated(void *Object,
                                                  bool PermitsMutation) {
    InstanceReceiver Bound;
    Bound.StorageValue = Object;
    Bound.PermitsMutationValue = PermitsMutation;
    return Bound;
  }

  [[nodiscard]] void *Storage() const noexcept { return StorageValue; }

  [[nodiscard]] bool PermitsMutation() const noexcept {
    return PermitsMutationValue;
  }

  [[nodiscard]] bool IsBound() const noexcept {
    return StorageValue != nullptr;
  }

private:
  void *StorageValue = nullptr;
  bool PermitsMutationValue = false;
};

} // namespace Luna
