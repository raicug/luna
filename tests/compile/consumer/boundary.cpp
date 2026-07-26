// clang-format off
#include <luna/luna.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace {

// The umbrella header alone exposes every consumer-facing type.
static_assert(
    std::is_same_v<Luna::Value, std::variant<bool, int, double, std::string>>,
    "Luna::Value must be reachable through <luna/luna.hpp> alone.");
static_assert(
    Luna::SupportedCallable<int (*)(int)>,
    "Callable support traits must be reachable through the umbrella.");
static_assert(static_cast<int>(Luna::ValueKind::Integer) >= 0,
              "Luna::ValueKind must be reachable through the umbrella.");
static_assert(static_cast<int>(Luna::ErrorCategory::Internal) >= 0,
              "Luna::ErrorCategory must be reachable through the umbrella.");

// State remains the sole public virtual-machine owner with its established
// operations, result types, and move behavior.
static_assert(!std::is_copy_constructible_v<Luna::State>,
              "Luna::State must remain non-copyable.");
static_assert(std::is_nothrow_move_constructible_v<Luna::State>,
              "Luna::State must remain nothrow move constructible.");
static_assert(std::is_nothrow_move_assignable_v<Luna::State>,
              "Luna::State must remain nothrow move assignable.");
static_assert(
    std::is_same_v<decltype(std::declval<Luna::State &>().IsReady()), bool>,
    "Luna::State::IsReady must keep returning bool.");
static_assert(std::is_same_v<decltype(std::declval<Luna::State &>().Bindings()),
                             Luna::BindingRegistry>,
              "Luna::State::Bindings must keep returning BindingRegistry.");
static_assert(std::is_same_v<decltype(std::declval<Luna::State &>().Execute(
                                 std::declval<std::string_view>())),
                             Luna::ExecutionResult>,
              "Luna::State::Execute must keep returning ExecutionResult.");
static_assert(
    std::is_same_v<decltype(std::declval<Luna::BindingRegistry &>().Freeze()),
                   Luna::RegistrationResult>,
    "BindingRegistry::Freeze must return RegistrationResult.");
static_assert(
    std::is_same_v<decltype(std::declval<Luna::BindingRegistry &>().Register(
                       std::declval<std::string_view>(),
                       std::declval<int (&)(int)>())),
                   Luna::RegistrationResult>,
    "Root-scope Register must keep returning RegistrationResult.");
static_assert(
    std::is_same_v<
        decltype(std::declval<Luna::BindingRegistry &>().RegisterFunction(
            std::declval<std::string_view>(), std::declval<int (&)(int)>())),
        Luna::RegistrationResult>,
    "Root-scope RegisterFunction must keep returning RegistrationResult.");
static_assert(
    std::is_same_v<
        decltype(std::declval<Luna::NamespaceBuilder &>().RegisterFunction(
            std::declval<std::string_view>(), std::declval<int (&)(int)>())),
        Luna::NamespaceBuilder &>,
    "A nested RegisterFunction must keep returning the same builder.");

// Stable identities and owning reflection snapshots are consumer-facing values
// reachable through the umbrella header alone: no Luau declaration, macro, or
// link is ever needed to store, compare, format, query, or retain them.
static_assert(!std::is_same_v<Luna::TypeId, Luna::SymbolId>,
              "Type and symbol identity must stay distinct consumer types.");
static_assert(std::is_trivially_copyable_v<Luna::TypeId> &&
                  std::is_trivially_copyable_v<Luna::SymbolId>,
              "Identities must remain plain storable values.");
static_assert(std::is_copy_constructible_v<Luna::ReflectionSnapshot> &&
                  std::is_default_constructible_v<Luna::ReflectionSnapshot>,
              "A reflection snapshot must remain a copyable owning value.");
static_assert(
    std::is_same_v<
        decltype(std::declval<const Luna::BindingRegistry &>().Reflection()),
        Luna::ReflectionSnapshot>,
    "Bindings().Reflection() must keep returning an owning snapshot.");
static_assert(
    std::is_same_v<decltype(std::declval<const Luna::TypeId &>().ToString()),
                   std::string>,
    "Identity formatting must produce a std::string.");
static_assert(std::is_same_v<decltype(Luna::SymbolId::Parse(
                                 std::declval<std::string_view>())),
                             std::optional<Luna::SymbolId>>,
              "Identity parsing must round-trip through std::optional.");

} // namespace

void VerifyConsumerBoundaryCompiles() {
  Luna::State State;
  [[maybe_unused]] const bool Ready = State.IsReady();
  [[maybe_unused]] const Luna::RegistrationResult Registration =
      State.Bindings().Register("Increment",
                                [](int Value) { return Value + 1; });
  [[maybe_unused]] const Luna::ExecutionResult Execution =
      State.Execute("return Increment(41)");
  Luna::State Moved = std::move(State);
  static_cast<void>(Moved.IsReady());
}

namespace {

// One overloaded consumer function, selected without a macro and without a cast
// through a virtual-machine type.
[[nodiscard]] int ConsumerMeasure(int Value) { return Value; }
[[nodiscard]] int ConsumerMeasure(int Value, int Scale) {
  return Value * Scale;
}

struct ConsumerScaling final {
  [[nodiscard]] double operator()(double Value) const { return Value; }
  [[nodiscard]] int operator()(int Value) const { return Value; }
};

static_assert(Luna::SupportedCallable<
                  decltype(Luna::Overload<int(int)>(&ConsumerMeasure))>,
              "An overload selection must stay a supported callable.");
static_assert(Luna::ExactOverloadTarget<ConsumerScaling, double(double)>,
              "A callable object invocable with the declared signature must "
              "satisfy the overload-target concept.");

} // namespace

// Explicit function registration at the root scope and inside a namespace,
// including overload selection, needs nothing beyond Luna and C++20.
void VerifyFunctionRegistrationConsumerBoundaryCompiles() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  [[maybe_unused]] const Luna::RegistrationResult Explicit =
      Registry.RegisterFunction("Measure",
                                Luna::Overload<int(int)>(&ConsumerMeasure));
  [[maybe_unused]] const Luna::RegistrationResult Alias = Registry.Register(
      "Scale", Luna::Overload<int(int, int)>(&ConsumerMeasure));
  [[maybe_unused]] const Luna::RegistrationResult Selected =
      Registry.RegisterFunction(
          "Half", Luna::Overload<double(double)>(ConsumerScaling()));

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::NamespaceBuilder &Staged = Studio.RegisterFunction(
      "Measure", Luna::Overload<int(int)>(&ConsumerMeasure));
  static_cast<void>(Staged.RegisterFunction(
      "Add", [](int Left, int Right) { return Left + Right; }));
  [[maybe_unused]] const Luna::RegistrationResult Published = Studio.Commit();
}

// Storing, comparing, formatting, and keying identities needs nothing beyond
// Luna and the standard library.
void VerifyIdentityConsumerBoundaryCompiles() {
  std::vector<Luna::TypeId> StoredTypes;
  std::map<Luna::SymbolId, std::string> OrderedSymbols;
  std::unordered_map<Luna::TypeId, std::string, Luna::CanonicalHash> ByType;

  const Luna::TypeId Unresolved;
  const std::string Text = Unresolved.ToString();
  const std::optional<Luna::TypeId> Parsed = Luna::TypeId::Parse(Text);
  StoredTypes.push_back(Parsed ? *Parsed : Unresolved);
  ByType.emplace(StoredTypes.front(), Text);

  const Luna::SymbolId Symbol;
  const Luna::SymbolId Other =
      Luna::SymbolId::Parse(std::string(Luna::SymbolId::TextLength, 'f'))
          .value_or(Symbol);
  OrderedSymbols.emplace(Symbol, Symbol.ToString());
  OrderedSymbols.emplace(Other, Other.ToString());
  [[maybe_unused]] const bool Equal = StoredTypes.front() == Unresolved;
  [[maybe_unused]] const bool Ordered = Symbol < Other && Other > Symbol;
  [[maybe_unused]] const std::size_t Hashed = Symbol.Hash() + Other.Hash();
  [[maybe_unused]] const bool Valid = Other.IsValid() && !Symbol.IsValid();

  // Canonical keys and descriptors are equally Luau-free consumer values.
  const Luna::StableTypeKey Key("studio.ui.Widget");
  [[maybe_unused]] const bool KeyIsValid =
      Key.IsValid() && Luna::StableTypeKey::IsValidText("studio.ui.Widget");
  [[maybe_unused]] const bool Reserved =
      Luna::StableTypeKey::Classify("luna.void") ==
      Luna::StableTypeKeyStatus::ReservedPrefix;
  const Luna::TypeDescriptor Class = Luna::TypeDescriptor::ForClass(Key);
  const Luna::TypeDescriptor Number =
      Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Double);
  [[maybe_unused]] const bool DescriptorsDiffer =
      Class != Number && Class.Kind() == Luna::TypeKind::Class &&
      !Luna::TypeKindText(Number.Kind()).empty();
}

// Querying and retaining a snapshot needs no Luau declaration either, and the
// snapshot outlives the State that produced it.
void VerifySnapshotConsumerBoundaryCompiles() {
  Luna::ReflectionSnapshot Retained;
  {
    Luna::State Owner;
    [[maybe_unused]] const Luna::RegistrationResult Registration =
        Owner.Bindings().Register("Double",
                                  [](int Value) { return Value * 2; });
    Retained = Owner.Bindings().Reflection();

    const Luna::ReflectionRecord ById = Retained.Find(Luna::SymbolId());
    const Luna::ReflectionRecord ByName = Retained.Find("Studio.Double");
    [[maybe_unused]] const bool Queried =
        ById.IsValid() || ByName.IsValid() ||
        Retained.FindType(Luna::TypeId()).IsValid();
  }

  // Every enumeration, record, string, and nested view still reads the
  // captured generation after the originating State is gone.
  const Luna::ReflectionRecordRange Symbols = Retained.Symbols();
  const Luna::ReflectionRecordRange Scoped =
      Retained.Symbols(Luna::ScopeId::Root());
  const Luna::ReflectionRecordRange OfKind =
      Retained.Symbols(Luna::SymbolKind::FunctionCandidate);
  const Luna::TypeRecordRange Types = Retained.Types();
  const Luna::ModuleRecordRange Modules = Retained.Modules();
  [[maybe_unused]] const std::size_t Counted = Symbols.Size() + Scoped.Size() +
                                               OfKind.Size() + Types.Size() +
                                               Modules.Size() + Retained.Size();

  const Luna::ReflectionRecord First = Symbols.At(0);
  [[maybe_unused]] const std::string Formatted = First.Id().ToString();
  [[maybe_unused]] const bool Described =
      First.Kind() == Luna::SymbolKind::Namespace && First.Scope().IsRoot() &&
      First.Returns() == Luna::ReturnShape::Zero &&
      First.Parameter(0).Type() == Luna::TypeId() &&
      First.Return(0).Descriptor() == Luna::TypeDescriptor::Unsupported() &&
      !First.Attribute(0).IsValid() && !First.Relation(0).IsValid() &&
      !First.HasModule() && First.QualifiedName().empty() &&
      First.Signature().empty() && Types.At(0).Name().empty() &&
      Modules.At(0).Version().empty();

  // A snapshot is a value, so copying and reassigning it is ordinary code.
  Luna::ReflectionSnapshot Copied = Retained;
  Retained = Luna::ReflectionSnapshot();
  [[maybe_unused]] const bool StillReadable =
      Copied.Generation() == 0 || Copied.IsEmpty();
}

namespace {

// Consumer enumerations a hierarchical registration exposes: one scoped, one
// unscoped, and one used as a flag set.
enum class ConsumerAlignment { Left = 0, Center = 1, Right = 2 };
enum class ConsumerAccess : int { None = 0, Read = 1, Write = 2 };
enum ConsumerLegacy { ConsumerLegacyFirst = 1, ConsumerLegacySecond = 2 };

// Every hierarchical builder operation is reachable through the umbrella header
// alone and keeps its Luna-owned result type.
static_assert(
    std::is_same_v<
        decltype(std::declval<Luna::BindingRegistry &>().RegisterNamespace(
            std::declval<std::string_view>())),
        Luna::NamespaceBuilder>,
    "Root-scope RegisterNamespace must keep returning a NamespaceBuilder.");
static_assert(
    std::is_same_v<
        decltype(std::declval<Luna::NamespaceBuilder &>().RegisterNamespace(
            std::declval<std::string_view>())),
        Luna::NamespaceBuilder>,
    "A nested RegisterNamespace must keep returning a NamespaceBuilder.");
static_assert(
    std::is_same_v<
        decltype(std::declval<Luna::BindingRegistry &>().RegisterConstant(
            std::declval<std::string_view>(), 1)),
        Luna::RegistrationResult>,
    "Root-scope RegisterConstant must keep returning RegistrationResult.");
static_assert(
    std::is_same_v<decltype(std::declval<Luna::BindingRegistry &>()
                                .RegisterEnum<ConsumerAlignment>(
                                    std::declval<std::string_view>(),
                                    std::declval<Luna::StableTypeKey>())),
                   Luna::EnumBuilder<ConsumerAlignment>>,
    "Root-scope RegisterEnum must keep returning an EnumBuilder.");
static_assert(
    std::is_same_v<decltype(std::declval<Luna::NamespaceBuilder &>().Commit()),
                   Luna::RegistrationResult>,
    "Committing a plan must keep returning RegistrationResult.");
static_assert(
    std::is_same_v<
        decltype(std::declval<Luna::EnumBuilder<ConsumerAlignment> &>()
                     .QualifiedName()),
        std::string_view>,
    "A builder's qualified name must stay an ordinary consumer string view.");
static_assert(!std::is_copy_constructible_v<Luna::NamespaceBuilder> &&
                  std::is_move_constructible_v<Luna::NamespaceBuilder>,
              "A namespace builder must stay a move-only owning handle.");
static_assert(
    !std::is_copy_constructible_v<Luna::EnumBuilder<ConsumerAlignment>> &&
        std::is_move_constructible_v<Luna::EnumBuilder<ConsumerAlignment>>,
    "An enum builder must stay a move-only owning handle.");
static_assert(Luna::ModuleConfiguration<void (&)(Luna::NamespaceBuilder &)>,
              "A plain consumer callable must satisfy the module "
              "configuration concept.");

} // namespace

// One consumer registering a whole hierarchy: nested namespaces, constants,
// scoped and unscoped enumerations, aliases, bitflags, documentation,
// attributes, and modules. Nothing here needs a Luau declaration, an extra
// include path, a link dependency, or a registration macro.
void VerifyHierarchicalBuilderConsumerBoundaryCompiles() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  [[maybe_unused]] const Luna::RegistrationResult RootConstant =
      Registry.RegisterConstant("Version", 7);
  [[maybe_unused]] const Luna::RegistrationResult TypedRootConstant =
      Registry.RegisterConstant("DefaultAlignment", ConsumerAlignment::Center,
                                Luna::StableTypeKey("consumer.Alignment"));

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  [[maybe_unused]] const std::string_view Qualified = Studio.QualifiedName();
  Luna::NamespaceBuilder &Described = Studio.RegisterConstant("Name", "Luna");
  Luna::NamespaceBuilder &Typed =
      Described.RegisterConstant("Fallback", ConsumerAlignment::Left,
                                 Luna::StableTypeKey("consumer.Alignment"));
  static_cast<void>(Typed.QualifiedName());

  Luna::NamespaceBuilder Nested = Studio.RegisterNamespace("Ui");
  Luna::NamespaceBuilder Deeper = Nested.RegisterNamespace("Layout");
  static_cast<void>(Deeper.RegisterConstant("Columns", 12));

  Luna::EnumBuilder<ConsumerAlignment> Alignments =
      Nested.RegisterEnum<ConsumerAlignment>(
          "Alignment", Luna::StableTypeKey("consumer.Alignment"));
  Luna::EnumBuilder<ConsumerAlignment> &StagedAlignments =
      Alignments.Value("Left", ConsumerAlignment::Left)
          .Value("Center", static_cast<std::int64_t>(1))
          .Alias("Start", "Left")
          .Documentation("Horizontal alignment.")
          .Documentation("Center", "Centered content.")
          .Attribute("Group", "Layout")
          .Attribute("Center", "Default", "true")
          .Example("print(Studio.Ui.Alignment.Center)")
          .Example("Center", "Studio.Ui.Alignment.Center");
  static_cast<void>(StagedAlignments.QualifiedName());

  // The documentation surface of a namespace and of the declarations staged
  // inside it, which documentation and declaration generation read.
  Luna::NamespaceBuilder &Annotated =
      Nested.Documentation("The user-interface surface.")
          .Attribute("Stability", "stable")
          .Example("local Layout = Studio.Ui.Layout")
          .Documentation("Alignment", "Horizontal alignment.")
          .Attribute("Alignment", "Group", "Layout")
          .Example("Alignment", "Studio.Ui.Alignment.Left");
  static_cast<void>(Annotated.QualifiedName());

  Luna::EnumBuilder<ConsumerAccess> Permissions =
      Nested.RegisterEnum<ConsumerAccess>(
          "Access", Luna::StableTypeKey("consumer.Access"));
  Luna::EnumBuilder<ConsumerAccess> &StagedPermissions =
      Permissions.Value("Read", ConsumerAccess::Read)
          .Value("Write", ConsumerAccess::Write)
          .Bitflags(static_cast<ConsumerAccess>(3));
  static_cast<void>(StagedPermissions.QualifiedName());

  Luna::EnumBuilder<ConsumerLegacy> Legacies =
      Deeper.RegisterEnum<ConsumerLegacy>(
          "Legacy", Luna::StableTypeKey("consumer.Legacy"));
  Luna::EnumBuilder<ConsumerLegacy> &StagedLegacies =
      Legacies.AllowUnscoped()
          .Value("First", ConsumerLegacyFirst)
          .Value("Second", static_cast<std::int64_t>(2))
          .Bitflags()
          .Bitflags(static_cast<std::int64_t>(3));
  static_cast<void>(StagedLegacies.QualifiedName());

  // A module registers into the scope it is declared in, through the same
  // builder operations.
  const auto ConfigureModule = [](Luna::NamespaceBuilder &Builder) {
    Luna::NamespaceBuilder Units = Builder.RegisterNamespace("Units");
    static_cast<void>(Units.RegisterConstant("Metre", 1));
  };
  Luna::ModuleDependency Requirement;
  Requirement.Identity = "consumer.units";
  if (const std::optional<Luna::VersionConstraint> Constraint =
          Luna::VersionConstraint::TryParse(">=1.0.0"))
    Requirement.Constraints.push_back(*Constraint);
  const std::optional<Luna::ModuleManifest> Definition =
      Luna::ModuleManifest::TryCreate(
          "consumer.units",
          Luna::SemanticVersion::TryParse("1.0.0").value_or(
              Luna::SemanticVersion()),
          {}, std::string(), {});
  const std::optional<Luna::ModuleManifest> Consumer =
      Luna::ModuleManifest::TryCreate(
          "consumer.physics",
          Luna::SemanticVersion::TryParse("1.2.0").value_or(
              Luna::SemanticVersion()),
          {Requirement}, std::string(), {});
  if (Definition && Consumer) {
    [[maybe_unused]] const Luna::RegistrationResult Provided =
        Registry.ProvideModule(*Definition, ConfigureModule);
    Luna::NamespaceBuilder &WithModule =
        Nested.RegisterModule(*Consumer, ConfigureModule);
    static_cast<void>(WithModule.QualifiedName());
    [[maybe_unused]] const Luna::RegistrationResult RootModule =
        Registry.RegisterModule(*Definition, ConfigureModule);
  }

  // The whole plan commits as one transaction, and an abandoned builder simply
  // goes out of scope.
  [[maybe_unused]] const Luna::RegistrationResult Committed = Studio.Commit();
  Luna::NamespaceBuilder Abandoned = Registry.RegisterNamespace("Discarded");
  Luna::NamespaceBuilder Moved = std::move(Abandoned);
  Abandoned = std::move(Moved);
  [[maybe_unused]] const Luna::ReflectionSnapshot Published =
      Registry.Reflection();
}

namespace {

// A consumer type a custom converter is written for.
struct ConsumerVector final {
  double X = 0.0;
  double Y = 0.0;
};

} // namespace

namespace Luna {

// The custom conversion boundary is reachable through the umbrella header alone
// and is built only from Luna-owned and standard-library types: no Luau type,
// header, include path, pointer, stack index, registry reference, constant, or
// macro appears in, or is required by, a converter.
template <> class TypeConverter<ConsumerVector> {
public:
  [[nodiscard]] ConversionProbe Probe(ValueView Source,
                                      const ConversionContext &Context) const {
    if (!Source.IsTable())
      return RejectedProbe(Context.Describe("expected a table of X and Y"));
    return ViableProbe(ConversionRank::SafeBuiltIn);
  }

  [[nodiscard]] ConversionResult<ConsumerVector>
  Read(ValueView Source, ConversionContext &Context) const {
    ConversionResult<ConsumerVector> Result;
    const std::optional<double> X = Source.Field("X").ToNumber();
    const std::optional<double> Y = Source.Field("Y").ToNumber();
    if (!X || !Y) {
      Result.Status = ConversionStatus::TypeMismatch;
      Result.Diagnostic = Context.Describe("expected numeric X and Y fields");
      return Result;
    }
    Result.Status = ConversionStatus::Success;
    Result.ConvertedValue = ConsumerVector{*X, *Y};
    return Result;
  }

  [[nodiscard]] WriteResult Write(const ConsumerVector &Source,
                                  ConversionContext &Context) const {
    OwnedValue Published = OwnedValue::Table();
    Published.SetField("X", OwnedValue::Number(Source.X));
    Published.SetField("Y", OwnedValue::Number(Source.Y));
    if (const WriteResult Reserved =
            Context.Reserve(Published.RequiredReservation());
        !Reserved.IsSuccess())
      return Reserved;
    return Context.Publish(Published);
  }
};

} // namespace Luna

namespace {

static_assert(Luna::ConversionCapable<ConsumerVector>,
              "A consumer converter must satisfy the boundary concept using "
              "only Luna-owned and standard-library types.");
static_assert(std::is_trivially_copyable_v<Luna::ValueView> &&
                  std::is_trivially_copyable_v<Luna::ConversionContext>,
              "Transient conversion tokens must stay plain Luna values.");
// A committing publication is unreachable through the const context a probe
// receives, so probe purity is enforced by the type system itself.
template <class Type>
concept PublishesThroughConstContext =
    requires(const Type &Context, const Luna::OwnedValue &Published) {
      Context.Publish(Published);
    };

static_assert(!PublishesThroughConstContext<Luna::ConversionContext>,
              "A probing context must not reach any committing publication.");
static_assert(
    std::is_same_v<decltype(Luna::MaximumConversionStringBytes()), std::size_t>,
    "The inherited string byte policy must be a Luna-owned value.");

} // namespace

namespace {

// An aggregate consumer type whose converter recurses through the boundary
// entry points rather than through any Luna internal.
struct ConsumerPolyline final {
  std::vector<ConsumerVector> Points;
};

} // namespace

namespace Luna {

template <> class TypeConverter<ConsumerPolyline> {
public:
  [[nodiscard]] ConversionProbe Probe(ValueView Source,
                                      const ConversionContext &Context) const {
    if (!Source.IsTable())
      return RejectedProbe(Context.Describe("expected a table of points"));
    for (std::size_t Index = 0; Index < Source.Size(); ++Index) {
      const ConversionProbe Element =
          ProbeValue<ConsumerVector>(Source.Element(Index), Context);
      if (!Element.IsViable)
        return Element;
    }
    // A nested user conversion is a user conversion, ranked as its own
    // category rather than as a score.
    return ViableProbe(ConversionRank::User);
  }

  [[nodiscard]] ConversionResult<ConsumerPolyline>
  Read(ValueView Source, ConversionContext &Context) const {
    ConversionResult<ConsumerPolyline> Result;
    ConsumerPolyline Converted;
    for (std::size_t Index = 0; Index < Source.Size(); ++Index) {
      const ConversionResult<ConsumerVector> Element =
          ReadValue<ConsumerVector>(Source.Element(Index), Context);
      if (!Element.IsSuccess()) {
        // The nested view already names the complete element path.
        Result.Status = Element.Status;
        Result.Diagnostic = Element.Diagnostic;
        return Result;
      }
      Converted.Points.push_back(*Element.ConvertedValue);
    }
    Result.Status = ConversionStatus::Success;
    Result.ConvertedValue = std::move(Converted);
    return Result;
  }

  [[nodiscard]] WriteResult Write(const ConsumerPolyline &Source,
                                  ConversionContext &Context) const {
    // The complete aggregate is staged and reserved before anything is
    // published, so a refusal exposes no partial table.
    OwnedValue Published = OwnedValue::Table();
    for (const ConsumerVector &Point : Source.Points) {
      OwnedValue Element = OwnedValue::Table();
      Element.SetField("X", OwnedValue::Number(Point.X));
      Element.SetField("Y", OwnedValue::Number(Point.Y));
      Published.Append(std::move(Element));
    }
    if (Published.LargestStringByteCount() > MaximumConversionStringBytes()) {
      WriteResult Refused;
      Refused.Status = WriteStatus::PolicyExceeded;
      Refused.Diagnostic = Context.Describe("exceeded the string byte policy");
      return Refused;
    }
    if (const WriteResult Reserved =
            Context.Reserve(Published.RequiredReservation());
        !Reserved.IsSuccess())
      return Reserved;
    return Context.Publish(Published);
  }
};

} // namespace Luna

namespace {

static_assert(Luna::ConversionCapable<ConsumerPolyline>,
              "A nested consumer converter must satisfy the boundary concept "
              "without naming a virtual machine.");

} // namespace

// Retaining a converted value needs no Luau declaration: the owning value and
// pack are ordinary consumer values built from the standard library.
void VerifyConversionConsumerBoundaryCompiles() {
  Luna::OwnedValue Retained = Luna::OwnedValue::Table();
  Retained.Append(Luna::OwnedValue::FromValue(Luna::Value(41)));
  Retained.SetField("Name", Luna::OwnedValue::Text("value"));

  Luna::ValuePack Pack;
  Pack.Append(std::move(Retained));
  Pack.Append(Luna::OwnedValue::Nil());

  const Luna::ValueReservation Required = Pack.RequiredReservation();
  [[maybe_unused]] const bool Accounted =
      Required.ValueCount >= Pack.Size() &&
      Pack.LargestStringByteCount() <= Luna::MaximumConversionStringBytes() &&
      Pack.At(0).HasField("Name") &&
      Pack.At(0).Field("Name").ToText() == std::string("value") &&
      !Pack.At(1).ToValue().has_value();

  // A transient view exposes shape only; an inert one answers as a value.
  const Luna::ValueView Inert;
  [[maybe_unused]] const bool Transient =
      !Inert.IsActive() && Inert.Kind() == Luna::ValueCategory::None &&
      Inert.Size() == 0 && Inert.Path().empty() && Inert.ToOwned().IsNil();
}

namespace {

// One consumer signature matrix, decided entirely at compile time. Every
// accepted form below is registrable through the public API, and every rejected
// one is refused by the same concept a registration is constrained on - so a
// consumer learns about an unsupported signature from its own compiler, with no
// Luau include, link, pointer, or macro anywhere in the picture.

// The accepted forms: required parameters, a trailing optional, declared
// defaults, both variadic forms, and zero, scalar, and multiple returns.
[[nodiscard]] int ConsumerRequired(int Value, std::string Text) {
  return Value + static_cast<int>(Text.size());
}

[[nodiscard]] int ConsumerOptional(int Value, std::optional<int> Factor) {
  return Value * (Factor ? *Factor : 1);
}

[[nodiscard]] int ConsumerOffset(int Value, int Amount) {
  return Value + Amount;
}

[[nodiscard]] int ConsumerViewTail(Luna::ArgumentView Arguments) {
  return static_cast<int>(Arguments.Size());
}

[[nodiscard]] int ConsumerPackTail(std::string Separator,
                                   Luna::ArgumentPack Arguments) {
  return static_cast<int>(Separator.size() + Arguments.Size());
}

void ConsumerZeroReturn(int Value) { static_cast<void>(Value); }

[[nodiscard]] std::pair<int, int> ConsumerPairReturn(int Value) {
  return {Value, Value};
}

[[nodiscard]] std::tuple<bool, double, std::string>
ConsumerTupleReturn(int Value) {
  return {Value > 0, static_cast<double>(Value), std::to_string(Value)};
}

[[nodiscard]] Luna::ReturnPack ConsumerDynamicReturn(int Count) {
  Luna::ReturnPack Pack;
  for (int Index = 0; Index < Count; ++Index)
    Pack.AppendInteger(Index);
  return Pack;
}

static_assert(Luna::SupportedCallable<decltype(&ConsumerRequired)>,
              "Required supported parameters must stay registrable.");
static_assert(Luna::SupportedCallable<decltype(&ConsumerOptional)>,
              "A trailing optional parameter must stay registrable.");
static_assert(
    Luna::SupportedCallable<decltype(Luna::WithDefaults(&ConsumerOffset, 5))>,
    "Declared defaults must stay registrable through the public helper.");
static_assert(Luna::SupportedCallable<decltype(&ConsumerViewTail)>,
              "The callback-lifetime variadic view must stay registrable.");
static_assert(Luna::SupportedCallable<decltype(&ConsumerPackTail)>,
              "The owning variadic pack must stay registrable.");
static_assert(Luna::SupportedCallable<decltype(&ConsumerZeroReturn)>,
              "A zero-value return must stay registrable.");
static_assert(Luna::SupportedCallable<decltype(&ConsumerPairReturn)>,
              "A returned pair must stay registrable.");
static_assert(Luna::SupportedCallable<decltype(&ConsumerTupleReturn)>,
              "A returned tuple must stay registrable.");
static_assert(Luna::SupportedCallable<decltype(&ConsumerDynamicReturn)>,
              "A returned dynamic pack must stay registrable.");
static_assert(Luna::SupportedCallable<
                  decltype(Luna::Overload<int(int)>(&ConsumerMeasure))>,
              "An overload selection of a supported signature must stay "
              "registrable.");

// The rejected forms. Each one names a shape or a type Luna does not describe,
// and the rejection is the concept's, not a diagnostic discovered at runtime.
static_assert(!Luna::SupportedCallable<int (*)(std::string_view)>,
              "A parameter type Luna cannot convert must stay rejected.");
static_assert(!Luna::SupportedCallable<std::string_view (*)(int)>,
              "A return type Luna cannot publish must stay rejected.");
static_assert(!Luna::SupportedCallable<std::optional<int> (*)(int)>,
              "An optional return is not a declared return shape.");
static_assert(!Luna::SupportedCallable<std::pair<int, float> (*)()>,
              "Every element of a returned pair must be a supported value.");
static_assert(!Luna::SupportedCallable<std::tuple<int, void *> (*)()>,
              "Every element of a returned tuple must be a supported value.");
static_assert(!Luna::SupportedCallable<int (*)(Luna::ReturnPack)>,
              "A return pack is never a parameter shape.");
static_assert(!Luna::SupportedCallable<int (*)(Luna::ArgumentView, int)>,
              "A variadic parameter must stay the final one.");
static_assert(
    !Luna::SupportedCallable<int (*)(Luna::ArgumentView, Luna::ArgumentPack)>,
    "A callable declares at most one variadic parameter.");
static_assert(!Luna::SupportedCallable<ConsumerScaling>,
              "A callable object with several signatures stays ambiguous.");
static_assert(
    !Luna::SupportedCallable<decltype(Luna::Overload<std::string_view(int)>(
        [](int Value) -> std::string_view {
          static_cast<void>(Value);
          return {};
        }))>,
    "An overload selection must not weaken the converter checks.");
static_assert(!Luna::ExactOverloadTarget<ConsumerScaling, int(std::string)>,
              "A signature the callable cannot be invoked with must stay "
              "rejected.");
static_assert(!Luna::ExactOverloadTarget<int (*)(int), int(int)>,
              "A function pointer keeps its own non-deduced overload form.");

// One selection the helper itself refuses: the declared signature is not one
// the target can be invoked with, so the call is ill-formed rather than a
// weaker registration.
template <class Signature, class Target>
concept OverloadSelectable =
    requires(Target Selected) { Luna::Overload<Signature>(Selected); };

static_assert(OverloadSelectable<double(double), ConsumerScaling>,
              "A selectable signature must stay selectable.");
static_assert(!OverloadSelectable<int(std::string), ConsumerScaling>,
              "An unselectable signature must not compile at all.");

} // namespace

// The same matrix through the public registration API, at the root scope and
// inside a namespace: rich parameters, declared defaults, both variadic forms,
// and every return shape, retained and read as ordinary consumer values.
void VerifyRichSignatureConsumerBoundaryCompiles() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  [[maybe_unused]] const Luna::RegistrationResult Optional =
      Registry.RegisterFunction("Scaled", &ConsumerOptional);
  [[maybe_unused]] const Luna::RegistrationResult Defaulted =
      Registry.RegisterFunction("Offset",
                                Luna::WithDefaults(&ConsumerOffset, 5));
  [[maybe_unused]] const Luna::RegistrationResult View =
      Registry.RegisterFunction("Count", &ConsumerViewTail);
  [[maybe_unused]] const Luna::RegistrationResult Pack =
      Registry.Register("Join", &ConsumerPackTail);

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::NamespaceBuilder &Staged =
      Studio.RegisterFunction("Divide", &ConsumerPairReturn)
          .RegisterFunction("Describe", &ConsumerTupleReturn)
          .RegisterFunction("Repeat", &ConsumerDynamicReturn)
          .RegisterFunction("Reset", &ConsumerZeroReturn);
  [[maybe_unused]] const Luna::RegistrationResult Published = Staged.Commit();

  // A declared parameter descriptor and a return pack are consumer values: a
  // shape can be described, validated, and inspected without a State.
  const Luna::ParameterDescriptor Required =
      Luna::ParameterDescriptor::ForRequired(Luna::ValueKind::Integer);
  const Luna::ParameterDescriptor WithDefault =
      Luna::ParameterDescriptor::ForDefaulted(Luna::ValueKind::Integer,
                                              Luna::Value(2));
  const Luna::ParameterDescriptor Tail =
      Luna::ParameterDescriptor::ForVariadic(true);
  const std::vector<Luna::ParameterDescriptor> Shape{Required, WithDefault,
                                                     Tail};
  const Luna::ParameterShapeIssue Issue = Luna::ValidateParameterShape(Shape);
  const Luna::ParameterArity Arity = Luna::ArityOf(Shape);
  [[maybe_unused]] const bool Described =
      Issue.IsValid() && Arity.Minimum == 1 && Arity.IsVariadic &&
      !Arity.Maximum.has_value() &&
      Required.Form() == Luna::ParameterForm::Required &&
      WithDefault.HasDefault() && Tail.Retains() &&
      !Luna::ParameterFormText(Luna::ParameterForm::Variadic).empty();

  Luna::ReturnPack Returns;
  Returns.AppendInteger(1).AppendText("two").AppendBoolean(true);
  const Luna::ReturnPack Copied = Returns;
  [[maybe_unused]] const bool Ordered =
      Returns.Size() == 3 && Returns.Position(0) == 1 &&
      Returns.At(0) != nullptr && !Returns.IsEmpty() && Copied == Returns &&
      Copied != Luna::ReturnPack::Empty() && Returns.Values().size() == 3 &&
      Luna::ReturnPack::Empty().IsEmpty();

  // The owning variadic pack is equally a consumer value, retained past the
  // callback that produced it, while an inert view answers as an empty one.
  Luna::ValuePack Values;
  Values.Append(Luna::OwnedValue::Number(1.0));
  const Luna::ArgumentPack Arguments(std::move(Values), 2);
  const Luna::ArgumentView Inert;
  [[maybe_unused]] const bool Retained =
      Arguments.Size() == 1 && Arguments.FirstPosition() == 2 &&
      Arguments.Position(0) == 2 && !Inert.IsActive() && Inert.Size() == 0 &&
      Inert.At(0).IsNil();
}

namespace {

// Two consumer classes a binding exposes as typed userdata: one plain value
// type and one polymorphic type whose objects an engine owns.
struct ConsumerTransform final {
  double Scale = 1.0;
  double Offset = 0.0;
};

class ConsumerEntity {
public:
  virtual ~ConsumerEntity() = default;

  [[nodiscard]] int Identifier() const noexcept { return IdentifierValue; }

private:
  int IdentifierValue = 0;
};

// Registering a class is reachable through the umbrella header alone, at the
// root scope and inside a namespace, and keeps its typed builder result.
static_assert(
    std::is_same_v<decltype(std::declval<Luna::BindingRegistry &>()
                                .RegisterClass<ConsumerTransform>(
                                    std::declval<std::string_view>(),
                                    std::declval<Luna::StableTypeKey>())),
                   Luna::ClassBuilder<ConsumerTransform>>,
    "Root-scope RegisterClass must keep returning a typed ClassBuilder.");
static_assert(
    std::is_same_v<decltype(std::declval<Luna::NamespaceBuilder &>()
                                .RegisterClass<ConsumerEntity>(
                                    std::declval<std::string_view>(),
                                    std::declval<Luna::StableTypeKey>())),
                   Luna::ClassBuilder<ConsumerEntity>>,
    "A nested RegisterClass must keep returning a typed ClassBuilder.");
static_assert(
    std::is_same_v<typename Luna::ClassBuilder<ConsumerTransform>::Class,
                   ConsumerTransform>,
    "A class builder must keep naming the consumer class it stages.");
static_assert(
    !std::is_copy_constructible_v<Luna::ClassBuilder<ConsumerTransform>> &&
        std::is_move_constructible_v<Luna::ClassBuilder<ConsumerTransform>>,
    "A class builder must stay a move-only owning handle.");
static_assert(
    std::is_same_v<
        decltype(std::declval<Luna::ClassBuilder<ConsumerTransform> &>()
                     .Commit()),
        Luna::RegistrationResult>,
    "Committing a class plan must keep returning RegistrationResult.");
static_assert(
    std::is_same_v<
        decltype(std::declval<const Luna::ClassBuilder<ConsumerTransform> &>()
                     .QualifiedName()),
        std::string_view>,
    "A class builder's qualified name must stay an ordinary string view.");

// The explicit lifetime of a borrowed object is an ownership statement, not a
// pointer: it is copyable, movable, comparable by the lifetime it declares, and
// built only from Luna-owned and standard-library types.
static_assert(std::is_copy_constructible_v<Luna::LifetimeHandle> &&
                  std::is_move_constructible_v<Luna::LifetimeHandle> &&
                  std::is_copy_assignable_v<Luna::LifetimeHandle> &&
                  std::is_default_constructible_v<Luna::LifetimeHandle>,
              "A lifetime handle must remain an ordinary consumer value.");
static_assert(std::is_same_v<decltype(Luna::LifetimeHandle::Undeclared()),
                             Luna::LifetimeHandle>,
              "An undeclared lifetime must stay the same consumer value type.");
static_assert(
    std::is_same_v<
        decltype(std::declval<const Luna::LifetimeHandle &>().Generation()),
        std::uint64_t>,
    "A lifetime generation must stay a plain Luna-owned counter.");
static_assert(
    std::is_same_v<
        decltype(std::declval<Luna::LifetimeHandle &>().Invalidate()), void>,
    "Invalidating a lifetime must stay a plain consumer operation.");
static_assert(
    std::is_same_v<
        decltype(std::declval<const Luna::LifetimeHandle &>().RefersToSame(
            std::declval<const Luna::LifetimeHandle &>())),
        bool>,
    "Comparing two declared lifetimes must stay a plain consumer query.");

// A class whose objects a consumer holds by value, borrows, or shares is
// describable as a canonical consumer descriptor for each of those forms, with
// no Luau type anywhere in the picture.
static_assert(
    Luna::TypeKindAcceptsChildCount(Luna::TypeKind::SharedOwnership, 1),
    "Shared ownership must stay a one-child structural consumer form.");
static_assert(
    Luna::TypeKindAcceptsChildCount(Luna::TypeKind::BorrowedReference, 1),
    "A borrowed reference must stay a one-child structural consumer form.");
static_assert(Luna::TypeKindAcceptsChildCount(Luna::TypeKind::Class, 0),
              "A registered class must stay a canonical consumer leaf.");

} // namespace

// One consumer registering classes and stating how their objects are owned:
// documentation and attributes on the class itself, an explicit lifetime handle
// for a borrowed engine object, an object held by value, and an object held
// through `std::shared_ptr`. Nothing here needs a Luau declaration, an extra
// include path, a link dependency, or a registration macro.
void VerifyClassOwnershipConsumerBoundaryCompiles() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  // A root-scope class, documented and annotated through its own builder.
  Luna::ClassBuilder<ConsumerTransform> Transform =
      Registry.RegisterClass<ConsumerTransform>(
          "Transform", Luna::StableTypeKey("consumer.Transform"));
  Luna::ClassBuilder<ConsumerTransform> &StagedTransform =
      Transform.Documentation("A scale and offset pair.")
          .Attribute("Category", "Math")
          .Attribute("Stable", "true");
  [[maybe_unused]] const std::string_view TransformName =
      StagedTransform.QualifiedName();
  [[maybe_unused]] const Luna::RegistrationResult PublishedTransform =
      StagedTransform.Commit();

  // A nested class in a namespace plan, committed with the rest of that plan.
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<ConsumerEntity> Entity =
      Studio.RegisterClass<ConsumerEntity>(
          "Entity", Luna::StableTypeKey("consumer.Entity"));
  Luna::ClassBuilder<ConsumerEntity> &StagedEntity =
      Entity.Documentation("One engine-owned entity.")
          .Attribute("Ownership", "borrowed");
  [[maybe_unused]] const std::string_view EntityName =
      StagedEntity.QualifiedName();
  [[maybe_unused]] const Luna::RegistrationResult PublishedStudio =
      Studio.Commit();

  // An abandoned class builder is ordinary consumer code: it is move-only, and
  // destroying it uncommitted is simply a scope exit.
  Luna::ClassBuilder<ConsumerTransform> Abandoned =
      Registry.RegisterClass<ConsumerTransform>(
          "Discarded", Luna::StableTypeKey("consumer.Discarded"));
  Luna::ClassBuilder<ConsumerTransform> MovedBuilder = std::move(Abandoned);
  Abandoned = std::move(MovedBuilder);

  // The three ownership forms, exactly as a consumer holds them.
  ConsumerTransform ByValue;
  ByValue.Scale = 2.0;
  const std::shared_ptr<ConsumerEntity> SharedEntity =
      std::make_shared<ConsumerEntity>();
  ConsumerEntity BorrowedEntity;
  [[maybe_unused]] const int Borrowed = BorrowedEntity.Identifier();

  // The borrowed object's explicit lifetime: declared, copied, compared,
  // invalidated once, and idempotent afterwards.
  Luna::LifetimeHandle Lifetime;
  const Luna::LifetimeHandle Shared = Lifetime;
  const Luna::LifetimeHandle Undeclared = Luna::LifetimeHandle::Undeclared();
  const std::uint64_t Live = Lifetime.Generation();
  [[maybe_unused]] const bool Declared =
      Lifetime.IsDeclared() && Lifetime.IsValid() &&
      Shared.RefersToSame(Lifetime) && !Undeclared.IsDeclared() &&
      !Undeclared.IsValid() && Undeclared.Generation() == 0 &&
      !Undeclared.RefersToSame(Lifetime);

  Lifetime.Invalidate();
  Lifetime.Invalidate();
  [[maybe_unused]] const bool Ended =
      !Lifetime.IsValid() && !Shared.IsValid() &&
      Lifetime.Generation() != Live && Lifetime.IsDeclared();

  // A second borrowed object gets its own lifetime, which the first one's
  // invalidation never ended.
  Luna::LifetimeHandle Other;
  [[maybe_unused]] const bool Independent =
      Other.IsValid() && !Other.RefersToSame(Lifetime);

  // Each ownership form is describable as a canonical consumer descriptor built
  // only from Luna-owned values.
  const Luna::TypeDescriptor Value =
      Luna::TypeDescriptor::ForClass(Luna::StableTypeKey("consumer.Transform"));
  std::vector<Luna::TypeDescriptor> Owned;
  Owned.push_back(
      Luna::TypeDescriptor::ForClass(Luna::StableTypeKey("consumer.Entity")));
  const Luna::TypeDescriptor SharedForm = Luna::TypeDescriptor::ForStructure(
      Luna::TypeKind::SharedOwnership, Owned);
  const Luna::TypeDescriptor BorrowedForm = Luna::TypeDescriptor::ForStructure(
      Luna::TypeKind::BorrowedReference, std::move(Owned));
  [[maybe_unused]] const bool Described =
      Value.Kind() == Luna::TypeKind::Class &&
      SharedForm.Kind() == Luna::TypeKind::SharedOwnership &&
      BorrowedForm.Kind() == Luna::TypeKind::BorrowedReference &&
      SharedForm != BorrowedForm && SharedEntity.use_count() == 1 &&
      ByValue.Scale > ByValue.Offset;
}

namespace {

// One consumer arena a class's objects are allocated from. It is ordinary
// consumer code: no Luna base class, no prescribed member names, and no virtual
// machine anywhere in it.
class ConsumerArena final {
public:
  [[nodiscard]] void *Reserve(std::size_t ByteCount, std::size_t Alignment) {
    ReservedBytes += ByteCount;
    ++Reservations;
    return ::operator new(ByteCount, std::align_val_t{Alignment});
  }

  void Return(void *Storage, std::size_t Alignment) {
    ++Returns;
    ::operator delete(Storage, std::align_val_t{Alignment});
  }

  [[nodiscard]] std::size_t Balance() const noexcept {
    return Reservations - Returns;
  }

private:
  std::size_t ReservedBytes = 0;
  std::size_t Reservations = 0;
  std::size_t Returns = 0;
};

// The semantic allocator protocol is reachable through the umbrella header
// alone and is built only from Luna-owned and standard-library types: no Luau
// type, header, include path, pointer, stack index, registry reference,
// constant, or macro appears in, or is required by, a custom allocator.
static_assert(std::is_copy_constructible_v<Luna::ClassAllocator> &&
                  std::is_move_constructible_v<Luna::ClassAllocator> &&
                  std::is_default_constructible_v<Luna::ClassAllocator>,
              "A class allocator must remain an ordinary consumer value.");
static_assert(std::is_trivially_copyable_v<Luna::StorageRequest> &&
                  std::is_trivially_copyable_v<Luna::AllocatorStepResult>,
              "The semantic storage request and step result must stay plain "
              "Luna values.");
static_assert(std::is_same_v<decltype(Luna::ClassAllocator::Undeclared()),
                             Luna::ClassAllocator>,
              "An undeclared protocol must stay the same consumer value type.");
static_assert(
    std::is_same_v<
        decltype(std::declval<const Luna::ClassAllocator &>().PolicyIdentity()),
        std::string_view>,
    "An allocator policy identity must stay an ordinary consumer string view.");
static_assert(
    std::is_same_v<
        decltype(std::declval<const Luna::ClassAllocator &>().OwnsStorage()),
        bool>,
    "Whether Luna owns the storage must stay a plain consumer query.");
static_assert(
    std::is_same_v<
        decltype(Luna::ClassAllocator::ForOwnedObject<ConsumerTransform>()),
        Luna::ClassAllocator>,
    "The ordinary protocol of a class must stay a consumer value.");

} // namespace

// One consumer stating how a class's storage is obtained, how its objects are
// constructed and destroyed, and how the storage is given back. Every step is a
// plain callable over the consumer's own arena, and the whole protocol is one
// copyable Luna value.
void VerifyCustomAllocatorConsumerBoundaryCompiles() {
  const std::shared_ptr<ConsumerArena> Arena =
      std::make_shared<ConsumerArena>();

  // The fully explicit protocol: allocate, construct, destroy a
  // known-constructed object, deallocate.
  Luna::ClassAllocator::AllocateOperation Allocate =
      [Arena](const Luna::StorageRequest &Requested) -> void * {
    return Arena->Reserve(Requested.ByteCount, Requested.Alignment);
  };
  Luna::ClassAllocator::ConstructOperation Construct =
      [](void *Storage) -> Luna::AllocatorStepResult {
    new (Storage) ConsumerTransform();
    return Luna::AllocatorStepResult::Done();
  };
  Luna::ClassAllocator::DestroyOperation Destroy =
      [](void *Storage) -> Luna::AllocatorStepResult {
    static_cast<ConsumerTransform *>(Storage)->~ConsumerTransform();
    return Luna::AllocatorStepResult::Done();
  };
  Luna::ClassAllocator::DeallocateOperation Deallocate =
      [Arena](
          void *Storage,
          const Luna::StorageRequest &Requested) -> Luna::AllocatorStepResult {
    Arena->Return(Storage, Requested.Alignment);
    return Luna::AllocatorStepResult::Done();
  };

  const Luna::StorageRequest Requested =
      Luna::StorageRequest::ForClass<ConsumerTransform>();
  const Luna::ClassAllocator Custom = Luna::ClassAllocator::FromOperations(
      "consumer.arena", Requested, std::move(Allocate), std::move(Construct),
      std::move(Destroy), std::move(Deallocate));

  // The two ready-made protocols, for storage Luna obtains itself and for an
  // object Luna destroys but does not deallocate.
  const Luna::ClassAllocator Ordinary =
      Luna::ClassAllocator::ForOwnedObject<ConsumerTransform>();
  const Luna::ClassAllocator Adopted =
      Luna::ClassAllocator::ForAdoptedObject<ConsumerEntity>("consumer.engine");

  // A protocol whose storage the consumer owns while Luna keeps construction
  // and destruction of the class type.
  const auto ReserveStorage = [Arena](const Luna::StorageRequest &Wanted) {
    return Arena->Reserve(Wanted.ByteCount, Wanted.Alignment);
  };
  const auto ReturnStorage = [Arena](void *Storage,
                                     const Luna::StorageRequest &Wanted) {
    Arena->Return(Storage, Wanted.Alignment);
  };
  const Luna::ClassAllocator Supplied =
      Luna::ClassAllocator::ForStorage<ConsumerTransform>(
          "consumer.arena.storage", ReserveStorage, ReturnStorage);

  const Luna::ClassAllocator Undeclared = Luna::ClassAllocator::Undeclared();
  const Luna::ClassAllocator Copied = Custom;
  [[maybe_unused]] const bool Declared =
      Custom.IsDeclared() && Custom.DeclaresAllocation() &&
      Custom.DeclaresConstruction() && Custom.DeclaresDestruction() &&
      Custom.OwnsStorage() && Custom.PolicyIdentity() == "consumer.arena" &&
      Custom.Storage() == Requested && Copied.RefersToSame(Custom) &&
      Ordinary.OwnsStorage() && Supplied.OwnsStorage() &&
      Adopted.DeclaresDestruction() && !Adopted.OwnsStorage() &&
      !Undeclared.IsDeclared() && !Undeclared.OwnsStorage() &&
      Arena->Balance() == 0;

  // The semantic request itself is a consumer value: a byte count, an
  // alignment, and nothing else.
  const Luna::StorageRequest Empty;
  const Luna::AllocatorStepResult Done = Luna::AllocatorStepResult::Done();
  const Luna::AllocatorStepResult Declined =
      Luna::AllocatorStepResult::Declined();
  [[maybe_unused]] const bool Described =
      Requested.IsUsable() && !Empty.IsUsable() &&
      Requested.ByteCount == sizeof(ConsumerTransform) &&
      Requested.Alignment == alignof(ConsumerTransform) && Done.Performed &&
      !Declined.Performed;
}

namespace {

// One consumer stating how objects of its classes come into existence: default
// and parameterized constructors, a factory that yields the class by value, a
// factory that yields shared ownership, and singleton accessors in each of the
// three forms an accessor may declare.
struct ConsumerPoint final {
  double X = 0.0;
  double Y = 0.0;

  ConsumerPoint() = default;
  ConsumerPoint(double XValue, double YValue) : X(XValue), Y(YValue) {}
};

[[nodiscard]] ConsumerPoint ConsumerMakePoint(double Scale) {
  return ConsumerPoint(Scale, Scale);
}

[[nodiscard]] std::shared_ptr<ConsumerPoint> ConsumerBoxPoint() {
  return std::make_shared<ConsumerPoint>();
}

[[nodiscard]] ConsumerPoint &ConsumerOriginReference() {
  static ConsumerPoint Origin;
  return Origin;
}

[[nodiscard]] ConsumerPoint *ConsumerOriginPointer() {
  return &ConsumerOriginReference();
}

[[nodiscard]] std::shared_ptr<ConsumerPoint> ConsumerSharedOrigin() {
  static const std::shared_ptr<ConsumerPoint> Origin =
      std::make_shared<ConsumerPoint>();
  return Origin;
}

// Every construction operation of a class builder is reachable through the
// umbrella header alone and keeps returning the same builder, so a consumer
// states a whole construction surface as one chain.
static_assert(
    std::is_same_v<decltype(std::declval<Luna::ClassBuilder<ConsumerPoint> &>()
                                .Constructor<double, double>()),
                   Luna::ClassBuilder<ConsumerPoint> &>,
    "A parameterized Constructor must keep returning the same builder.");
static_assert(
    std::is_same_v<
        decltype(std::declval<Luna::ClassBuilder<ConsumerPoint> &>()
                     .Constructor<>(std::declval<std::string_view>())),
        Luna::ClassBuilder<ConsumerPoint> &>,
    "A named Constructor must keep returning the same builder.");
static_assert(
    std::is_same_v<decltype(std::declval<Luna::ClassBuilder<ConsumerPoint> &>()
                                .Factory(std::declval<std::string_view>(),
                                         &ConsumerMakePoint)),
                   Luna::ClassBuilder<ConsumerPoint> &>,
    "Factory must keep returning the same builder.");
static_assert(
    std::is_same_v<decltype(std::declval<Luna::ClassBuilder<ConsumerPoint> &>()
                                .Singleton(std::declval<std::string_view>(),
                                           &ConsumerOriginPointer)),
                   Luna::ClassBuilder<ConsumerPoint> &>,
    "Singleton must keep returning the same builder.");
static_assert(
    std::is_same_v<
        decltype(std::declval<Luna::ClassBuilder<ConsumerPoint> &>().Allocator(
            std::declval<Luna::ClassAllocator>())),
        Luna::ClassBuilder<ConsumerPoint> &>,
    "Allocator must keep returning the same builder.");

// The ownership statement of a construction candidate is a Luna-owned value
// too: three explicit forms, one coherence rule, and no virtual machine
// anywhere.
static_assert(std::is_copy_constructible_v<Luna::OwnershipPolicy> &&
                  std::is_default_constructible_v<Luna::OwnershipPolicy>,
              "An ownership policy must remain an ordinary consumer value.");
static_assert(
    std::is_same_v<
        decltype(std::declval<const Luna::OwnershipPolicy &>().Ownership()),
        Luna::ConstructionOwnership>,
    "An ownership policy must keep naming its construction ownership.");
static_assert(
    std::is_same_v<
        decltype(std::declval<const Luna::OwnershipPolicy &>().IsCoherent()),
        bool>,
    "Ownership coherence must stay a plain consumer query.");
static_assert(
    Luna::ConstructionOwnershipText(Luna::ConstructionOwnership::Shared) ==
        std::string_view("shared"),
    "The reflected ownership text must stay a consumer-readable value.");

} // namespace

// One consumer declaring the whole construction surface of a class, including
// the storage protocol its created values come from, stated both before and
// after the candidates that use it. Nothing here needs a Luau declaration, an
// extra include path, a link dependency, or a registration macro.
void VerifyClassConstructionConsumerBoundaryCompiles() {
  const std::shared_ptr<ConsumerArena> Arena =
      std::make_shared<ConsumerArena>();
  const auto ReserveStorage = [Arena](const Luna::StorageRequest &Wanted) {
    return Arena->Reserve(Wanted.ByteCount, Wanted.Alignment);
  };
  const auto ReturnStorage = [Arena](void *Storage,
                                     const Luna::StorageRequest &Wanted) {
    Arena->Return(Storage, Wanted.Alignment);
  };

  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  // The storage protocol stated first, then every candidate that uses it.
  Luna::ClassBuilder<ConsumerPoint> Point =
      Registry.RegisterClass<ConsumerPoint>(
          "Point", Luna::StableTypeKey("consumer.Point"));
  Luna::ClassAllocator Supplied =
      Luna::ClassAllocator::ForStorage<ConsumerPoint>(
          "consumer.point.arena", ReserveStorage, ReturnStorage);
  Luna::ClassBuilder<ConsumerPoint> &WithStorage =
      Point.Allocator(std::move(Supplied));
  Luna::ClassBuilder<ConsumerPoint> &WithConstructors =
      WithStorage.Constructor<>()
          .Constructor<double, double>()
          .Constructor<double, double>("FromPair")
          .Factory("Uniform", &ConsumerMakePoint)
          .Factory("Boxed", &ConsumerBoxPoint)
          .Documentation("One two-component point.")
          .Documentation("Uniform", "One point with equal components.")
          .Attribute("Category", "Math")
          .Attribute("Uniform", "Pure", "true");
  [[maybe_unused]] const Luna::RegistrationResult PublishedPoint =
      WithConstructors.Commit();

  // The three singleton forms, plus the explicit policies a consumer may state
  // for them, with the storage protocol stated after the candidates instead.
  Luna::LifetimeHandle Lifetime;
  Luna::OwnershipPolicy Borrowed = Luna::OwnershipPolicy::Borrowed(Lifetime);
  Luna::OwnershipPolicy Shared = Luna::OwnershipPolicy::Shared();
  [[maybe_unused]] const Luna::OwnershipPolicy Owned =
      Luna::OwnershipPolicy::LuaOwned();
  [[maybe_unused]] const bool Coherent =
      Borrowed.IsCoherent() && Shared.IsCoherent() && Owned.IsCoherent() &&
      Borrowed.Ownership() == Luna::ConstructionOwnership::Borrowed &&
      Borrowed.Lifetime().IsDeclared();

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Consumer");
  Luna::ClassBuilder<ConsumerPoint> Nested =
      Studio.RegisterClass<ConsumerPoint>(
          "Point", Luna::StableTypeKey("consumer.nested.Point"));
  Luna::ClassBuilder<ConsumerPoint> &WithAccessors =
      Nested.Constructor<double, double>()
          .Singleton("Origin", &ConsumerOriginReference)
          .Singleton("Address", &ConsumerOriginPointer, std::move(Borrowed))
          .Singleton("Boxed", &ConsumerSharedOrigin, std::move(Shared));
  Luna::ClassAllocator Ordinary =
      Luna::ClassAllocator::ForOwnedObject<ConsumerPoint>("consumer.point");
  Luna::ClassBuilder<ConsumerPoint> &WithLateStorage =
      WithAccessors.Allocator(std::move(Ordinary));
  static_cast<void>(WithLateStorage.QualifiedName());
  [[maybe_unused]] const Luna::RegistrationResult PublishedNested =
      Studio.Commit();
  [[maybe_unused]] const bool Balanced = Arena->Balance() == 0;
}

namespace {

// One consumer class hierarchy whose whole member surface a binding exposes: a
// base declaring targets the derived class registers, a virtual member, an
// overload set, a static member, every accessor form, and both field forms.
struct ConsumerVehicle {
  virtual ~ConsumerVehicle() = default;

  int Fuel = 0;

  [[nodiscard]] int Range() const { return Fuel * 2; }
  void Refuel(int Amount) { Fuel += Amount; }
  [[nodiscard]] virtual int Wheels() const { return 0; }
};

struct ConsumerTruck final : ConsumerVehicle {
  int Load = 0;
  int Spare = 0;
  double Ratio = 1.0;
  const int Plate = 7;
  std::string Name = "truck";

  [[nodiscard]] int Wheels() const override { return 6; }
  [[nodiscard]] int Capacity() const { return Load * 2; }
  void SetCapacity(int Value) { Load = Value; }
  [[nodiscard]] double Weight() { return 1.0; }
  [[nodiscard]] int Haul(int Distance) const { return Load * Distance; }
  [[nodiscard]] int Haul(int Distance, int Trips) const {
    return Load * Distance * Trips;
  }
  [[nodiscard]] static int Axles() { return 3; }
};

[[nodiscard]] int ConsumerTruckLoad(const ConsumerTruck &Source) {
  return Source.Load;
}

void ConsumerSetTruckLoad(ConsumerTruck &Target, int Value) {
  Target.Load = Value;
}

[[nodiscard]] int ConsumerTruckWheels(const ConsumerTruck *Source) {
  return Source->Wheels();
}

[[nodiscard]] int ConsumerTruckTare(ConsumerTruck *Target) {
  Target->SetCapacity(0);
  return 0;
}

[[nodiscard]] float ConsumerTruckDrag(const ConsumerTruck &Source) {
  return static_cast<float>(Source.Load);
}

// The non-mutating compatibility check a consumer states for one safe downcast.
// It is stateless and reads the object it was given, so nothing about it can
// mutate the value it decides on.
struct ConsumerVehicleIsTruck {
  [[nodiscard]] bool operator()(const ConsumerVehicle &Received) const {
    return Received.Wheels() == 6;
  }
};

using TruckBuilder = Luna::ClassBuilder<ConsumerTruck>;

// Every class-member operation is reachable through the umbrella header alone
// and keeps returning the same builder, so a consumer states a whole member
// surface as one chain.
static_assert(std::is_same_v<decltype(std::declval<TruckBuilder &>().Method(
                                 std::declval<std::string_view>(),
                                 &ConsumerTruck::Wheels)),
                             TruckBuilder &>,
              "Method must keep returning the same builder.");
static_assert(std::is_same_v<
                  decltype(std::declval<TruckBuilder &>().StaticMethod(
                      std::declval<std::string_view>(), &ConsumerTruck::Axles)),
                  TruckBuilder &>,
              "StaticMethod must keep returning the same builder.");
static_assert(std::is_same_v<decltype(std::declval<TruckBuilder &>().Property(
                                 std::declval<std::string_view>(),
                                 &ConsumerTruck::Capacity)),
                             TruckBuilder &>,
              "A single-getter Property must keep returning the same builder.");
static_assert(
    std::is_same_v<decltype(std::declval<TruckBuilder &>().Property(
                       std::declval<std::string_view>(),
                       &ConsumerTruck::Capacity, &ConsumerTruck::SetCapacity)),
                   TruckBuilder &>,
    "A getter and setter Property must keep returning the same builder.");
static_assert(
    std::is_same_v<decltype(std::declval<TruckBuilder &>().Property(
                       std::declval<std::string_view>(),
                       std::declval<Luna::PropertyPolicy>(),
                       &ConsumerTruck::Capacity)),
                   TruckBuilder &>,
    "An explicit policy with one accessor must keep returning the same "
    "builder.");
static_assert(
    std::is_same_v<decltype(std::declval<TruckBuilder &>().Property(
                       std::declval<std::string_view>(),
                       std::declval<Luna::PropertyPolicy>(),
                       &ConsumerTruck::Capacity, &ConsumerTruck::SetCapacity)),
                   TruckBuilder &>,
    "An explicit policy with both accessors must keep returning the same "
    "builder.");
static_assert(
    std::is_same_v<decltype(std::declval<TruckBuilder &>().Field(
                       std::declval<std::string_view>(), &ConsumerTruck::Load)),
                   TruckBuilder &>,
    "Field must keep returning the same builder.");
static_assert(
    std::is_same_v<decltype(std::declval<TruckBuilder &>().Field(
                       std::declval<std::string_view>(), &ConsumerTruck::Load,
                       std::declval<Luna::FieldPolicy>())),
                   TruckBuilder &>,
    "A Field with an explicit policy must keep returning the same builder.");

// The explicit relationship operations are reachable through the umbrella
// header alone and keep returning the same builder, so a consumer states a
// whole hierarchy and its safe downcast policy as one chain.
static_assert(std::is_same_v<
                  decltype(std::declval<TruckBuilder &>().Base<ConsumerVehicle>(
                      std::declval<const Luna::StableTypeKey &>())),
                  TruckBuilder &>,
              "Base must keep returning the same builder.");
static_assert(
    std::is_same_v<
        decltype(std::declval<TruckBuilder &>().Cast<ConsumerVehicle>(
            std::declval<const Luna::StableTypeKey &>())),
        TruckBuilder &>,
    "A runtime-type-assisted Cast must keep returning the same builder.");
static_assert(std::is_same_v<
                  decltype(std::declval<TruckBuilder &>().Cast<ConsumerVehicle>(
                      std::declval<const Luna::StableTypeKey &>(),
                      std::declval<std::string_view>(),
                      std::declval<ConsumerVehicleIsTruck>())),
                  TruckBuilder &>,
              "A declared-check Cast must keep returning the same builder.");

// The declared policies are consumer values: constexpr text, constexpr
// direction queries, and one coherence rule each.
static_assert(Luna::MemberAccessText(Luna::MemberAccess::ReadWrite) ==
                  std::string_view("read-write"),
              "The reflected access text must stay a consumer-readable value.");
static_assert(Luna::PropertyEvaluationText(Luna::PropertyEvaluation::Lazy) ==
                  std::string_view("lazy"),
              "The reflected evaluation text must stay a consumer value.");
static_assert(Luna::MemberOwnershipText(Luna::MemberOwnership::Copied) ==
                  std::string_view("copied"),
              "The reflected member ownership text must stay a consumer "
              "value.");
static_assert(Luna::PermitsMemberRead(Luna::MemberAccess::ReadOnly) &&
                  !Luna::PermitsMemberWrite(Luna::MemberAccess::ReadOnly),
              "A read-only member must keep permitting exactly one direction.");
static_assert(std::is_copy_constructible_v<Luna::PropertyPolicy> &&
                  std::is_default_constructible_v<Luna::PropertyPolicy> &&
                  std::is_copy_constructible_v<Luna::FieldPolicy> &&
                  std::is_default_constructible_v<Luna::FieldPolicy>,
              "A member policy must remain an ordinary consumer value.");

// The accepted member target forms. Each one states the object it operates on,
// and the trait a registration is constrained on is what says so.
template <class Target>
constexpr bool MethodTargetAccepted =
    Luna::Detail::MethodTargetShape<ConsumerTruck, Target>::IsSupported;
template <class Target>
constexpr bool MethodReceiverIsConst =
    Luna::Detail::MethodTargetShape<ConsumerTruck, Target>::ReceiverIsConst;
template <class Target>
constexpr bool GetterAccepted =
    Luna::Detail::MemberReadShape<ConsumerTruck, Target>::IsSupported;
template <class Target>
constexpr bool GetterNeedsMutableReceiver =
    Luna::Detail::MemberReadShape<ConsumerTruck,
                                  Target>::RequiresMutableReceiver;
template <class Target>
constexpr bool SetterAccepted =
    Luna::Detail::MemberWriteShape<ConsumerTruck, Target>::IsSupported;
template <class Target>
constexpr bool GetterValueSupported = Luna::SupportedValue<
    typename Luna::Detail::MemberReadShape<ConsumerTruck, Target>::Declared>;

static_assert(MethodTargetAccepted<int (ConsumerTruck::*)(int) const>,
              "A const member function pointer of the class is a method "
              "target.");
static_assert(MethodReceiverIsConst<int (ConsumerTruck::*)(int) const>,
              "A const member function pointer declares a const receiver.");
static_assert(MethodTargetAccepted<int (ConsumerVehicle::*)() const> &&
                  MethodReceiverIsConst<int (ConsumerVehicle::*)() const>,
              "A member function pointer declared on a base class is a method "
              "target of the derived class.");
static_assert(MethodTargetAccepted<void (ConsumerVehicle::*)(int)> &&
                  !MethodReceiverIsConst<void (ConsumerVehicle::*)(int)>,
              "A non-const base-declared member function pointer declares a "
              "mutable receiver.");
static_assert(MethodTargetAccepted<decltype(&ConsumerTruckLoad)> &&
                  MethodReceiverIsConst<decltype(&ConsumerTruckLoad)>,
              "A wrapper taking a const reference to the class is a method "
              "target.");
static_assert(MethodTargetAccepted<decltype(&ConsumerSetTruckLoad)> &&
                  !MethodReceiverIsConst<decltype(&ConsumerSetTruckLoad)>,
              "A wrapper taking a mutable reference to the class is a method "
              "target.");
static_assert(MethodTargetAccepted<decltype(&ConsumerTruckWheels)> &&
                  MethodReceiverIsConst<decltype(&ConsumerTruckWheels)>,
              "A wrapper taking a const pointer to the class is a method "
              "target.");
static_assert(MethodTargetAccepted<decltype(&ConsumerTruckTare)> &&
                  !MethodReceiverIsConst<decltype(&ConsumerTruckTare)>,
              "A wrapper taking a mutable pointer to the class is a method "
              "target.");
static_assert(
    MethodTargetAccepted<decltype(Luna::Overload<int(int), ConsumerTruck>(
        &ConsumerTruck::Haul))>,
    "An explicit overload selection of one class-scope signature is a method "
    "target.");

// The rejected member target forms. Each rejection is the trait's, so a
// consumer learns about an undeclarable member from its own compiler rather
// than from a diagnostic discovered at runtime.
static_assert(!MethodTargetAccepted<int (*)(int)>,
              "A callable whose first parameter is not the class declares no "
              "receiver.");
static_assert(!MethodTargetAccepted<int>,
              "A value that is not callable at all is no method target.");
static_assert(!MethodTargetAccepted<void (*)(const ConsumerTransform &)>,
              "A wrapper over another class is no method target of this one.");
static_assert(GetterAccepted<int (ConsumerTruck::*)() const> &&
                  !GetterNeedsMutableReceiver<int (ConsumerTruck::*)() const>,
              "A const accessor reads through a const view.");
static_assert(GetterAccepted<double (ConsumerTruck::*)()> &&
                  GetterNeedsMutableReceiver<double (ConsumerTruck::*)()>,
              "A non-const accessor needs a mutable view.");
static_assert(GetterAccepted<int ConsumerTruck::*> &&
                  !GetterNeedsMutableReceiver<int ConsumerTruck::*>,
              "A data member of the class is read as the value it holds.");
static_assert(GetterAccepted<decltype(&ConsumerTruckLoad)>,
              "A callable taking the class and returning a supported value is "
              "a getter.");
static_assert(!GetterAccepted<void (ConsumerTruck::*)(int)>,
              "A setter is no getter.");
static_assert(!GetterAccepted<int (*)(const ConsumerTransform &)>,
              "A callable over another class is no getter of this one.");
static_assert(!GetterValueSupported<decltype(&ConsumerTruckDrag)>,
              "A getter whose declared value type Luna cannot convert stays "
              "rejected.");
static_assert(!GetterValueSupported<float (ConsumerTruck::*)() const>,
              "A const accessor of an unconvertible value type stays "
              "rejected.");
static_assert(SetterAccepted<void (ConsumerTruck::*)(int)>,
              "A mutator of the class is a setter.");
static_assert(SetterAccepted<int ConsumerTruck::*>,
              "A mutable data member of the class is a setter.");
static_assert(SetterAccepted<decltype(&ConsumerSetTruckLoad)>,
              "A callable taking the class and one supported value is a "
              "setter.");
static_assert(!SetterAccepted<const int ConsumerTruck::*>,
              "A const data member is never written through.");
static_assert(!SetterAccepted<int (ConsumerTruck::*)() const>,
              "A getter is no setter.");
static_assert(!SetterAccepted<void (*)(int)>,
              "A callable whose first parameter is not the class is no "
              "setter.");

} // namespace

// One consumer declaring the whole member surface of a class: instance methods
// through every accepted target form, one static method, every `PropertyPolicy`
// mode, and every `FieldPolicy` factory. Nothing here needs a Luau declaration,
// an extra include path, a link dependency, or a registration macro.
void VerifyClassMemberConsumerBoundaryCompiles() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  Luna::NamespaceBuilder Fleet = Registry.RegisterNamespace("Fleet");
  Luna::ClassBuilder<ConsumerTruck> Truck = Fleet.RegisterClass<ConsumerTruck>(
      "Truck", Luna::StableTypeKey("consumer.Truck"));

  // Methods: base-declared const and non-const member pointers, a virtual
  // member, four wrapper forms, one overload set selected without a macro, and
  // one static member.
  Luna::ClassBuilder<ConsumerTruck> &WithMethods =
      Truck.Constructor<>()
          .Method("Range", &ConsumerVehicle::Range)
          .Method("Refuel", &ConsumerVehicle::Refuel)
          .Method("Wheels", &ConsumerVehicle::Wheels)
          .Method("Load", &ConsumerTruckLoad)
          .Method("SetLoad", &ConsumerSetTruckLoad)
          .Method("Count", &ConsumerTruckWheels)
          .Method("Tare", &ConsumerTruckTare)
          .Method("Haul",
                  Luna::Overload<int(int), ConsumerTruck>(&ConsumerTruck::Haul))
          .Method("Haul", Luna::Overload<int(int, int), ConsumerTruck>(
                              &ConsumerTruck::Haul))
          .StaticMethod("Axles", &ConsumerTruck::Axles);

  // Properties: both default forms plus every policy factory.
  Luna::ClassBuilder<ConsumerTruck> &WithProperties =
      WithMethods
          .Property("Capacity", &ConsumerTruck::Capacity,
                    &ConsumerTruck::SetCapacity)
          .Property("Weight", &ConsumerTruck::Weight)
          .Property("Reading", Luna::PropertyPolicy::ReadOnly(),
                    &ConsumerTruck::Capacity)
          .Property("Hidden", Luna::PropertyPolicy::WriteOnly(),
                    &ConsumerTruck::SetCapacity)
          .Property("Adjustable", Luna::PropertyPolicy::ReadWrite(),
                    &ConsumerTruck::Capacity, &ConsumerTruck::SetCapacity)
          .Property("Computed", Luna::PropertyPolicy::Computed(),
                    &ConsumerTruck::Capacity)
          .Property("ComputedPair", Luna::PropertyPolicy::ComputedReadWrite(),
                    &ConsumerTruck::Capacity, &ConsumerTruck::SetCapacity)
          .Property("Cached", Luna::PropertyPolicy::Lazy(),
                    &ConsumerTruck::Capacity)
          .Property("CachedPair", Luna::PropertyPolicy::LazyReadWrite(),
                    &ConsumerTruck::Capacity, &ConsumerTruck::SetCapacity)
          .Property("Tally", &ConsumerTruckLoad, &ConsumerSetTruckLoad);

  // Fields: the default policy, both explicit directions, a const data member,
  // and an explicit ownership statement.
  Luna::ClassBuilder<ConsumerTruck> &WithFields =
      WithProperties.Field("Slots", &ConsumerTruck::Load)
          .Field("Balance", &ConsumerTruck::Spare,
                 Luna::FieldPolicy::ReadWrite())
          .Field("Name", &ConsumerTruck::Name, Luna::FieldPolicy::ReadOnly())
          .Field("Plate", &ConsumerTruck::Plate)
          .Field("Ratio", &ConsumerTruck::Ratio,
                 Luna::FieldPolicy::Owned(Luna::MemberOwnership::Copied));

  Luna::ClassBuilder<ConsumerTruck> &Described =
      WithFields.Documentation("One hauling truck.")
          .Attribute("Category", "Fleet");
  static_cast<void>(Described.QualifiedName());
  [[maybe_unused]] const Luna::RegistrationResult Published = Fleet.Commit();

  // The declared policies answer as ordinary consumer values.
  const Luna::PropertyPolicy Default;
  const Luna::PropertyPolicy Lazy = Luna::PropertyPolicy::Lazy();
  const Luna::PropertyPolicy WriteOnly = Luna::PropertyPolicy::WriteOnly();
  [[maybe_unused]] const bool DescribedProperty =
      Default.Access() == Luna::MemberAccess::ReadOnly &&
      Default.Evaluation() == Luna::PropertyEvaluation::Immediate &&
      Default.PermitsRead() && !Default.PermitsWrite() && !Default.IsLazy() &&
      Default.IsCoherent() && Lazy.IsLazy() && Lazy.IsCoherent() &&
      Luna::PropertyPolicy::LazyReadWrite().PermitsWrite() &&
      Luna::PropertyPolicy::Computed().Evaluation() ==
          Luna::PropertyEvaluation::Computed &&
      Luna::PropertyPolicy::ComputedReadWrite().PermitsRead() &&
      Luna::PropertyPolicy::ReadWrite().PermitsWrite() &&
      WriteOnly.PermitsWrite() && !WriteOnly.PermitsRead();

  const Luna::FieldPolicy DefaultField;
  const Luna::FieldPolicy Borrowed =
      Luna::FieldPolicy::Owned(Luna::MemberOwnership::Borrowed);
  [[maybe_unused]] const bool DescribedField =
      DefaultField.Access() == Luna::MemberAccess::ReadWrite &&
      !DefaultField.DeclaresDirection() && DefaultField.PermitsRead() &&
      DefaultField.PermitsWrite() && DefaultField.IsCoherent() &&
      DefaultField.Ownership() == Luna::MemberOwnership::Copied &&
      Luna::FieldPolicy::ReadOnly().DeclaresDirection() &&
      !Luna::FieldPolicy::ReadOnly().PermitsWrite() &&
      Luna::FieldPolicy::ReadWrite().PermitsWrite() && !Borrowed.IsCoherent() &&
      Borrowed.Ownership() == Luna::MemberOwnership::Borrowed;
}

namespace {

// Compile-only relationship/operator consumers. Both accepted declarations and
// declarations intentionally rejected at registration remain expressible using
// only <luna/luna.hpp>, C++20, and standard-library types.
struct ConsumerRelationshipBase {
  virtual ~ConsumerRelationshipBase() = default;
  int Value = 2;
};
struct ConsumerRelationshipSide {
  int Side = 3;
};
struct ConsumerRelationshipDerived final : ConsumerRelationshipBase,
                                           ConsumerRelationshipSide {
  [[nodiscard]] int Add(int Other) const { return Value + Other; }
  [[nodiscard]] int Negate() const { return -Value; }
  [[nodiscard]] std::string Text() const { return std::to_string(Value); }
  void BadValueOperator(int) {}
  [[nodiscard]] int BadLength(int) const { return Value; }
};
struct ConsumerRelationshipUnrelated {};
struct ConsumerPrivateDerived final : private ConsumerRelationshipBase {};
struct ConsumerPlainBase {};
struct ConsumerPlainDerived final : ConsumerPlainBase {};

using RelationshipBuilder = Luna::ClassBuilder<ConsumerRelationshipDerived>;

static_assert(
    std::is_same_v<decltype(std::declval<RelationshipBuilder &>().Operator(
                       Luna::ClassOperator::Add,
                       &ConsumerRelationshipDerived::Add)),
                   RelationshipBuilder &>,
    "Operator must keep returning the same typed class builder.");
static_assert(
    Luna::ClassOperatorOperandCount(Luna::ClassOperator::Length) == 0 &&
        Luna::ClassOperatorOperandCount(Luna::ClassOperator::Add) == 1 &&
        Luna::ClassOperatorOperandCount(Luna::ClassOperator::Assign) == 2,
    "Operator families must retain their public call shapes.");
static_assert(
    Luna::ClassOperatorProducesValue(Luna::ClassOperator::Add) &&
        !Luna::ClassOperatorProducesValue(Luna::ClassOperator::Assign) &&
        Luna::ClassOperatorUsesReservedDispatch(Luna::ClassOperator::Index) &&
        Luna::ClassOperatorUsesReservedDispatch(Luna::ClassOperator::Assign),
    "Value and reserved-dispatch operator policies stay consumer-visible.");

} // namespace

void VerifyInheritanceOperatorConsumerBoundaryCompiles() {
  Luna::State Owner;
  Luna::NamespaceBuilder Types =
      Owner.Bindings().RegisterNamespace("ConsumerRelationships");
  Luna::ClassBuilder<ConsumerRelationshipBase> Base =
      Types.RegisterClass<ConsumerRelationshipBase>(
          "Base", Luna::StableTypeKey("consumer.relationship.Base"));
  static_cast<void>(Base.QualifiedName());
  Luna::ClassBuilder<ConsumerRelationshipSide> Side =
      Types.RegisterClass<ConsumerRelationshipSide>(
          "Side", Luna::StableTypeKey("consumer.relationship.Side"));
  static_cast<void>(Side.QualifiedName());
  Luna::ClassBuilder<ConsumerRelationshipDerived> Derived =
      Types.RegisterClass<ConsumerRelationshipDerived>(
          "Derived", Luna::StableTypeKey("consumer.relationship.Derived"));
  Derived
      .Base<ConsumerRelationshipBase>(
          Luna::StableTypeKey("consumer.relationship.Base"))
      .Base<ConsumerRelationshipSide>(
          Luna::StableTypeKey("consumer.relationship.Side"))
      .Cast<ConsumerRelationshipBase>(
          Luna::StableTypeKey("consumer.relationship.Base"))
      .Operator(Luna::ClassOperator::Add, &ConsumerRelationshipDerived::Add)
      .Operator(Luna::ClassOperator::Negate,
                &ConsumerRelationshipDerived::Negate)
      .Operator(Luna::ClassOperator::ToText, &ConsumerRelationshipDerived::Text)
      // An operator is documented by what it answers, never by the Luna-owned
      // segment it is published under.
      .Documentation(Luna::ClassOperator::Add, "Adds two derived values.")
      .Attribute(Luna::ClassOperator::Add, "Pure", "true")
      .Example(Luna::ClassOperator::Add, "local Sum = Left + Right")
      .Example("One derived value with two bases.");
  [[maybe_unused]] const Luna::RegistrationResult Accepted = Types.Commit();

  // These forms intentionally describe registration-time refusals: an
  // unrelated/inaccessible base, a cast with no safe policy, a value operator
  // returning void, and an operator with the wrong operand count. Their public
  // declarations compile without exposing Luau; Commit reports the rejection.
  Luna::ClassBuilder<ConsumerRelationshipDerived> Missing =
      Owner.Bindings().RegisterClass<ConsumerRelationshipDerived>(
          "MissingBase", Luna::StableTypeKey("consumer.rejected.Missing"));
  [[maybe_unused]] const Luna::RegistrationResult MissingResult =
      Missing
          .Base<ConsumerRelationshipUnrelated>(
              Luna::StableTypeKey("consumer.relationship.Unrelated"))
          .Commit();

  Luna::ClassBuilder<ConsumerPrivateDerived> Inaccessible =
      Owner.Bindings().RegisterClass<ConsumerPrivateDerived>(
          "Inaccessible", Luna::StableTypeKey("consumer.rejected.Private"));
  [[maybe_unused]] const Luna::RegistrationResult InaccessibleResult =
      Inaccessible
          .Base<ConsumerRelationshipBase>(
              Luna::StableTypeKey("consumer.relationship.Base"))
          .Commit();

  Luna::ClassBuilder<ConsumerPlainDerived> UnsafeCast =
      Owner.Bindings().RegisterClass<ConsumerPlainDerived>(
          "UnsafeCast", Luna::StableTypeKey("consumer.rejected.UnsafeCast"));
  UnsafeCast.Base<ConsumerPlainBase>(
      Luna::StableTypeKey("consumer.relationship.PlainBase"));
  [[maybe_unused]] const Luna::RegistrationResult UnsafeCastResult =
      UnsafeCast
          .Cast<ConsumerPlainBase>(
              Luna::StableTypeKey("consumer.relationship.PlainBase"))
          .Commit();

  Luna::ClassBuilder<ConsumerRelationshipDerived> BadOperator =
      Owner.Bindings().RegisterClass<ConsumerRelationshipDerived>(
          "BadOperator", Luna::StableTypeKey("consumer.rejected.Operator"));
  [[maybe_unused]] const Luna::RegistrationResult BadOperatorResult =
      BadOperator
          .Operator(Luna::ClassOperator::Add,
                    &ConsumerRelationshipDerived::BadValueOperator)
          .Operator(Luna::ClassOperator::Length,
                    &ConsumerRelationshipDerived::BadLength)
          .Commit();
}
