// clang-format off
#include <luna/luna.hpp>

#include "state/testing/test_hooks.hpp"

#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "class iteration check failed: " << Description << '\n';
}

int StepCalls = 0;

struct Roster final {
  std::vector<std::string> Names{"first", "second", "third"};

  [[nodiscard]] Luna::ReturnPack Step(std::optional<int> Control) const {
    ++StepCalls;
    const int Next = Control ? *Control + 1 : 1;
    if (Next > static_cast<int>(Names.size()))
      return Luna::ReturnPack::Empty();
    Luna::ReturnPack Produced;
    Produced.AppendInteger(Next).AppendText(
        Names[static_cast<std::size_t>(Next - 1)]);
    return Produced;
  }

  [[nodiscard]] int Count() const { return static_cast<int>(Names.size()); }

  [[nodiscard]] int ScalarStep(std::optional<int> Control) const {
    return Control ? *Control : 0;
  }
};

struct Badge final {
  std::string Label;

  [[nodiscard]] std::string Shout() const { return Label + "!"; }
};

struct Wall final {
  std::vector<std::string> Labels{"alpha", "beta"};

  [[nodiscard]] Luna::ReturnPack Step(std::optional<int> Control) const;
};

} // namespace

template <> struct Luna::RegisteredClassTrait<Badge> : std::true_type {};
template <> struct Luna::RegisteredClassTrait<Wall> : std::true_type {};

namespace {

Luna::ReturnPack Wall::Step(std::optional<int> Control) const {
  const int Next = Control ? *Control + 1 : 1;
  if (Next > static_cast<int>(Labels.size()))
    return Luna::ReturnPack::Empty();
  Luna::ReturnPack Produced;
  Produced.AppendInteger(Next);
  Produced.AppendInstance<Badge>(
      Badge{Labels[static_cast<std::size_t>(Next - 1)]});
  return Produced;
}

[[nodiscard]] Luna::StableTypeKey RosterKey() {
  return Luna::StableTypeKey("Studio.IterationRoster");
}

[[nodiscard]] bool RegisterModel(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<Roster> Class =
      Studio.RegisterClass<Roster>("Roster", RosterKey());

  Luna::ClassBuilder<Roster> &WithConstructor = Class.Constructor<>();
  Luna::ClassBuilder<Roster> &WithLength =
      WithConstructor.Operator(Luna::ClassOperator::Length, &Roster::Count);
  Luna::ClassBuilder<Roster> &WithIteration =
      WithLength.Operator(Luna::ClassOperator::Iterate, &Roster::Step);
  static_cast<void>(WithIteration.QualifiedName());
  return Studio.Commit().IsSuccess();
}

[[nodiscard]] bool Succeeds(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return true;
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    std::cerr << "class iteration source failed: " << Diagnostic->Message()
              << '\n';
  return false;
}

void CheckGenericForIteratesOneClass() {
  StepCalls = 0;
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "Value = Studio.Roster.New()"),
        "a script constructs one roster");
  Check(Succeeds(Owner, "assert(#Value == 3)"),
        "the roster still answers its other operators");

  Check(Succeeds(Owner, "local Keys = {}\n"
                        "local Values = {}\n"
                        "for Position, Name in Value do\n"
                        "  Keys[#Keys + 1] = Position\n"
                        "  Values[#Values + 1] = Name\n"
                        "end\n"
                        "assert(#Keys == 3, 'three steps')\n"
                        "assert(Keys[1] == 1 and Keys[3] == 3, 'positions')\n"
                        "assert(Values[1] == 'first', 'first value')\n"
                        "assert(Values[3] == 'third', 'last value')"),
        "a generic for loop iterates the class through its declared step");
  Check(StepCalls == 4,
        "the loop runs one step per element plus the step that ends it");

  Check(Succeeds(Owner, "local Count = 0\n"
                        "for _ in Value do Count = Count + 1 end\n"
                        "assert(Count == 3)"),
        "the same class iterates again from the beginning");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every iteration restores the entry stack depth");
}

void CheckGenericForYieldsInstances() {
  Luna::State Owner;
  Check(Owner.IsReady(), "the state is ready");

  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<Badge> Badges = Studio.RegisterClass<Badge>(
      "Badge", Luna::StableTypeKey("Studio.IterationBadge"));
  Luna::ClassBuilder<Badge> &DeclaredBadge =
      Badges.Field("Label", &Badge::Label).Method("Shout", &Badge::Shout);
  static_cast<void>(DeclaredBadge.QualifiedName());

  Luna::ClassBuilder<Wall> Walls = Studio.RegisterClass<Wall>(
      "Wall", Luna::StableTypeKey("Studio.IterationWall"));
  Luna::ClassBuilder<Wall> &DeclaredWall =
      Walls.Constructor<>().Operator(Luna::ClassOperator::Iterate, &Wall::Step);
  static_cast<void>(DeclaredWall.QualifiedName());

  const Luna::RegistrationResult Committed = Studio.Commit();
  Check(Committed.IsSuccess(), "the iterating container publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "local Wall = Studio.Wall.New()\n"
                        "local Shouted = {}\n"
                        "for Position, Badge in Wall do\n"
                        "  assert(type(Badge) == 'userdata')\n"
                        "  Shouted[Position] = Badge:Shout()\n"
                        "end\n"
                        "assert(#Shouted == 2)\n"
                        "assert(Shouted[1] == 'alpha!')\n"
                        "assert(Shouted[2] == 'beta!')"),
        "a generic for over a container class yields instances the step "
        "manufactured");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "iterating instances restores the entry stack depth");
}

void CheckMalformedIterationStepsAreRefused() {
  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::ClassBuilder<Roster> Class =
        Registry.RegisterClass<Roster>("Roster", RosterKey());
    const Luna::RegistrationResult Result =
        Class.Operator(Luna::ClassOperator::Iterate, &Roster::ScalarStep)
            .Commit();
    Check(!Result.IsSuccess(),
          "an iteration step that publishes one scalar rather than a pack is "
          "refused");
    const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
    Check(Diagnostic != nullptr &&
              Diagnostic->Message().find("ReturnPack") != std::string::npos,
          "the refusal names the pack an iteration step publishes");
  }
}

} // namespace

int RunClassIterationTests();

int RunClassIterationTests() {
  FailureCount = 0;
  CheckGenericForIteratesOneClass();
  CheckGenericForYieldsInstances();
  CheckMalformedIterationStepsAreRefused();
  return FailureCount == 0 ? 0 : 1;
}
