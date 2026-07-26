// clang-format off
#include <luna/detail/property_adapter.hpp>

#include <string>
#include <type_traits>
// clang-format on

namespace {

// The declared shape of every accepted and rejected accessor is decided from
// this header alone: no other Luna header, no Luau include path, and nothing
// but the standard library.
struct StandaloneGadget final {
  int Charge = 0;
  const int Serial = 1;
  std::string Label;
  double Ratio = 1.0;

  [[nodiscard]] int Level() const { return Charge * 2; }
  void SetLevel(int Value) { Charge = Value; }
  [[nodiscard]] double Weight() { return Ratio; }
  [[nodiscard]] float Drag() const { return 1.0F; }
};

[[nodiscard]] int StandaloneRead(const StandaloneGadget &Source) {
  return Source.Charge;
}

void StandaloneWrite(StandaloneGadget &Target, int Value) {
  Target.Charge = Value;
}

template <class Target>
using ReadShape = Luna::Detail::MemberReadShape<StandaloneGadget, Target>;
template <class Target>
using WriteShape = Luna::Detail::MemberWriteShape<StandaloneGadget, Target>;

// A const accessor reads through a const view; every other accepted getter form
// states what it needs instead.
static_assert(
    ReadShape<int (StandaloneGadget::*)() const>::IsSupported &&
        !ReadShape<int (StandaloneGadget::*)() const>::RequiresMutableReceiver,
    "A const accessor reads through a const view.");
static_assert(
    ReadShape<double (StandaloneGadget::*)()>::IsSupported &&
        ReadShape<double (StandaloneGadget::*)()>::RequiresMutableReceiver,
    "A non-const accessor needs a mutable view.");
static_assert(ReadShape<int StandaloneGadget::*>::IsSupported &&
                  !ReadShape<int StandaloneGadget::*>::RequiresMutableReceiver,
              "A data member is read as the value it holds.");
static_assert(ReadShape<const int StandaloneGadget::*>::IsSupported,
              "A const data member is still readable.");
static_assert(
    ReadShape<decltype(&StandaloneRead)>::IsSupported &&
        !ReadShape<decltype(&StandaloneRead)>::RequiresMutableReceiver,
    "A callable taking a const reference is a getter.");
static_assert(
    std::is_same_v<typename ReadShape<int StandaloneGadget::*>::Declared, int>,
    "A field's declared value type is the type it holds.");

// A mutator, a mutable data member, and a callable of the class are setters.
static_assert(WriteShape<void (StandaloneGadget::*)(int)>::IsSupported,
              "A mutator of the class is a setter.");
static_assert(WriteShape<int StandaloneGadget::*>::IsSupported,
              "A mutable data member is a setter.");
static_assert(WriteShape<decltype(&StandaloneWrite)>::IsSupported,
              "A callable taking the class and one value is a setter.");

// Every other form is rejected by the shape a registration is constrained on.
static_assert(!ReadShape<void (StandaloneGadget::*)(int)>::IsSupported,
              "A setter is no getter.");
static_assert(!ReadShape<int (*)(int)>::IsSupported,
              "A callable that names no receiver is no getter.");
static_assert(!WriteShape<const int StandaloneGadget::*>::IsSupported,
              "A const data member is never written through.");
static_assert(!WriteShape<int (StandaloneGadget::*)() const>::IsSupported,
              "A getter is no setter.");
static_assert(!WriteShape<void (*)(int)>::IsSupported,
              "A callable that names no receiver is no setter.");

// A declared value type Luna cannot copy across the member boundary is refused
// by the value constraint rather than exposed.
static_assert(
    !Luna::SupportedValue<
        typename ReadShape<float (StandaloneGadget::*)() const>::Declared>,
    "An unconvertible declared value type stays rejected.");
static_assert(
    Luna::SupportedValue<
        typename ReadShape<int (StandaloneGadget::*)() const>::Declared>,
    "A supported declared value type stays accepted.");

} // namespace
