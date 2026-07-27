// clang-format off
#include <luna/luna.hpp>

#include "support/harness.hpp"
#include "support/model.hpp"

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace {

using LunaBenchmark::CacheMode;
using LunaBenchmark::DiagnosticText;

[[nodiscard]] bool Accepted(const Luna::RegistrationResult &Result,
                            std::string_view Step) {
  if (Result.IsSuccess())
    return true;
  std::cerr << "luna-benchmark registration-failure step=" << Step
            << " reason=\"" << DiagnosticText(Result) << "\"\n";
  return false;
}

[[nodiscard]] int Measure(int Value) { return Value + 1; }
[[nodiscard]] int Measure(int Left, int Right) { return Left * Right; }

struct Surface {
  double Height = 1.0;

  [[nodiscard]] double Raised() const { return Height + 1.0; }
};

template <std::size_t Ordinal> struct Widget final : Surface {
  double Width = 1.0;

  Widget() = default;
  explicit Widget(double WidthValue) : Width(WidthValue) {}

  [[nodiscard]] double Scaled(double Factor) const { return Width * Factor; }
  [[nodiscard]] double Area() const { return Width * Width; }
};

constexpr std::size_t OverloadSetCount = 32;
constexpr std::size_t FunctionRegistrationCount = OverloadSetCount * 2;
constexpr std::size_t ClassCount = 8;
constexpr std::size_t ClassDeclarationCount = 2 + ClassCount * 5;

[[nodiscard]] Luna::StableTypeKey SurfaceKey() {
  return Luna::StableTypeKey("benchmarks.registration.Surface");
}

[[nodiscard]] bool RegisterFunctionCorpus(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  for (std::size_t Index = 0; Index < OverloadSetCount; ++Index) {
    const std::string Name = "Measure" + std::to_string(Index);
    if (!Accepted(
            Registry.RegisterFunction(Name, Luna::Overload<int(int)>(&Measure)),
            "root-function"))
      return false;
    if (!Accepted(Registry.RegisterFunction(
                      Name, Luna::Overload<int(int, int)>(&Measure)),
                  "root-function-overload"))
      return false;
  }
  return true;
}

template <std::size_t Ordinal> void StageClass(Luna::NamespaceBuilder &Studio) {
  using Staged = Widget<Ordinal>;
  const std::string Name = "Widget" + std::to_string(Ordinal);
  const std::string Identity = "benchmarks.registration." + Name;
  Luna::ClassBuilder<Staged> Class =
      Studio.RegisterClass<Staged>(Name, Luna::StableTypeKey(Identity));
  static_cast<void>(Class.template Constructor<double>()
                        .Field("Width", &Staged::Width)
                        .Method("Scaled", &Staged::Scaled)
                        .Property("Area", &Staged::Area)
                        .template Base<Surface>(SurfaceKey())
                        .QualifiedName());
}

template <std::size_t... Ordinals>
void StageClasses(Luna::NamespaceBuilder &Studio,
                  std::index_sequence<Ordinals...>) {
  (StageClass<Ordinals>(Studio), ...);
}

[[nodiscard]] bool RegisterClassCorpus(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<Surface> Base =
      Studio.RegisterClass<Surface>("Surface", SurfaceKey());
  static_cast<void>(Base.Property("Raised", &Surface::Raised).QualifiedName());
  StageClasses(Studio, std::make_index_sequence<ClassCount>());
  return Accepted(Studio.Commit(), "class-plan");
}

[[nodiscard]] bool FreezeState(Luna::State &Owner) {
  return Accepted(Owner.Bindings().Freeze(), "freeze");
}

} // namespace

int main(int ArgumentCount, char **ArgumentValues) {
  LunaBenchmark::Suite Suite("registration", ArgumentCount, ArgumentValues);

  Suite.Measure("RootOverloadSets", "32 root overload sets of two candidates",
                CacheMode::UnfrozenUncached, FunctionRegistrationCount, [] {
                  Luna::State Owner;
                  if (!Owner.IsReady())
                    return false;
                  return RegisterFunctionCorpus(Owner);
                });

  Suite.Measure("RootOverloadSets",
                "32 root overload sets of two candidates, then freeze",
                CacheMode::FrozenCached, FunctionRegistrationCount, [] {
                  Luna::State Owner;
                  if (!Owner.IsReady())
                    return false;
                  if (!RegisterFunctionCorpus(Owner))
                    return false;
                  return FreezeState(Owner);
                });

  Suite.Measure("ClassSurface",
                "one base class and 8 derived classes of one constructor, "
                "field, method, property, and base edge",
                CacheMode::UnfrozenUncached, ClassDeclarationCount, [] {
                  Luna::State Owner;
                  if (!Owner.IsReady())
                    return false;
                  return RegisterClassCorpus(Owner);
                });

  Suite.Measure("ClassSurface",
                "one base class and 8 derived classes of one constructor, "
                "field, method, property, and base edge, then freeze",
                CacheMode::FrozenCached, ClassDeclarationCount, [] {
                  Luna::State Owner;
                  if (!Owner.IsReady())
                    return false;
                  if (!RegisterClassCorpus(Owner))
                    return false;
                  return FreezeState(Owner);
                });

  return Suite.Finish();
}
