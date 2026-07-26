// Integration coverage of the whole typed class-member surface through the real
// Luau compiler and virtual machine.
//
// Nothing here reaches a member through a private hook: every object is created
// by script through a declared constructor, every method is called from script,
// and every property and field is read and written as ordinary script syntax.
// What that combination proves, which no unit case can:
//
//   * A class living two namespace levels down publishes its whole member
//     surface in one transaction, and the class table a script reaches its
//     members through is the same value the object reaches them through.
//   * A member-pointer target declared on a base class operates on the derived
//     object the call site supplied, and a virtual one dispatches on that
//     object rather than on the class the pointer was declared in.
//   * Every `PropertyPolicy` mode behaves as declared from script: an immediate
//     read, a computed read that never reuses anything, a lazy read that reuses
//     exactly one successful result per object, and every direction refusal.
//   * The lazy cache is driven entirely through script reads and writes: a
//     failing getter records nothing, a successful write invalidates, and a
//     generation change invalidates by mismatch.
//   * A setter that mutates its object and then throws leaves the mutation
//     readable through a field, because Luna promises virtual-machine rollback
//     after user code starts and nothing more.
//   * Two States that registered the same surface in different orders report
//     byte-for-byte identical diagnostics.

// clang-format off
#include <luna/luna.hpp>

#include "state/testing/test_hooks.hpp"
#include "state/userdata/member_dispatch.hpp"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;
using Luna::Detail::MemberDispatchStage;
using Luna::Detail::MemberDispatchStageText;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "class member integration check failed: " << Description << '\n';
}

// -- the model under test ---------------------------------------------------

// How often each declared target ran. A lazy getter that produced a value once
// must not run again while that value is still valid, and a refused access must
// not run its target at all, so these counters are the direct observation of
// both.
std::size_t LevelReads = 0;
std::size_t ChargeCalls = 0;
std::size_t SideReads = 0;
std::size_t SumCalls = 0;
std::size_t CombineCalls = 0;
std::size_t FaceCalls = 0;
std::size_t FailCalls = 0;
std::size_t CapacityReads = 0;
std::size_t CapacityWrites = 0;
std::size_t WeightReads = 0;
std::size_t ExpensiveReads = 0;
std::size_t MarkCalls = 0;

void ResetCounters() {
  LevelReads = 0;
  ChargeCalls = 0;
  SideReads = 0;
  SumCalls = 0;
  CombineCalls = 0;
  FaceCalls = 0;
  FailCalls = 0;
  CapacityReads = 0;
  CapacityWrites = 0;
  WeightReads = 0;
  ExpensiveReads = 0;
  MarkCalls = 0;
}

// The base class. Its members are declared here and registered on the derived
// class, so a base-declared member pointer and a virtual one both operate on
// the object the call site supplied.
struct Container {
  virtual ~Container() = default;

  int Energy = 3;

  [[nodiscard]] int Level() const {
    ++LevelReads;
    return Energy * 2;
  }

  void Charge(int Amount) {
    ++ChargeCalls;
    Energy += Amount;
  }

  [[nodiscard]] virtual int Sides() const {
    ++SideReads;
    return 0;
  }
};

// The registered class. Its own accessors carry every property mode and both
// field forms, and the failure of its lazy getter is a per-object policy the
// getter reads rather than anything keyed by storage - a script sets it through
// an ordinary writable field.
struct Crate final : Container {
  int Slots = 5;
  const int Serial = 42;
  std::string Label = "crate";
  bool Fragile = false;
  int Trace = 0;

  [[nodiscard]] int Sides() const override {
    ++SideReads;
    return 6;
  }

  [[nodiscard]] int Capacity() const {
    ++CapacityReads;
    return Slots * 2;
  }

  void SetCapacity(int Value) {
    ++CapacityWrites;
    Slots = Value;
  }

  [[nodiscard]] double Weight() {
    ++WeightReads;
    return 1.5;
  }

  [[nodiscard]] int Expensive() const {
    ++ExpensiveReads;
    if (Fragile)
      throw std::runtime_error("the expensive getter refused");
    return Slots + 100;
  }

  [[nodiscard]] int Combine(int First) const {
    ++CombineCalls;
    return Slots + First;
  }

  [[nodiscard]] int Combine(int First, int Second) const {
    ++CombineCalls;
    return Slots + First + Second;
  }

  // The setter that makes the side-effect boundary observable from script: it
  // changes the object first and only then refuses.
  void Mark(int Value) {
    ++MarkCalls;
    Trace = Value;
    throw std::runtime_error("the marking setter refused after mutating");
  }

  void Fail() {
    ++FailCalls;
    throw std::runtime_error("the crate refused");
  }

  [[nodiscard]] static int Faces() {
    ++FaceCalls;
    return 6;
  }
};

// One explicit wrapper member: it states the object it operates on through its
// first parameter rather than through a member pointer.
[[nodiscard]] int SumOf(const Crate &Source) {
  ++SumCalls;
  return Source.Slots + Source.Energy;
}

[[nodiscard]] Luna::StableTypeKey CrateKey() {
  return Luna::StableTypeKey("Studio.IntegrationCrate");
}

constexpr std::string_view CrateName = "Studio.Physics.Crate";

// The whole member surface of one class two namespace levels down, published as
// one plan.
[[nodiscard]] bool RegisterModel(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::NamespaceBuilder Physics = Studio.RegisterNamespace("Physics");
  Luna::ClassBuilder<Crate> Class =
      Physics.RegisterClass<Crate>("Crate", CrateKey());

  Luna::ClassBuilder<Crate> &WithConstructor = Class.Constructor<>();

  // Methods: two base-declared member pointers, one virtual member, one
  // explicit wrapper, one overload set selected without a macro, one throwing
  // member, and one static member.
  Luna::ClassBuilder<Crate> &WithMethods =
      WithConstructor.Method("Level", &Container::Level)
          .Method("Charge", &Container::Charge)
          .Method("Sides", &Container::Sides)
          .Method("Sum", &SumOf)
          .Method("Combine", Luna::Overload<int(int), Crate>(&Crate::Combine))
          .Method("Combine",
                  Luna::Overload<int(int, int), Crate>(&Crate::Combine))
          .Method("Fail", &Crate::Fail)
          .StaticMethod("Faces", &Crate::Faces);

  // Properties: both default forms plus every explicit policy.
  Luna::ClassBuilder<Crate> &WithProperties =
      WithMethods.Property("Capacity", &Crate::Capacity, &Crate::SetCapacity)
          .Property("Weight", &Crate::Weight)
          .Property("Reading", Luna::PropertyPolicy::ReadOnly(),
                    &Crate::Capacity)
          .Property("Hidden", Luna::PropertyPolicy::WriteOnly(),
                    &Crate::SetCapacity)
          .Property("Adjustable", Luna::PropertyPolicy::ReadWrite(),
                    &Crate::Capacity, &Crate::SetCapacity)
          .Property("Computed", Luna::PropertyPolicy::Computed(),
                    &Crate::Expensive)
          .Property("ComputedPair", Luna::PropertyPolicy::ComputedReadWrite(),
                    &Crate::Capacity, &Crate::SetCapacity)
          .Property("Cached", Luna::PropertyPolicy::Lazy(), &Crate::Expensive)
          .Property("CachedPair", Luna::PropertyPolicy::LazyReadWrite(),
                    &Crate::Capacity, &Crate::SetCapacity)
          .Property("Marked", Luna::PropertyPolicy::WriteOnly(), &Crate::Mark);

  // Fields: writable, const, explicitly read-only, and both explicit policy
  // factories.
  Luna::ClassBuilder<Crate> &WithFields =
      WithProperties
          .Field("Slots", &Crate::Slots, Luna::FieldPolicy::ReadWrite())
          .Field("Serial", &Crate::Serial)
          .Field("Label", &Crate::Label, Luna::FieldPolicy::ReadOnly())
          .Field("Fragile", &Crate::Fragile)
          .Field("Trace", &Crate::Trace, Luna::FieldPolicy::ReadOnly());

  Luna::ClassBuilder<Crate> &Described =
      WithFields.Documentation("One storage crate.")
          .Documentation("Cached", "The expensive value, produced once.")
          .Attribute("Cached", "cost", "high");
  static_cast<void>(Described.QualifiedName());
  return Studio.Commit().IsSuccess();
}

// Exactly the same surface, declared in a different order: the fields first,
// then the properties, then the methods, then the constructor, with the deeper
// namespace opened before a sibling one. A diagnostic that depends on
// registration order cannot survive this.
[[nodiscard]] bool RegisterPermutedModel(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::NamespaceBuilder Rendering = Studio.RegisterNamespace("Rendering");
  static_cast<void>(Rendering.RegisterConstant("Layers", 4));
  Luna::NamespaceBuilder Physics = Studio.RegisterNamespace("Physics");
  Luna::ClassBuilder<Crate> Class =
      Physics.RegisterClass<Crate>("Crate", CrateKey());

  Luna::ClassBuilder<Crate> &WithFields =
      Class.Field("Trace", &Crate::Trace, Luna::FieldPolicy::ReadOnly())
          .Field("Fragile", &Crate::Fragile)
          .Field("Label", &Crate::Label, Luna::FieldPolicy::ReadOnly())
          .Field("Serial", &Crate::Serial)
          .Field("Slots", &Crate::Slots, Luna::FieldPolicy::ReadWrite());

  Luna::ClassBuilder<Crate> &WithProperties =
      WithFields
          .Property("Marked", Luna::PropertyPolicy::WriteOnly(), &Crate::Mark)
          .Property("CachedPair", Luna::PropertyPolicy::LazyReadWrite(),
                    &Crate::Capacity, &Crate::SetCapacity)
          .Property("Cached", Luna::PropertyPolicy::Lazy(), &Crate::Expensive)
          .Property("ComputedPair", Luna::PropertyPolicy::ComputedReadWrite(),
                    &Crate::Capacity, &Crate::SetCapacity)
          .Property("Computed", Luna::PropertyPolicy::Computed(),
                    &Crate::Expensive)
          .Property("Adjustable", Luna::PropertyPolicy::ReadWrite(),
                    &Crate::Capacity, &Crate::SetCapacity)
          .Property("Hidden", Luna::PropertyPolicy::WriteOnly(),
                    &Crate::SetCapacity)
          .Property("Reading", Luna::PropertyPolicy::ReadOnly(),
                    &Crate::Capacity)
          .Property("Weight", &Crate::Weight)
          .Property("Capacity", &Crate::Capacity, &Crate::SetCapacity);

  Luna::ClassBuilder<Crate> &WithMethods =
      WithProperties.StaticMethod("Faces", &Crate::Faces)
          .Method("Fail", &Crate::Fail)
          .Method("Combine",
                  Luna::Overload<int(int, int), Crate>(&Crate::Combine))
          .Method("Combine", Luna::Overload<int(int), Crate>(&Crate::Combine))
          .Method("Sum", &SumOf)
          .Method("Sides", &Container::Sides)
          .Method("Charge", &Container::Charge)
          .Method("Level", &Container::Level);

  Luna::ClassBuilder<Crate> &WithConstructor = WithMethods.Constructor<>();
  static_cast<void>(WithConstructor.QualifiedName());
  return Studio.Commit().IsSuccess();
}

// -- helpers ----------------------------------------------------------------

[[nodiscard]] bool Succeeds(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return true;
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    std::cerr << "class member source failed: " << Diagnostic->Message()
              << '\n';
  return false;
}

[[nodiscard]] std::string Refusal(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return std::string();
  const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
  return Diagnostic ? Diagnostic->Message() : std::string("<no diagnostic>");
}

[[nodiscard]] bool Contains(std::string_view Text, std::string_view Needle) {
  return Text.find(Needle) != std::string_view::npos;
}

// One integer the script computed, read back without any conversion of its own.
[[nodiscard]] int ScriptResult(Luna::State &Owner, const std::string &Source) {
  if (!Succeeds(Owner, Source))
    return -1;
  const auto Observed = Hooks::ObserveIntegerGlobal(Owner, "Result");
  return Observed ? *Observed : -1;
}

// The callback checkpoint a refused access restores exactly.
[[nodiscard]] bool RestoredCheckpoint(const Luna::State &Owner) {
  const auto Observation = Hooks::ObserveLastCallbackStackRestoration(Owner);
  return Observation.has_value() &&
         Observation->EntryDepth == Observation->RestoredDepth &&
         Observation->ErrorDepth == Observation->RestoredDepth + 1;
}

// One refused member dispatch, described by the step that decided it and the
// half of the side-effect boundary that step belongs to.
[[nodiscard]] bool RefusedAt(const Luna::State &Owner,
                             MemberDispatchStage Stage,
                             std::string_view Boundary) {
  const auto Observed = Hooks::ObserveLastClassMemberDispatch(Owner);
  if (!Observed || !Observed->Attempted || Observed->Succeeded)
    return false;
  if (MemberDispatchStageText(Observed->Stage) !=
      MemberDispatchStageText(Stage))
    return false;
  if (Luna::Detail::MemberSideEffectBoundaryText(Observed->Boundary) !=
      Boundary)
    return false;
  return Observed->PublishedCount == 0 &&
         Observed->EntryDepth == Observed->RestoredDepth;
}

// The reflected record of one member candidate of one qualified name.
[[nodiscard]] Luna::ReflectionRecord
CandidateOf(const Luna::ReflectionSnapshot &Taken, Luna::SymbolKind Kind,
            std::string_view QualifiedName, std::size_t ParameterCount) {
  const Luna::ReflectionRecordRange Candidates = Taken.Symbols(Kind);
  for (std::size_t Index = 0; Index < Candidates.Size(); ++Index) {
    const Luna::ReflectionRecord Candidate = Candidates.At(Index);
    if (Candidate.QualifiedName() != QualifiedName)
      continue;
    if (Candidate.ParameterCount() == ParameterCount)
      return Candidate;
  }
  return Luna::ReflectionRecord();
}

[[nodiscard]] std::string MemberPath(std::string_view Member) {
  return std::string(CrateName) + "." + std::string(Member);
}

// -- the reflected surface --------------------------------------------------

void CheckTheWholeSurfaceIsReflectedAsDeclared() {
  ResetCounters();
  Luna::State Owner;
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);
  Check(
      Owner.IsReady() && RegisterModel(Owner),
      "one plan publishes the nested namespaces, the class, and every member");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "publishing the whole member surface restores the entry stack depth");
  Check(LevelReads == 0 && CapacityReads == 0 && ExpensiveReads == 0 &&
            FaceCalls == 0,
        "registering members runs no member at all");
  Check(Hooks::ClassMemberCount(Owner, CrateName) == 15,
        "every declared property and field is published with its class");

  const Luna::ReflectionSnapshot Snapshot = Owner.Bindings().Reflection();
  const Luna::ReflectionRecord Class = Snapshot.Find(CrateName);
  Check(Class.IsValid() && Class.Kind() == Luna::SymbolKind::Class &&
            Class.Documentation() == "One storage crate.",
        "the nested class is reflected under its canonical qualified name");

  // A base-declared const member pointer reflects a const receiver of the
  // registered class, not of the class it was declared in.
  const Luna::ReflectionRecord Level =
      CandidateOf(Snapshot, Luna::SymbolKind::Method, MemberPath("Level"), 0);
  Check(Level.IsValid() && Contains(Level.Signature(), "IntegrationCrate") &&
            Contains(Level.Signature(), "const"),
        "a base-declared const method reflects a const receiver of the "
        "registered class");
  const Luna::ReflectionRecord Charge =
      CandidateOf(Snapshot, Luna::SymbolKind::Method, MemberPath("Charge"), 1);
  Check(Charge.IsValid() && !Contains(Charge.Signature(), "const"),
        "a base-declared non-const method reflects a mutable receiver");

  const Luna::ReflectionRecord Faces = CandidateOf(
      Snapshot, Luna::SymbolKind::StaticMethod, MemberPath("Faces"), 0);
  Check(Faces.IsValid() && !Contains(Faces.Signature(), "IntegrationCrate"),
        "a static method reflects no receiver at all");
  Check(Snapshot.Find(MemberPath("Combine")).Kind() ==
                Luna::SymbolKind::OverloadSet &&
            Hooks::OverloadCandidateCount(Owner, MemberPath("Combine")) == 2,
        "two selected method overloads form one canonical overload set");

  // Every property mode reflects exactly the policy it declared.
  struct Expectation final {
    std::string_view Member;
    std::string_view Access;
    std::string_view Evaluation;
  };
  const Expectation Expected[] = {
      {"Capacity", "read-write", "immediate"},
      {"Weight", "read-only", "immediate"},
      {"Reading", "read-only", "immediate"},
      {"Hidden", "write-only", "immediate"},
      {"Adjustable", "read-write", "immediate"},
      {"Computed", "read-only", "computed"},
      {"ComputedPair", "read-write", "computed"},
      {"Cached", "read-only", "lazy"},
      {"CachedPair", "read-write", "lazy"},
      {"Marked", "write-only", "immediate"},
  };
  for (const Expectation &Declared : Expected) {
    const Luna::ReflectionRecord Property =
        Snapshot.Find(MemberPath(Declared.Member));
    Check(Property.IsValid() && Property.Kind() == Luna::SymbolKind::Property &&
              Property.AccessPolicy() == Declared.Access &&
              Property.Evaluation() == Declared.Evaluation,
          "a declared property mode reflects exactly its own policy");
  }
  Check(Snapshot.Find(MemberPath("Cached")).Documentation() ==
                "The expensive value, produced once." &&
            Snapshot.Find(MemberPath("Cached")).AttributeCount() == 1,
        "a documented and annotated lazy property reflects both");
  Check(!Snapshot.Find(MemberPath("Weight")).ReceiverPermitsConst(),
        "a property declared with a non-const getter needs a mutable "
        "receiver");

  // Fields reflect their declared direction and their copied ownership.
  const Luna::ReflectionRecord Slots = Snapshot.Find(MemberPath("Slots"));
  const Luna::ReflectionRecord Serial = Snapshot.Find(MemberPath("Serial"));
  const Luna::ReflectionRecord Label = Snapshot.Find(MemberPath("Label"));
  Check(Slots.Kind() == Luna::SymbolKind::Field && Slots.IsReadable() &&
            Slots.IsWritable() && Slots.MemberOwnershipPolicy() == "copied",
        "a writable field reflects both directions and copied ownership");
  Check(Serial.IsValid() && Serial.IsReadable() && !Serial.IsWritable(),
        "a const data member is a read-only field");
  Check(Label.IsValid() && Label.IsReadable() && !Label.IsWritable(),
        "an explicitly read-only field reflects only its read direction");
  Check(Hooks::LazyMemberCacheEntryCount(Owner) == 0,
        "registration records no cached value at all");
}

// -- methods through the virtual machine ------------------------------------

void CheckMethodsRunOnTheSuppliedObject() {
  ResetCounters();
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);
  Check(Succeeds(Owner, "Value = Studio.Physics.Crate.New()"),
        "a script constructs one object of the nested class");

  // The three spellings of one member call are one call, and the member value
  // reached through the object is the member value the class declares.
  Check(ScriptResult(Owner, "Result = Value:Level()") == 6,
        "a base-declared const method reads the object it was called on");
  Check(ScriptResult(Owner, "Result = Value.Level(Value)") == 6,
        "a dot call with an explicit receiver is the same call");
  Check(ScriptResult(Owner, "Result = Studio.Physics.Crate.Level(Value)") == 6,
        "the class-scope spelling of the same member is the same call");
  Check(LevelReads == 3, "each of the three spellings ran the member once");
  Check(ScriptResult(Owner,
                     "Result = 0\n"
                     "if Value.Level == Studio.Physics.Crate.Level then Result "
                     "= 1 end") == 1,
        "an instance reaches exactly the member value its class declares");

  // A virtual member declared through the base dispatches on the object the
  // call site supplied.
  Check(ScriptResult(Owner, "Result = Value:Sides()") == 6,
        "a virtual member dispatches through the supplied object");
  Check(SideReads == 1, "the override ran exactly once");

  // A non-const base-declared method mutates exactly its own object, and the
  // change is visible through the const-declared reader.
  Check(ScriptResult(Owner, "Value:Charge(4)\nResult = Value:Level()") == 14,
        "a non-const base-declared method mutates the supplied object");
  Check(ChargeCalls == 1, "the mutating member ran exactly once");

  // An explicit wrapper and a selected overload set are ordinary members.
  Check(ScriptResult(Owner, "Result = Value:Sum()") == 12,
        "an explicit wrapper member reads the object through its first "
        "parameter");
  Check(ScriptResult(Owner, "Result = Value:Combine(1)") == 6 &&
            ScriptResult(Owner, "Result = Value:Combine(1, 2)") == 8,
        "both selected overloads of one member name are reachable");
  Check(CombineCalls == 2, "each overload ran exactly once");
  Check(ScriptResult(Owner, "Result = Studio.Physics.Crate.Faces()") == 6 &&
            FaceCalls == 1,
        "a static method is called without any instance at all");

  // A dot call without a receiver fails receiver validation before any
  // ordinary-argument decision, and an ordinary argument keeps its own
  // one-based position.
  const std::string Missing =
      Refusal(Owner, "return Studio.Physics.Crate.Charge(4)");
  Check(Contains(Missing, "receiver") && !Contains(Missing, "argument 1"),
        "a dot call without a receiver is refused as its receiver");
  const std::string Argument = Refusal(Owner, "return Value:Charge('x')");
  Check(Contains(Argument, "Member 'Studio.Physics.Crate.Charge' argument 1"),
        "an ordinary argument of a member names the member and its position");
  Check(ChargeCalls == 1, "no refused call reached native code");
  Check(RestoredCheckpoint(Owner),
        "a refused member call restores the callback checkpoint exactly");

  // A throwing member is translated with the foundation's own wording, and the
  // State keeps calling members afterwards.
  const std::string Thrown = Refusal(Owner, "return Value:Fail()");
  Check(Contains(Thrown, "the crate refused") && FailCalls == 1,
        "a throwing member is translated and ran exactly once");
  Check(Owner.IsReady() && ScriptResult(Owner, "Result = Value:Level()") == 14,
        "the State keeps calling members after every refusal");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every member call and refusal restores the root stack depth");
}

// -- every property mode and both field forms -------------------------------

void CheckEveryMemberModeBehavesAsDeclared() {
  ResetCounters();
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);
  Check(Succeeds(Owner, "Value = Studio.Physics.Crate.New()"),
        "a script constructs one object of the nested class");

  // Immediate reads and writes, through a property and through a field.
  Check(ScriptResult(Owner, "Result = Value.Capacity") == 10 &&
            CapacityReads == 1,
        "an immediate read-write property reads through its declared getter");
  Check(Succeeds(Owner, "Value.Capacity = 7") && CapacityWrites == 1 &&
            ScriptResult(Owner, "Result = Value.Slots") == 7,
        "an immediate property write reaches the object its field reads");
  Check(Succeeds(Owner, "Value.Slots = 9") &&
            ScriptResult(Owner, "Result = Value.Adjustable") == 18,
        "a writable field and an explicitly read-write property agree on the "
        "object");
  Check(ScriptResult(Owner, "Result = Value.Reading") == 18,
        "an explicitly read-only property reads its declared getter");
  Check(Succeeds(Owner, "Value.Hidden = 3") &&
            ScriptResult(Owner, "Result = Value.Slots") == 3,
        "a write-only property writes through its declared setter");

  // A non-const getter reads through the mutable view a script value carries.
  Check(Succeeds(Owner, "Result = Value.Weight") && WeightReads == 1,
        "a property declared with a non-const getter reads through a mutable "
        "view");

  // A computed property runs its getter on every read and never reuses one.
  const std::size_t ComputedBefore = ExpensiveReads;
  Check(ScriptResult(Owner, "Result = Value.Computed") == 103 &&
            ScriptResult(Owner, "Result = Value.Computed") == 103 &&
            ExpensiveReads == ComputedBefore + 2,
        "a computed property runs its getter on every read");
  Check(Hooks::LazyMemberCacheEntryCount(Owner) == 0,
        "a computed property records nothing at all");
  const std::size_t PairReads = CapacityReads;
  Check(Succeeds(Owner, "Value.ComputedPair = 4") &&
            ScriptResult(Owner, "Result = Value.ComputedPair") == 8 &&
            CapacityReads == PairReads + 1,
        "a computed read-write property writes and recomputes");

  // Directions are decided before the target: a read-only field, a const data
  // member, and a write-only property all refuse before any accessor runs.
  const std::size_t WritesBefore = CapacityWrites;
  const std::string Constant = Refusal(Owner, "Value.Serial = 1");
  Check(Contains(Constant, "Member 'Studio.Physics.Crate.Serial' permits no "
                           "write."),
        "a const data member permits no write and names itself");
  Check(RefusedAt(Owner, MemberDispatchStage::Direction, "before_user_code"),
        "a const field is refused at the direction step");
  const std::string ReadOnlyField = Refusal(Owner, "Value.Label = 'x'");
  Check(Contains(ReadOnlyField, "permits no write."),
        "an explicitly read-only field permits no write");
  const std::string WriteOnly = Refusal(Owner, "Result = Value.Hidden");
  Check(Contains(WriteOnly, "Member 'Studio.Physics.Crate.Hidden' permits no "
                            "read."),
        "a write-only property permits no read");
  Check(CapacityWrites == WritesBefore,
        "no refused direction ran a declared accessor");
  Check(ScriptResult(Owner, "Result = Value.Serial") == 42 &&
            Succeeds(Owner, "Result = Value.Label"),
        "both read-only forms still read");

  // A value the declared type does not accept is refused before the setter, and
  // a name the class never declared is refused before anything at all.
  const std::string Mistyped = Refusal(Owner, "Value.Slots = 'nine'");
  Check(Contains(Mistyped, "Member 'Studio.Physics.Crate.Slots' value") &&
            Contains(Mistyped, "expected signed 32-bit integer"),
        "a field write of another canonical type names the member and the "
        "expectation");
  Check(RefusedAt(Owner, MemberDispatchStage::Value, "before_user_code"),
        "a refused value is reported before user code");
  const std::string Unknown = Refusal(Owner, "Result = Value.Missing");
  Check(Contains(Unknown, "Class 'Studio.Physics.Crate' declares no member "
                          "'Missing'."),
        "a member the class never declared names the class");
  Check(RestoredCheckpoint(Owner),
        "the last refused access restored the callback checkpoint exactly");

  // A setter that mutates its object and then throws: the virtual machine is
  // restored and the exception is translated, and the mutation stays - read
  // back through a field, entirely from script.
  Check(ScriptResult(Owner, "Result = Value.Trace") == 0,
        "the object starts unmarked");
  const std::string Marked = Refusal(Owner, "Value.Marked = 7");
  Check(
      Contains(Marked, "member 'Studio.Physics.Crate.Marked' setter threw:") &&
          MarkCalls == 1,
      "a throwing setter names the member and ran exactly once");
  Check(RefusedAt(Owner, MemberDispatchStage::Target, "after_user_code"),
        "a throwing setter is reported after user code and publishes nothing");
  Check(ScriptResult(Owner, "Result = Value.Trace") == 7,
        "the native side effect the consumer's own code performed survives, "
        "because Luna promises no native rollback after user code starts");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every member access and refusal restores the root stack depth");
  Check(Owner.IsReady() && ScriptResult(Owner, "Result = Value.Capacity") == 8,
        "the State keeps reading members after every refusal");
}

// -- the lazy cache, driven entirely from script ----------------------------

void CheckLazyValuesAreCachedAndInvalidatedFromScript() {
  ResetCounters();
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);
  Check(Succeeds(Owner, "First = Studio.Physics.Crate.New()\n"
                        "Second = Studio.Physics.Crate.New()\n"),
        "a script constructs two objects of the class");
  Check(Hooks::LazyMemberCacheEntryCount(Owner) == 0,
        "a constructed value starts with no cached member value");

  // The first read produces the value; the second reuses it.
  Check(ScriptResult(Owner, "Result = First.Cached") == 105 &&
            ExpensiveReads == 1,
        "the first read of a lazy property runs its declared getter");
  Check(Hooks::LazyMemberCacheEntryCount(Owner) == 1 &&
            Hooks::LazyMemberCacheNodeCount(Owner) == 1,
        "one successful lazy read records exactly one value of one object");
  Check(ScriptResult(Owner, "Result = First.Cached") == 105 &&
            ExpensiveReads == 1,
        "a second read of a lazy property reuses the recorded value");
  const auto Reused = Hooks::ObserveLastClassMemberDispatch(Owner);
  Check(Reused && Reused->Succeeded && Reused->ServedFromCache,
        "the reused read is served from the cache rather than from the getter");

  // Another object of the same class has its own value.
  Check(ScriptResult(Owner, "Result = Second.Cached") == 105 &&
            ExpensiveReads == 2 && Hooks::LazyMemberCacheNodeCount(Owner) == 2,
        "a lazy value is recorded per object, not per member");

  // A successful write invalidates the values of exactly its own object.
  Check(Succeeds(Owner, "First.Slots = 6"), "a script writes a field");
  Check(Hooks::LazyMemberCacheEntryCount(Owner) == 1,
        "a successful field write invalidates the values of its own object");
  Check(ScriptResult(Owner, "Result = First.Cached") == 106 &&
            ExpensiveReads == 3,
        "the read after an invalidating write runs the getter again");
  Check(ScriptResult(Owner, "Result = Second.Cached") == 105 &&
            ExpensiveReads == 3,
        "the other object's recorded value was never invalidated");

  // A successful property setter invalidates the same way, and a lazy
  // read-write property is reused between writes.
  Check(ScriptResult(Owner, "Result = First.CachedPair") == 12 &&
            ScriptResult(Owner, "Result = First.CachedPair") == 12 &&
            CapacityReads == 1,
        "a lazy read-write property reuses its recorded value");
  Check(Succeeds(Owner, "First.CachedPair = 10") &&
            ScriptResult(Owner, "Result = First.CachedPair") == 20 &&
            CapacityReads == 2,
        "a successful setter invalidates the value its getter recorded");

  // A refused write invalidates nothing, because nothing about the object
  // changed.
  const std::size_t EntriesBefore = Hooks::LazyMemberCacheEntryCount(Owner);
  Check(!Refusal(Owner, "First.Slots = 'six'").empty() &&
            Hooks::LazyMemberCacheEntryCount(Owner) == EntriesBefore,
        "a refused write invalidates nothing at all");

  // A getter that fails records nothing. The failure is a per-object policy the
  // getter reads, and the script sets it through an ordinary writable field.
  const std::size_t LiveBeforeWrite =
      Hooks::LiveLazyMemberCacheEntryCount(Owner);
  Check(Succeeds(Owner, "Second.Fragile = true"),
        "the script makes the second object's getter refuse");
  Check(Hooks::LiveLazyMemberCacheEntryCount(Owner) == LiveBeforeWrite - 1,
        "that write invalidated the failing object's earlier recorded value");

  const std::size_t LiveBeforeFailure =
      Hooks::LiveLazyMemberCacheEntryCount(Owner);
  const std::size_t ReadsBeforeFailure = ExpensiveReads;
  const std::string Failed = Refusal(Owner, "Result = Second.Cached");
  Check(
      Contains(Failed, "member 'Studio.Physics.Crate.Cached' getter threw:") &&
          Contains(Failed, "the expensive getter refused"),
      "a failing lazy getter is translated and names its member");
  Check(RefusedAt(Owner, MemberDispatchStage::Target, "after_user_code"),
        "a failing lazy getter publishes nothing");
  Check(ExpensiveReads == ReadsBeforeFailure + 1 &&
            Hooks::LiveLazyMemberCacheEntryCount(Owner) == LiveBeforeFailure,
        "a failed lazy getter records nothing at all");
  Check(!Refusal(Owner, "Result = Second.Cached").empty() &&
            ExpensiveReads == ReadsBeforeFailure + 2,
        "a failed lazy getter is retried rather than reused");
  Check(Succeeds(Owner, "Second.Fragile = false") &&
            ScriptResult(Owner, "Result = Second.Cached") == 105,
        "the object's own policy decides when its getter succeeds again");

  // A generation change invalidates every earlier entry by mismatch, and the
  // next script read records its own value in place of the stale one.
  Check(Hooks::LiveLazyMemberCacheEntryCount(Owner) > 0,
        "there are live recorded values before the generation changes");
  Check(Hooks::AdvanceLifecycleGeneration(Owner),
        "the registered model is replaced by a later generation");
  Check(Hooks::LiveLazyMemberCacheEntryCount(Owner) == 0,
        "a generation change invalidates every earlier value by mismatch");
  const Luna::Detail::LazyCacheCounters Before =
      Hooks::LazyMemberCacheCounters(Owner);
  Check(ScriptResult(Owner, "Result = First.Cached") == 110,
        "a read under the later generation still produces its value");
  const Luna::Detail::LazyCacheCounters After =
      Hooks::LazyMemberCacheCounters(Owner);
  Check(After.GenerationMismatch == Before.GenerationMismatch + 1,
        "the read under the later generation missed by mismatch");
  Check(Hooks::LiveLazyMemberCacheEntryCount(Owner) == 1,
        "the later generation records its own value in place of the stale one");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every lazy read, write, and refusal restores the root stack depth");
  Check(Owner.IsReady(),
        "the State remains usable after every lazy failure and invalidation");
}

// -- deterministic diagnostics across registration orders -------------------

void CheckDiagnosticsNeverDependOnRegistrationOrder() {
  ResetCounters();
  Luna::State First;
  Luna::State Second;
  Check(First.IsReady() && RegisterModel(First),
        "the first State registers the surface in declaration order");
  Check(Second.IsReady() && RegisterPermutedModel(Second),
        "the second State registers the same surface in another order");
  Check(Hooks::ClassMemberCount(First, CrateName) ==
            Hooks::ClassMemberCount(Second, CrateName),
        "both States published the same member surface");

  const std::string Sources[] = {
      "Value = Studio.Physics.Crate.New()\nreturn Value.Missing",
      "Value = Studio.Physics.Crate.New()\nValue.Serial = 1",
      "Value = Studio.Physics.Crate.New()\nreturn Value.Hidden",
      "Value = Studio.Physics.Crate.New()\nValue.Slots = 'nine'",
      "Value = Studio.Physics.Crate.New()\nreturn Value:Charge('x')",
      "Value = Studio.Physics.Crate.New()\nreturn Value:Fail()",
      "return Studio.Physics.Crate.Charge(4)",
      "Value = Studio.Physics.Crate.New()\nreturn Value.Capacity()",
  };
  for (const std::string &Source : Sources) {
    const std::string Reported = Refusal(First, Source);
    Check(!Reported.empty(), "every listed source is refused");
    Check(Refusal(Second, Source) == Reported,
          "one member failure family reports one identical message whichever "
          "order the surface was registered in");
    Check(Refusal(First, Source) == Reported,
          "the same failure reports the same message however often it happens");
  }

  // Both States keep working afterwards, and the successful surface agrees too.
  Check(ScriptResult(First, "Value = Studio.Physics.Crate.New()\n"
                            "Value.Capacity = 4\n"
                            "Result = Value:Sides() + Value.Slots") == 10,
        "the first State keeps using its member surface");
  Check(ScriptResult(Second, "Value = Studio.Physics.Crate.New()\n"
                             "Value.Capacity = 4\n"
                             "Result = Value:Sides() + Value.Slots") == 10,
        "the permuted State behaves identically on success too");
  Check(ScriptResult(Second, "Result = Studio.Rendering.Layers") == 4,
        "the permuted plan's sibling namespace published as well");
}

} // namespace

int RunClassMemberIntegrationTests();

int RunClassMemberIntegrationTests() {
  FailureCount = 0;
  CheckTheWholeSurfaceIsReflectedAsDeclared();
  CheckMethodsRunOnTheSuppliedObject();
  CheckEveryMemberModeBehavesAsDeclared();
  CheckLazyValuesAreCachedAndInvalidatedFromScript();
  CheckDiagnosticsNeverDependOnRegistrationOrder();
  return FailureCount == 0 ? 0 : 1;
}
