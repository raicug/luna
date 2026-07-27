#pragma once

// clang-format off
#include <luna/binding/value.hpp>

#include "state/type/conversion_outcome.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class TypeGeneration;
struct TypeRecord;

struct ClassExposureIntent;

enum class StructuredKind { Null, Scalar, List, Map, Handle };

[[nodiscard]] std::string_view StructuredKindText(StructuredKind Kind) noexcept;

class StructuredValue final {
public:
  StructuredValue() = default;

  [[nodiscard]] static StructuredValue Null();
  [[nodiscard]] static StructuredValue Scalar(Value Source);
  [[nodiscard]] static StructuredValue List(std::vector<StructuredValue> Items);

  [[nodiscard]] static StructuredValue Map(std::vector<StructuredValue> Items);

  [[nodiscard]] static StructuredValue Handle(void *Storage,
                                              bool PermitsMutation);

  [[nodiscard]] static StructuredValue
  ExposedHandle(void *Storage, bool PermitsMutation,
                std::shared_ptr<const ClassExposureIntent> Intent);

  [[nodiscard]] StructuredKind Kind() const noexcept { return KindValue; }
  [[nodiscard]] bool IsNull() const noexcept {
    return KindValue == StructuredKind::Null;
  }

  [[nodiscard]] const Value *ScalarValue() const noexcept;
  [[nodiscard]] std::span<const StructuredValue> Elements() const noexcept {
    return ElementValues;
  }

  [[nodiscard]] void *HandleStorage() const noexcept;
  [[nodiscard]] bool HandlePermitsMutation() const noexcept {
    return HandleIsMutable;
  }

  [[nodiscard]] const ClassExposureIntent *HandleExposure() const noexcept;

  [[nodiscard]] std::size_t Size() const noexcept;

  [[nodiscard]] const StructuredValue *
  ElementAt(std::size_t Index) const noexcept;
  [[nodiscard]] const StructuredValue *KeyAt(std::size_t Index) const noexcept;
  [[nodiscard]] const StructuredValue *
  MappedAt(std::size_t Index) const noexcept;

private:
  StructuredKind KindValue = StructuredKind::Null;
  Value ScalarStorage;
  bool HasScalar = false;
  std::vector<StructuredValue> ElementValues;
  void *HandleStorageValue = nullptr;
  bool HandleIsMutable = false;
  std::shared_ptr<const ClassExposureIntent> HandleIntent;
};

[[nodiscard]] bool HasSameStructure(const StructuredValue &Left,
                                    const StructuredValue &Right);

enum class StructuredFailure {
  None,
  UnavailableType,
  TypeMismatch,
  IntegerNonFinite,
  IntegerOutOfRange,
  IntegerFractional,
  StringTooLong,
  EmbeddedNullByte,
  SinglePrecisionRange,
  MissingElement,
  ForeignTableKey,
  ElementCountMismatch,
  UnsupportedMapKey,
  DuplicateMapKey,
  UndeclaredEnumerator,
  UnsupportedFlagBits,
  ForeignUserdata,
  ForeignOriginState,
  MetatableMismatch,
  ExpiredUserdata,
  UserdataTypeMismatch,
  IncompatibleUserdataObject,
  ConstViolation,
  ConflictingOwnership,
  UnavailableConversion,
  StackUnavailable,
  InternalFailure
};

[[nodiscard]] std::string_view
StructuredFailureText(StructuredFailure Failure) noexcept;

[[nodiscard]] bool IsInternalStructuredFailure(StructuredFailure Failure);

struct StructuredDiagnostic final {
  StructuredFailure Failure = StructuredFailure::InternalFailure;

  std::string Path;

  std::string ExpectedType;

  std::string ReceivedType;

  double ReceivedNumber = 0.0;

  std::size_t ReceivedCount = 0;
  std::size_t PermittedCount = 0;

  std::size_t Position = 0;
};

[[nodiscard]] StructuredDiagnostic
StructuredDiagnosticFrom(const ArgumentReadResult &Read,
                         std::string ExpectedType, std::string Path);

class ConversionScope final {
public:
  ConversionScope(const TypeGeneration &Types, lua_State *State) noexcept
      : TypesValue(&Types), StateValue(State) {}

  [[nodiscard]] const TypeGeneration &Types() const noexcept {
    return *TypesValue;
  }
  [[nodiscard]] lua_State *State() const noexcept { return StateValue; }

  void PushElement(std::size_t OneBasedPosition);
  void PushKey(std::string_view KeyText);
  void PushField(std::string_view Name);
  void Pop();

  [[nodiscard]] std::size_t Depth() const noexcept { return Segments.size(); }
  [[nodiscard]] std::string Path() const;

  [[nodiscard]] StructuredDiagnostic Reject(StructuredFailure Failure,
                                            const TypeRecord &Record) const;

private:
  const TypeGeneration *TypesValue;
  lua_State *StateValue;
  std::vector<std::string> Segments;
};

class ConversionPathScope final {
public:
  ConversionPathScope(ConversionScope &Scope, std::size_t OneBasedPosition)
      : ScopeValue(&Scope) {
    Scope.PushElement(OneBasedPosition);
  }

  ConversionPathScope(ConversionScope &Scope, std::string_view KeyText,
                      bool IsField)
      : ScopeValue(&Scope) {
    if (IsField)
      Scope.PushField(KeyText);
    else
      Scope.PushKey(KeyText);
  }

  ConversionPathScope(const ConversionPathScope &) = delete;
  ConversionPathScope &operator=(const ConversionPathScope &) = delete;

  ~ConversionPathScope() { ScopeValue->Pop(); }

private:
  ConversionScope *ScopeValue;
};

struct StructuredReadResult final {
  bool Accepted = false;
  StructuredValue ConvertedValue;
  StructuredDiagnostic Diagnostic;

  [[nodiscard]] static StructuredReadResult Accept(StructuredValue Converted);
  [[nodiscard]] static StructuredReadResult
  Reject(StructuredDiagnostic Failure);

  [[nodiscard]] bool IsSuccess() const noexcept { return Accepted; }
};

struct StructuredWriteResult final {
  bool Accepted = false;

  int PublishedCount = 0;
  StructuredDiagnostic Diagnostic;

  [[nodiscard]] static StructuredWriteResult Accept(int PublishedCount);
  [[nodiscard]] static StructuredWriteResult
  Reject(StructuredDiagnostic Failure);

  [[nodiscard]] bool IsSuccess() const noexcept { return Accepted; }
};

using StructuredReadFunction = StructuredReadResult (*)(
    ConversionScope &Scope, const TypeRecord &Record, int StackIndex);
using StructuredWriteFunction =
    StructuredWriteResult (*)(ConversionScope &Scope, const TypeRecord &Record,
                              const StructuredValue &Source);

enum class ConversionSubjectKind { Callable, Member };

enum class ConversionDirection { Argument, Return, Receiver, MemberValue };

struct ConversionSubject final {
  ConversionSubjectKind Kind = ConversionSubjectKind::Callable;
  std::string Name;
};

[[nodiscard]] std::string
DescribeConversionSubject(const ConversionSubject &Subject);

[[nodiscard]] std::string
DescribeConversionSubjectContext(const ConversionSubject &Subject);

[[nodiscard]] ConversionSubject SubjectForCallable(std::string_view Name,
                                                   bool IsInstanceMember);

[[nodiscard]] std::string DescribeConversionFailure(
    const ConversionSubject &Subject, ConversionDirection Direction,
    std::size_t OneBasedPosition, const StructuredDiagnostic &Failure);

[[nodiscard]] std::string FormatConversionNumber(double Number);

} // namespace Luna::Detail
