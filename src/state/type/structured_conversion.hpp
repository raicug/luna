#pragma once

// The recursive half of the conversion vocabulary. The foundation converters
// move exactly one scalar `Luna::Value` in either direction, which cannot
// describe an aggregate, so a structural type converts a `StructuredValue`
// instead: a Luna-owned staging tree of nulls, scalars, ordered lists, and
// ordered key/value maps that holds no virtual-machine resource.
//
// A structural conversion also needs a path. Every recursive step pushes one
// element position or map key onto the scope, so the first deterministic
// failure can name the complete nested path - `argument 2[4].Key` - instead of
// only the argument it started from. Nothing here imposes a nesting, element,
// key, or graph cap: the only Luna-owned size policy is the inherited
// foundation string limit, and the only other bound is the virtual-machine
// stack capacity a publication must actually reserve, reported with the count
// it needed.
//
// This header names Luau only through an opaque forward declaration.

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

// The ownership statement of one native object staged for exposure. Only the
// userdata write path defines and reads it; the conversion vocabulary carries
// it without ever interpreting it.
struct ClassExposureIntent;

// Shape of one staged value.
enum class StructuredKind {
  // The absent value of a nullable type: `nil` on the Luau side.
  Null,
  // One foundation scalar.
  Scalar,
  // An ordered element list: a sequence, fixed array, pair, tuple, or pack.
  List,
  // Ordered key/value entries of an associative map.
  Map,
  // One validated native handle of a registered class.
  Handle
};

[[nodiscard]] std::string_view StructuredKindText(StructuredKind Kind) noexcept;

// One fully staged native value. It is complete before anything is published,
// which is what lets a writer validate an entire aggregate and then publish it
// in one step, and lets a reader reject a nested element without ever handing a
// partial aggregate to native code.
class StructuredValue final {
public:
  StructuredValue() = default;

  [[nodiscard]] static StructuredValue Null();
  [[nodiscard]] static StructuredValue Scalar(Value Source);
  [[nodiscard]] static StructuredValue List(std::vector<StructuredValue> Items);

  // Ordered map entries as alternating key and mapped values. An odd count is
  // rejected as an empty map rather than silently truncated.
  [[nodiscard]] static StructuredValue Map(std::vector<StructuredValue> Items);

  // One native object of a registered class that already passed every access
  // check. The pointer is borrowed for the callback that read it: the staged
  // value owns nothing, holds no virtual-machine resource, and never keeps the
  // object alive. A handle is only ever produced by validated access, so a
  // staged handle is proof that the value was neither stale, foreign, wrongly
  // typed, nor const-violating.
  [[nodiscard]] static StructuredValue Handle(void *Storage,
                                              bool PermitsMutation);

  // One native object staged for exposure as a value of a registered class,
  // together with the ownership statement that decides how the value will be
  // owned and released. A handle produced by reading carries no intent, because
  // reading establishes no ownership; only a staged exposure does, which is why
  // writing one without an intent is refused rather than guessed at.
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

  // The validated native object of a staged handle, or null for every other
  // kind.
  [[nodiscard]] void *HandleStorage() const noexcept;
  [[nodiscard]] bool HandlePermitsMutation() const noexcept {
    return HandleIsMutable;
  }

  // The ownership statement of a staged exposure, or null for a handle that
  // only names a validated object.
  [[nodiscard]] const ClassExposureIntent *HandleExposure() const noexcept;

  // Element count of a list, or entry count of a map.
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

// Structural equality of two staged values. Only tests and round-trip models
// need it; conversion itself never compares staged values.
[[nodiscard]] bool HasSameStructure(const StructuredValue &Left,
                                    const StructuredValue &Right);

// The first deterministic reason one recursive conversion refused.
enum class StructuredFailure {
  None,
  // The captured generation does not describe the requested canonical type.
  UnavailableType,
  TypeMismatch,
  IntegerNonFinite,
  IntegerOutOfRange,
  IntegerFractional,
  StringTooLong,
  EmbeddedNullByte,
  SinglePrecisionRange,
  // A sequence, array, pair, tuple, or pack position held no value.
  MissingElement,
  // A table carried a key outside the shape the canonical type describes.
  ForeignTableKey,
  // A fixed array, pair, tuple, or pack received the wrong element count.
  ElementCountMismatch,
  // A map key was not a boolean, number, or string.
  UnsupportedMapKey,
  // Two distinct raw keys converted to one canonical key.
  DuplicateMapKey,
  // The integer is a valid value of the enumeration's representation but not
  // one of its declared enumerators.
  UndeclaredEnumerator,
  // A declared bitflag enumeration received a value carrying at least one bit
  // it does not support. The value is rejected whole, never truncated.
  UnsupportedFlagBits,
  // The value is not a Luna userdata at all, or it was written by another
  // layout version.
  ForeignUserdata,
  // The userdata was exposed by a different logical State.
  ForeignOriginState,
  // The userdata does not carry the metatable identity of the requested class.
  MetatableMismatch,
  // The userdata has no payload, lost its lifetime handle, or is unpublished,
  // invalidated, destroyed, or released.
  ExpiredUserdata,
  // Neither the userdata's dynamic type nor any registered cast path leads to
  // the requested class.
  UserdataTypeMismatch,
  // One registered safe cast policy leads to the requested class, but its
  // non-mutating compatibility check refused this object.
  IncompatibleUserdataObject,
  // A mutating access arrived at a const view.
  ConstViolation,
  // The object is already exposed in this State under a different ownership
  // model, so exposing it again would create a second owner of one object.
  ConflictingOwnership,
  // The type is described but its conversion arrives with a later milestone.
  UnavailableConversion,
  // The virtual machine could not reserve the slots a publication needs.
  StackUnavailable,
  InternalFailure
};

[[nodiscard]] std::string_view
StructuredFailureText(StructuredFailure Failure) noexcept;

// A failure the caller reports as an internal refusal rather than a caller
// mistake.
[[nodiscard]] bool IsInternalStructuredFailure(StructuredFailure Failure);

struct StructuredDiagnostic final {
  StructuredFailure Failure = StructuredFailure::InternalFailure;

  // Complete nested element or key path of the failure, empty at the root.
  std::string Path;

  // Public name of the canonical type that refused the value.
  std::string ExpectedType;

  // Luau representation actually received.
  std::string ReceivedType;

  double ReceivedNumber = 0.0;

  // Received and permitted size of the offending resource, when the failure is
  // a size or shape policy. A zero permitted count means the failure carries no
  // permitted bound.
  std::size_t ReceivedCount = 0;
  std::size_t PermittedCount = 0;

  // One-based call argument or return position the conversion itself
  // determined, used by variadic packs. Zero when the caller owns the position.
  std::size_t Position = 0;
};

// Bridges one foundation scalar rejection into the recursive vocabulary, so a
// nested foundation leaf keeps the exact foundation wording.
[[nodiscard]] StructuredDiagnostic
StructuredDiagnosticFrom(const ArgumentReadResult &Read,
                         std::string ExpectedType, std::string Path);

// One conversion in progress. It carries the generation the invocation
// captured, the stack the conversion runs on, and the nested path built so far.
// Depth is observed, never limited.
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

  // One rejection at the current path, already carrying the record's public
  // name and size policy.
  [[nodiscard]] StructuredDiagnostic Reject(StructuredFailure Failure,
                                            const TypeRecord &Record) const;

private:
  const TypeGeneration *TypesValue;
  lua_State *StateValue;
  std::vector<std::string> Segments;
};

// Scoped path segment: the path is restored even when a converter returns
// early, so sibling elements never inherit a stale path.
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

  // Values published on success. A void return publishes none, a scalar or one
  // aggregate table publishes one, and a root pair, tuple, or return pack
  // publishes one per ordered element.
  int PublishedCount = 0;
  StructuredDiagnostic Diagnostic;

  [[nodiscard]] static StructuredWriteResult Accept(int PublishedCount);
  [[nodiscard]] static StructuredWriteResult
  Reject(StructuredDiagnostic Failure);

  [[nodiscard]] bool IsSuccess() const noexcept { return Accepted; }
};

// The committing recursive reader and writer of one canonical type. A record
// supplies these instead of the scalar pair when its type is structural.
using StructuredReadFunction = StructuredReadResult (*)(
    ConversionScope &Scope, const TypeRecord &Record, int StackIndex);
using StructuredWriteFunction =
    StructuredWriteResult (*)(ConversionScope &Scope, const TypeRecord &Record,
                              const StructuredValue &Source);

// What one conversion belongs to, so a nested failure names the callable or
// member rather than only the position.
enum class ConversionSubjectKind { Callable, Member };

// Which part of one call a conversion belongs to. The receiver of an instance
// member is rank position zero rather than one of the numbered arguments, so it
// is named rather than numbered, and the single value one property or field
// carries has no call position at all.
enum class ConversionDirection { Argument, Return, Receiver, MemberValue };

struct ConversionSubject final {
  ConversionSubjectKind Kind = ConversionSubjectKind::Callable;
  std::string Name;
};

// The subject prefix every diagnostic of one callable or member starts with:
// `Callable 'Name'` or `Member 'Class.Member'`. It is the one place that
// wording is decided, so an argument, a receiver, a return, a getter, and a
// setter of one member all name it identically.
[[nodiscard]] std::string
DescribeConversionSubject(const ConversionSubject &Subject);

// The same subject inside a sentence: `callable 'Name'` or
// `member 'Class.Member'`.
[[nodiscard]] std::string
DescribeConversionSubjectContext(const ConversionSubject &Subject);

// The subject one callable's diagnostics name. An instance member names itself
// as a member of its class; every other callable - a free function, a static
// method, a constructor, a factory - names itself as a callable.
[[nodiscard]] ConversionSubject SubjectForCallable(std::string_view Name,
                                                   bool IsInstanceMember);

// One atomic diagnostic message: subject, direction, one-based position,
// complete nested path, and the received and permitted sizes when the failure
// is a size or shape policy.
[[nodiscard]] std::string DescribeConversionFailure(
    const ConversionSubject &Subject, ConversionDirection Direction,
    std::size_t OneBasedPosition, const StructuredDiagnostic &Failure);

// Foundation-identical number formatting, used by every nested diagnostic.
[[nodiscard]] std::string FormatConversionNumber(double Number);

} // namespace Luna::Detail
