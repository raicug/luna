// clang-format off
#include <luna/detail/method_adapter.hpp>

#include <type_traits>
// clang-format on

namespace {

// The declared shape of every accepted and rejected method target is decided
// from this header alone: no other Luna header, no Luau include path, and
// nothing but the standard library.
struct StandaloneBase {
  virtual ~StandaloneBase() = default;

  [[nodiscard]] int Depth() const { return 0; }
  void Extend(int By) { static_cast<void>(By); }
  [[nodiscard]] virtual int Faces() const { return 0; }
};

struct StandaloneCrate final : StandaloneBase {
  [[nodiscard]] int Faces() const override { return 6; }
  [[nodiscard]] int Fit(int Amount) const { return Amount; }
};

[[nodiscard]] int StandaloneRead(const StandaloneCrate &Source) {
  return Source.Faces();
}

void StandaloneWrite(StandaloneCrate &Target, int Amount) {
  Target.Extend(Amount);
}

template <class Target>
using Shape = Luna::Detail::MethodTargetShape<StandaloneCrate, Target>;

// A member function pointer states its receiver through its own class and const
// qualification, including when it was declared on a base.
static_assert(Shape<int (StandaloneCrate::*)(int) const>::IsSupported &&
                  Shape<int (StandaloneCrate::*)(int) const>::ReceiverIsConst,
              "A const member function pointer declares a const receiver.");
static_assert(Shape<int (StandaloneBase::*)() const>::IsSupported &&
                  Shape<int (StandaloneBase::*)() const>::ReceiverIsConst,
              "A base-declared const member pointer is a method target.");
static_assert(Shape<void (StandaloneBase::*)(int)>::IsSupported &&
                  !Shape<void (StandaloneBase::*)(int)>::ReceiverIsConst,
              "A base-declared mutating member pointer needs a mutable "
              "receiver.");
static_assert(Shape<int (StandaloneCrate::*)(int) noexcept>::IsSupported,
              "A noexcept member pointer is a method target.");
static_assert(Shape<int (StandaloneCrate::*)(int) const noexcept>::IsSupported,
              "A const noexcept member pointer is a method target.");

// An explicit wrapper states its receiver through its first parameter.
static_assert(Shape<decltype(&StandaloneRead)>::IsSupported &&
                  Shape<decltype(&StandaloneRead)>::ReceiverIsConst,
              "A wrapper taking a const reference declares a const receiver.");
static_assert(Shape<decltype(&StandaloneWrite)>::IsSupported &&
                  !Shape<decltype(&StandaloneWrite)>::ReceiverIsConst,
              "A wrapper taking a mutable reference declares a mutable "
              "receiver.");
static_assert(Shape<int (*)(StandaloneCrate *)>::IsSupported,
              "A wrapper taking a mutable pointer is a method target.");
static_assert(Shape<int (*)(const StandaloneCrate *)>::IsSupported,
              "A wrapper taking a const pointer is a method target.");

// Every other form is rejected by the shape a registration is constrained on.
static_assert(!Shape<int (*)(int)>::IsSupported,
              "A callable whose first parameter is not the class declares no "
              "receiver.");
static_assert(!Shape<int (*)(const StandaloneBase &)>::IsSupported,
              "A wrapper over a base reference is not a receiver of the "
              "registered class.");
static_assert(!Shape<int>::IsSupported,
              "A value that is not callable at all is no method target.");
static_assert(std::is_same_v<typename Shape<int>::Declared,
                             Luna::Detail::UnsupportedMemberShape>,
              "A rejected target declares the unsupported member shape.");

// One instance member and one static member are two different staged requests.
static_assert(!std::is_copy_constructible_v<Luna::Detail::MethodRequest> &&
                  std::is_move_constructible_v<Luna::Detail::MethodRequest>,
              "A staged member request is a move-only value.");

} // namespace
