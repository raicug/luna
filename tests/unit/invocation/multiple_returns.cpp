// Zero, scalar, and multiple return shapes, and their atomic publication.
//
// Three things are checked here, in the order the shape travels through Luna.
// First the declared shape: `void` produces zero values, a supported scalar
// one, and a returned `std::pair`, `std::tuple`, or `Luna::ReturnPack` the
// ordered elements of the pack, described by one canonical return type and one
// reflected return shape. Then the publication itself: every element is staged
// and validated and the whole publication reserved before the first value
// reaches a result position, so a refused element exposes zero return values,
// restores the callback checkpoint exactly, and reports one deterministic
// diagnostic naming the one-based return position. Finally the same shapes run
// through the real compiler and virtual machine.

// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/callable_descriptor.hpp>
#include <luna/binding/callable_metadata.hpp>
#include <luna/binding/conversion.hpp>
#include <luna/binding/return_pack.hpp>
#include <luna/binding/supported_callable.hpp>
#include <luna/binding/value.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/core/results/execution_result.hpp>
#include <luna/detail/callable_adapter.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/state/state.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/identity_registry.hpp"
#include "state/invocation/testing/test_hooks.hpp"
#include "state/reflection/database.hpp"
#include "state/reflection/storage.hpp"
#include "state/registration/return_shape.hpp"
#include "state/testing/test_hooks.hpp"
#include "state/type/structural_types.hpp"
#include "state/type/type_record.hpp"

#include <cstddef>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::InvocationPrimitiveTestHooks;
using StateHooks = Luna::Detail::StateTestHooks;
using WriteStatus = Luna::Detail::ReturnWriteStatus;
using Luna::ReturnDisposition;
using Luna::ReturnShape;
using Luna::ValueKind;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "multiple return check failed: " << Description << '\n';
}

[[nodiscard]] bool Mentions(const Luna::ErrorDiagnostic *Diagnostic,
                            std::string_view Text) {
  return Diagnostic && Diagnostic->Message().find(Text) != std::string::npos;
}

// -- the callables whose return shapes are under test -----------------------

void Reset() {}

[[nodiscard]] int Doubled(int Value) { return Value * 2; }

[[nodiscard]] std::pair<int, int> Divide(int Left, int Right) {
  if (Right == 0)
    return {0, 0};
  return {Left / Right, Left % Right};
}

[[nodiscard]] std::tuple<bool, double, std::string> Describe(int Value) {
  return {Value > 0, static_cast<double>(Value) / 2.0, std::to_string(Value)};
}

[[nodiscard]] std::tuple<int> Single(int Value) { return {Value}; }

// The dynamic pack: the element count depends on the arguments rather than on
// the signature.
[[nodiscard]] Luna::ReturnPack Repeat(std::string Text, int Count) {
  Luna::ReturnPack Pack;
  for (int Index = 0; Index < Count; ++Index)
    Pack.AppendText(Text);
  return Pack;
}

// One element that violates the inherited per-string byte policy, so the whole
// pack must publish nothing.
[[nodiscard]] std::pair<int, std::string> Oversized() {
  return {1, std::string(Luna::MaximumConversionStringBytes() + 1, 'x')};
}

static_assert(Luna::SupportedCallable<decltype(&Divide)>,
              "a returned pair of supported values is one declared shape");
static_assert(Luna::SupportedCallable<decltype(&Describe)>,
              "a returned tuple of supported values is one declared shape");
static_assert(Luna::SupportedCallable<decltype(&Repeat)>,
              "a returned dynamic pack is one declared shape");
static_assert(!Luna::SupportedCallable<std::pair<int, float> (*)()>,
              "a pack element must be a supported value");
static_assert(!Luna::SupportedCallable<std::tuple<int, void *> (*)()>,
              "a pack element must be a supported value");

template <class Callable>
[[nodiscard]] Luna::CallableMetadata MetadataOf(Callable &&Target) {
  Luna::ErasedCallableDescriptor Descriptor =
      Luna::Detail::MakeErasedCallableDescriptor(
          std::forward<Callable>(Target));
  return Descriptor.Metadata();
}

[[nodiscard]] Luna::TypeDescriptor Fixed(Luna::FixedTypeKey Key) {
  return Luna::TypeDescriptor::ForFixed(Key);
}

// -- declared shapes --------------------------------------------------------

void CheckDeclaredReturnShapes() {
  const Luna::CallableMetadata Zero = MetadataOf(&Reset);
  const Luna::CallableMetadata Scalar = MetadataOf(&Doubled);
  const Luna::CallableMetadata Pair = MetadataOf(&Divide);
  const Luna::CallableMetadata Tuple = MetadataOf(&Describe);
  const Luna::CallableMetadata OneElement = MetadataOf(&Single);
  const Luna::CallableMetadata Dynamic = MetadataOf(&Repeat);

  Check(Zero.ReturnType().Disposition() == ReturnDisposition::Void &&
            Scalar.ReturnType().Disposition() == ReturnDisposition::Value,
        "void maps to zero values and a scalar to exactly one");

  const std::span<const ValueKind> PairKinds = Pair.ReturnType().PackKinds();
  Check(Pair.ReturnType().Disposition() == ReturnDisposition::Pack &&
            Pair.ReturnType().HasDeclaredPackShape() && PairKinds.size() == 2 &&
            PairKinds[0] == ValueKind::Integer &&
            PairKinds[1] == ValueKind::Integer,
        "a returned pair declares its two ordered element types");

  const std::span<const ValueKind> TupleKinds = Tuple.ReturnType().PackKinds();
  Check(TupleKinds.size() == 3 && TupleKinds[0] == ValueKind::Boolean &&
            TupleKinds[1] == ValueKind::Number &&
            TupleKinds[2] == ValueKind::String,
        "a returned tuple declares its ordered element types");

  Check(Dynamic.ReturnType().Disposition() == ReturnDisposition::Pack &&
            !Dynamic.ReturnType().HasDeclaredPackShape() &&
            Dynamic.ReturnType().PackKinds().empty(),
        "a returned dynamic pack declares no fixed element types");

  // Canonical types: a pack is one canonical return pack of its element types,
  // and a dynamic pack is Luna's owning value pack.
  std::vector<Luna::TypeDescriptor> Elements;
  Elements.push_back(Fixed(Luna::FixedTypeKey::Int32));
  Elements.push_back(Fixed(Luna::FixedTypeKey::Int32));
  const Luna::TypeDescriptor ExpectedPair =
      Luna::Detail::ReturnPackTypeOf(std::move(Elements));

  Check(Luna::Detail::CanonicalReturnType(Zero.ReturnType()) ==
                Fixed(Luna::FixedTypeKey::Void) &&
            Luna::Detail::CanonicalReturnType(Scalar.ReturnType()) ==
                Fixed(Luna::FixedTypeKey::Int32),
        "zero and scalar shapes keep their foundation canonical types");
  Check(Luna::Detail::CanonicalReturnType(Pair.ReturnType()) == ExpectedPair,
        "a declared pack is one canonical return pack of its element types");
  Check(Luna::Detail::CanonicalReturnType(Dynamic.ReturnType()) ==
            Fixed(Luna::FixedTypeKey::ValuePack),
        "a dynamic pack is the canonical owning value pack");

  // Availability follows what the shape publishes, one value per element.
  Check(Luna::Detail::PublishedReturnTypes(ExpectedPair).size() == 2 &&
            Luna::Detail::PublishedReturnTypes(
                Fixed(Luna::FixedTypeKey::ValuePack))
                .empty(),
        "a pack publishes one value per element and a dynamic pack none");

  // Reflection distinguishes zero, scalar, and multiple return shapes.
  Check(Luna::Detail::ReflectedReturnShapeOf(Zero.ReturnType()) ==
                ReturnShape::Zero &&
            Luna::Detail::ReflectedReturnShapeOf(Scalar.ReturnType()) ==
                ReturnShape::Scalar &&
            Luna::Detail::ReflectedReturnShapeOf(Pair.ReturnType()) ==
                ReturnShape::Multiple &&
            Luna::Detail::ReflectedReturnShapeOf(Tuple.ReturnType()) ==
                ReturnShape::Multiple &&
            Luna::Detail::ReflectedReturnShapeOf(Dynamic.ReturnType()) ==
                ReturnShape::Multiple,
        "reflection distinguishes zero, scalar, and multiple return shapes");
  Check(Luna::Detail::ReflectedReturnShapeOf(OneElement.ReturnType()) ==
            ReturnShape::Scalar,
        "a one-element declared pack publishes exactly one value");

  const std::vector<Luna::Detail::ReflectionReturnFields> ReflectedTuple =
      Luna::Detail::MakeReflectedReturnFields(Tuple.ReturnType());
  Check(ReflectedTuple.size() == 3 && ReflectedTuple[0].Name == "Result1" &&
            ReflectedTuple[2].Descriptor == Fixed(Luna::FixedTypeKey::String),
        "each declared pack element is reflected in return order");
  Check(Luna::Detail::MakeReflectedReturnFields(Dynamic.ReturnType()).empty(),
        "a dynamic pack reflects no per-value record");
}

// A reflected multiple shape accepts a dynamic pack's absent element records
// and still refuses a multiple shape that claims exactly one value.
void CheckReflectedMultipleShapeConsistency() {
  using Builder = Luna::Detail::ReflectionGenerationBuilder;
  using Status = Luna::Detail::ReflectionGenerationStatus;

  const auto Candidate = [](ReturnShape Shape, std::size_t ReturnCount) {
    Luna::Detail::ReflectionRecordFields Fields;
    Fields.Kind = Luna::SymbolKind::Namespace;
    Fields.Id = Luna::SymbolId::FromBytes(Luna::SymbolId::Storage{1});
    Fields.Name = "Shape";
    Fields.QualifiedName = "Shape";
    Fields.Returns = Shape;
    for (std::size_t Index = 0; Index < ReturnCount; ++Index) {
      Luna::Detail::ReflectionReturnFields Value;
      Value.Name = "Result" + std::to_string(Index + 1);
      Value.Descriptor = Fixed(Luna::FixedTypeKey::Int32);
      if (const auto Identity =
              Luna::Detail::TypeIdentityRegistry::ComputeIdentity(
                  Value.Descriptor))
        Value.Type = *Identity;
      Fields.ReturnValues.push_back(std::move(Value));
    }
    Builder Generation;
    Generation.AddRecord(std::move(Fields));
    Luna::Detail::ReflectionDatabase Reflection;
    return Reflection.PublishGeneration(Generation);
  };

  Check(Candidate(ReturnShape::Multiple, 0) == Status::Valid,
        "a dynamic pack reflects the multiple shape without element records");
  Check(Candidate(ReturnShape::Multiple, 2) == Status::Valid,
        "a declared pack reflects one record per published value");
  Check(Candidate(ReturnShape::Multiple, 1) == Status::InconsistentReturns,
        "a multiple shape never claims exactly one value");
}

// -- atomic publication -----------------------------------------------------

[[nodiscard]] Luna::InvocationOutcome
StagedPack(std::vector<Luna::Value> Values) {
  return Luna::InvocationOutcome::WithValues(std::move(Values));
}

void CheckPacksPublishEveryValueInOrder() {
  const Luna::ReturnMetadata Declared = Luna::ReturnMetadata::ForPack(
      {ValueKind::Integer, ValueKind::String, ValueKind::Boolean});
  const auto Published = Hooks::Write(
      Declared,
      StagedPack({Luna::Value(7), Luna::Value(std::string("seven")),
                  Luna::Value(true)}),
      0, 0, 0, 2);

  Check(Published.Result.Status == WriteStatus::PackPublished &&
            Published.Result.ReturnCount == 3 && Published.StackDepth == 5,
        "a declared pack publishes one value per element in return order");
  Check(Published.WrittenValues.size() == 3 &&
            Published.WrittenValues[0] == Luna::Value(7) &&
            Published.WrittenValues[1] == Luna::Value(std::string("seven")) &&
            Published.WrittenValues[2] == Luna::Value(true),
        "the published values keep their declared order and types");

  const auto Dynamic =
      Hooks::Write(Luna::ReturnMetadata::ForDynamicPack(),
                   StagedPack({Luna::Value(std::string("a")),
                               Luna::Value(std::string("b"))}));
  Check(Dynamic.Result.Status == WriteStatus::PackPublished &&
            Dynamic.Result.ReturnCount == 2 && Dynamic.StackDepth == 2,
        "a dynamic pack publishes the values the invocation produced");

  const auto Empty = Hooks::Write(Luna::ReturnMetadata::ForDynamicPack(),
                                  StagedPack({}), 0, 0, 0, 1);
  Check(Empty.Result.Status == WriteStatus::PackPublished &&
            Empty.Result.ReturnCount == 0 && Empty.StackDepth == 1,
        "an empty pack publishes zero values without failing");
}

void CheckRefusedPacksPublishNothing() {
  const Luna::ReturnMetadata Declared =
      Luna::ReturnMetadata::ForPack({ValueKind::Integer, ValueKind::String});

  // One element that does not match its declared type refuses the whole pack
  // and names the one-based return position.
  const auto Mismatch = Hooks::Write(
      Declared, StagedPack({Luna::Value(1), Luna::Value(2)}), 0, 0, 0, 3);
  Check(Mismatch.Result.Status == WriteStatus::InternalFailure &&
            Mismatch.Result.ReturnCount == 0 && Mismatch.StackDepth == 3,
        "a refused element publishes zero values and restores the checkpoint");
  Check(Mismatch.Result.Diagnostic.has_value() &&
            Mentions(&*Mismatch.Result.Diagnostic, "Return value 2"),
        "a refused element names its one-based return position");

  // A produced count that disagrees with the declared shape is refused whole.
  const auto WrongCount =
      Hooks::Write(Declared,
                   StagedPack({Luna::Value(1), Luna::Value(std::string("x")),
                               Luna::Value(2)}),
                   0, 0, 0, 1);
  Check(WrongCount.Result.Status == WriteStatus::InternalFailure &&
            WrongCount.Result.ReturnCount == 0 && WrongCount.StackDepth == 1,
        "a pack whose produced count disagrees with its shape publishes "
        "nothing");
  Check(Mentions(&*WrongCount.Result.Diagnostic, "3 values") &&
            Mentions(&*WrongCount.Result.Diagnostic, "publishes 2"),
        "a count refusal reports the produced and declared counts");

  // The inherited per-string byte policy applies to every element.
  const std::string TooLong(Luna::MaximumConversionStringBytes() + 1, 'x');
  const auto Oversize = Hooks::Write(
      Declared, StagedPack({Luna::Value(1), Luna::Value(TooLong)}), 0, 0, 0, 2);
  Check(Oversize.Result.Status == WriteStatus::InternalFailure &&
            Oversize.StackDepth == 2 &&
            Mentions(&*Oversize.Result.Diagnostic, "Return value 2") &&
            Mentions(&*Oversize.Result.Diagnostic, "1048576-byte maximum"),
        "an oversized element reports the inherited byte policy and publishes "
        "nothing");

  // Reservation happens before anything is published, so an unavailable
  // reservation publishes nothing either.
  const auto Unreserved = Hooks::Write(
      Declared, StagedPack({Luna::Value(1), Luna::Value(std::string("x"))}), 0,
      0, 1, 2);
  Check(Unreserved.Result.Status == WriteStatus::InternalFailure &&
            Unreserved.StackDepth == 2 &&
            Mentions(&*Unreserved.Result.Diagnostic, "2 return values"),
        "a pack that cannot reserve its publication publishes nothing");

  // A failure after the elements were published still exposes zero values.
  const auto LateFailure = Hooks::Write(
      Declared, StagedPack({Luna::Value(1), Luna::Value(std::string("x"))}), 1,
      0, 0, 4);
  Check(LateFailure.Result.Status == WriteStatus::InternalFailure &&
            LateFailure.Result.ReturnCount == 0 && LateFailure.StackDepth == 4,
        "an injected late failure restores the checkpoint and publishes zero "
        "values");

  // A pack outcome and a scalar shape never publish each other's values.
  const auto WrongShape =
      Hooks::Write(Luna::ReturnMetadata::ForValue(ValueKind::Integer),
                   StagedPack({Luna::Value(1)}), 0, 0, 0, 1);
  Check(WrongShape.Result.Status == WriteStatus::InternalFailure &&
            WrongShape.StackDepth == 1,
        "a scalar shape refuses a pack outcome");
}

// -- the same shapes through the real compiler and virtual machine ----------

[[nodiscard]] bool RegisterReturnShapes(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  const bool Pair = Registry.RegisterFunction("Divide", &Divide).IsSuccess();
  const bool Tuple =
      Registry.RegisterFunction("Describe", &Describe).IsSuccess();
  const bool Dynamic = Registry.RegisterFunction("Repeat", &Repeat).IsSuccess();
  const bool Scalar =
      Registry.RegisterFunction("Doubled", &Doubled).IsSuccess();
  const bool Zero = Registry.RegisterFunction("Reset", &Reset).IsSuccess();
  const bool Refused =
      Registry.RegisterFunction("Oversized", &Oversized).IsSuccess();
  return Pair && Tuple && Dynamic && Scalar && Zero && Refused;
}

[[nodiscard]] bool Succeeds(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return true;
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    std::cerr << "multiple return source failed: " << Diagnostic->Message()
              << '\n';
  return false;
}

[[nodiscard]] std::string Failure(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return std::string();
  const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
  return Diagnostic ? Diagnostic->Message() : std::string("<no diagnostic>");
}

void CheckReturnShapesThroughTheVirtualMachine() {
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterReturnShapes(Owner),
        "every return shape registers through RegisterFunction");
  const auto EntryDepth = StateHooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner,
                 "local Quotient, Remainder = Divide(7, 2)\n"
                 "assert(Quotient == 3 and Remainder == 1, 'pair')\n"
                 "local Positive, Half, Text = Describe(5)\n"
                 "assert(Positive == true and Half == 2.5 and Text == '5', "
                 "'tuple')\n"
                 "assert(select('#', Divide(9, 4)) == 2, 'pair count')\n"
                 "assert(select('#', Describe(1)) == 3, 'tuple count')\n"),
        "a returned pair and tuple publish their ordered values");

  Check(Succeeds(Owner,
                 "assert(select('#', Repeat('a', 3)) == 3, 'dynamic count')\n"
                 "assert(select('#', Repeat('a', 0)) == 0, 'empty pack')\n"
                 "local First, Second = Repeat('b', 2)\n"
                 "assert(First == 'b' and Second == 'b', 'dynamic values')\n"),
        "a dynamic pack publishes exactly the values it produced");

  Check(Succeeds(Owner, "assert(select('#', Doubled(2)) == 1, 'scalar')\n"
                        "assert(select('#', Reset()) == 0, 'void')\n"),
        "zero and scalar shapes keep publishing zero and one value");

  const std::string Refused = Failure(Owner, "return Oversized()");
  Check(Refused.find("Return value 2") != std::string::npos &&
            Refused.find("1048576-byte maximum") != std::string::npos,
        "a refused element reports one deterministic position diagnostic");

  const auto Restoration =
      StateHooks::ObserveLastCallbackStackRestoration(Owner);
  Check(Restoration.has_value() &&
            Restoration->EntryDepth == Restoration->RestoredDepth &&
            Restoration->ErrorDepth == Restoration->RestoredDepth + 1,
        "a refused pack restores the exact callback checkpoint");

  Check(StateHooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "published and refused packs both restore the root stack depth");

  // The State stays reusable after a refused publication.
  Check(Succeeds(Owner, "local A, B = Divide(11, 3)\n"
                        "assert(A == 3 and B == 2, 'reuse')\n"),
        "a State remains reusable after a refused return pack");
}

} // namespace

int RunMultipleReturnShapeTests() {
  FailureCount = 0;
  CheckDeclaredReturnShapes();
  CheckReflectedMultipleShapeConsistency();
  CheckPacksPublishEveryValueInOrder();
  CheckRefusedPacksPublishNothing();
  CheckReturnShapesThroughTheVirtualMachine();
  return FailureCount == 0 ? 0 : 1;
}
