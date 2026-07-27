#pragma once

// clang-format off
#include <luna/binding/value.hpp>

#include <functional>
#include <string>
#include <string_view>
// clang-format on

namespace Luna {

enum class MemberAccess { ReadOnly, WriteOnly, ReadWrite };

enum class PropertyEvaluation { Immediate, Computed, Lazy };

enum class MemberOwnership { Copied, Borrowed, Shared };

[[nodiscard]] constexpr std::string_view
MemberAccessText(MemberAccess Access) noexcept {
  switch (Access) {
  case MemberAccess::ReadOnly:
    return "read-only";
  case MemberAccess::WriteOnly:
    return "write-only";
  case MemberAccess::ReadWrite:
    return "read-write";
  }
  return "read-only";
}

[[nodiscard]] constexpr std::string_view
PropertyEvaluationText(PropertyEvaluation Evaluation) noexcept {
  switch (Evaluation) {
  case PropertyEvaluation::Immediate:
    return "immediate";
  case PropertyEvaluation::Computed:
    return "computed";
  case PropertyEvaluation::Lazy:
    return "lazy";
  }
  return "immediate";
}

[[nodiscard]] constexpr std::string_view
MemberOwnershipText(MemberOwnership Ownership) noexcept {
  switch (Ownership) {
  case MemberOwnership::Copied:
    return "copied";
  case MemberOwnership::Borrowed:
    return "borrowed";
  case MemberOwnership::Shared:
    return "shared";
  }
  return "copied";
}

[[nodiscard]] constexpr bool PermitsMemberRead(MemberAccess Access) noexcept {
  return Access != MemberAccess::WriteOnly;
}

[[nodiscard]] constexpr bool PermitsMemberWrite(MemberAccess Access) noexcept {
  return Access != MemberAccess::ReadOnly;
}

class PropertyPolicy final {
public:
  PropertyPolicy() = default;

  [[nodiscard]] static PropertyPolicy ReadOnly() {
    return PropertyPolicy(MemberAccess::ReadOnly,
                          PropertyEvaluation::Immediate);
  }

  [[nodiscard]] static PropertyPolicy WriteOnly() {
    return PropertyPolicy(MemberAccess::WriteOnly,
                          PropertyEvaluation::Immediate);
  }

  [[nodiscard]] static PropertyPolicy ReadWrite() {
    return PropertyPolicy(MemberAccess::ReadWrite,
                          PropertyEvaluation::Immediate);
  }

  [[nodiscard]] static PropertyPolicy Computed() {
    return PropertyPolicy(MemberAccess::ReadOnly, PropertyEvaluation::Computed);
  }

  [[nodiscard]] static PropertyPolicy Lazy() {
    return PropertyPolicy(MemberAccess::ReadOnly, PropertyEvaluation::Lazy);
  }

  [[nodiscard]] static PropertyPolicy LazyReadWrite() {
    return PropertyPolicy(MemberAccess::ReadWrite, PropertyEvaluation::Lazy);
  }

  [[nodiscard]] static PropertyPolicy ComputedReadWrite() {
    return PropertyPolicy(MemberAccess::ReadWrite,
                          PropertyEvaluation::Computed);
  }

  [[nodiscard]] MemberAccess Access() const noexcept { return AccessValue; }

  [[nodiscard]] PropertyEvaluation Evaluation() const noexcept {
    return EvaluationValue;
  }

  [[nodiscard]] bool PermitsRead() const noexcept {
    return PermitsMemberRead(AccessValue);
  }

  [[nodiscard]] bool PermitsWrite() const noexcept {
    return PermitsMemberWrite(AccessValue);
  }

  [[nodiscard]] bool IsLazy() const noexcept {
    return EvaluationValue == PropertyEvaluation::Lazy;
  }

  [[nodiscard]] bool IsCoherent() const noexcept {
    if (EvaluationValue == PropertyEvaluation::Immediate)
      return true;
    return PermitsMemberRead(AccessValue);
  }

private:
  PropertyPolicy(MemberAccess Selected, PropertyEvaluation Produced) noexcept
      : AccessValue(Selected), EvaluationValue(Produced) {}

  MemberAccess AccessValue = MemberAccess::ReadOnly;
  PropertyEvaluation EvaluationValue = PropertyEvaluation::Immediate;
};

class FieldPolicy final {
public:
  FieldPolicy() = default;

  [[nodiscard]] static FieldPolicy ReadOnly() {
    return FieldPolicy(MemberAccess::ReadOnly, MemberOwnership::Copied, true);
  }

  [[nodiscard]] static FieldPolicy ReadWrite() {
    return FieldPolicy(MemberAccess::ReadWrite, MemberOwnership::Copied, true);
  }

  [[nodiscard]] static FieldPolicy Owned(MemberOwnership Selected) {
    return FieldPolicy(MemberAccess::ReadWrite, Selected, true);
  }

  [[nodiscard]] MemberAccess Access() const noexcept { return AccessValue; }

  [[nodiscard]] bool DeclaresDirection() const noexcept {
    return DirectionIsExplicit;
  }

  [[nodiscard]] MemberOwnership Ownership() const noexcept {
    return OwnershipValue;
  }

  [[nodiscard]] bool PermitsRead() const noexcept {
    return PermitsMemberRead(AccessValue);
  }

  [[nodiscard]] bool PermitsWrite() const noexcept {
    return PermitsMemberWrite(AccessValue);
  }

  [[nodiscard]] bool IsCoherent() const noexcept {
    return OwnershipValue == MemberOwnership::Copied;
  }

private:
  FieldPolicy(MemberAccess Selected, MemberOwnership Owning,
              bool IsExplicit) noexcept
      : AccessValue(Selected), OwnershipValue(Owning),
        DirectionIsExplicit(IsExplicit) {}

  MemberAccess AccessValue = MemberAccess::ReadWrite;
  MemberOwnership OwnershipValue = MemberOwnership::Copied;
  bool DirectionIsExplicit = false;
};

namespace Detail {

struct MemberReadOutcome final {
  bool Succeeded = false;
  Value Produced;
  std::string Refusal;

  [[nodiscard]] static MemberReadOutcome Accept(Value Result) {
    MemberReadOutcome Outcome;
    Outcome.Succeeded = true;
    Outcome.Produced = std::move(Result);
    return Outcome;
  }

  [[nodiscard]] static MemberReadOutcome Refuse(std::string Reason) {
    MemberReadOutcome Outcome;
    Outcome.Refusal = std::move(Reason);
    return Outcome;
  }
};

struct MemberWriteOutcome final {
  bool Succeeded = false;
  std::string Refusal;

  [[nodiscard]] static MemberWriteOutcome Accept() {
    MemberWriteOutcome Outcome;
    Outcome.Succeeded = true;
    return Outcome;
  }

  [[nodiscard]] static MemberWriteOutcome Refuse(std::string Reason) {
    MemberWriteOutcome Outcome;
    Outcome.Refusal = std::move(Reason);
    return Outcome;
  }
};

using MemberReadOperation =
    std::function<MemberReadOutcome(const void *Object)>;

using MemberWriteOperation =
    std::function<MemberWriteOutcome(void *Object, const Value &Incoming)>;

} // namespace Detail

} // namespace Luna
