// clang-format off
#include <luna/binding/registered_class.hpp>

#include <memory>
#include <type_traits>
#include <utility>
// clang-format on

namespace {

struct Anchor final {
  int Value = 0;
};

struct Foreign final {
  int Value = 0;
};

} // namespace

template <> struct Luna::RegisteredClassTrait<Anchor> : std::true_type {};

namespace {

static_assert(Luna::RegisteredClassType<Anchor>,
              "A class that opted in must satisfy the concept.");
static_assert(!Luna::RegisteredClassType<Foreign>,
              "A class that never opted in must not satisfy the concept.");
static_assert(!Luna::RegisteredClassType<int>,
              "A scalar must not satisfy the registered class concept.");

static_assert(Luna::Detail::IsInstanceReturnType<Anchor>,
              "A registered class must name an instance result by value.");
static_assert(Luna::Detail::IsInstanceReturnType<Anchor *>,
              "A registered class must name a borrowed instance result.");
static_assert(Luna::Detail::IsInstanceReturnType<std::shared_ptr<Anchor>>,
              "A registered class must name a shared instance result.");
static_assert(!Luna::Detail::IsInstanceReturnType<Foreign>,
              "A class that never opted in names no instance result.");
static_assert(
    Luna::Detail::InstanceReturnTrait<Anchor *>::RequiresLifetime,
    "A borrowed instance result must state that it requires a lifetime.");

} // namespace

void VerifyRegisteredClassHeaderCompilesStandalone() {
  Luna::Detail::RecordClassKey<Anchor>(
      Luna::StableTypeKey("Standalone.Anchor"));
  const Luna::StableTypeKey &Recorded =
      Luna::Detail::RecordedClassKey<Anchor>();

  const Luna::Detail::ClassKeyResolver Resolve =
      Luna::Detail::ClassKeyResolverFor<Anchor>();

  Anchor Held{7};
  const Luna::OwnedValue Copied = Luna::OwnedValue::Instance<Anchor>(Anchor{3});
  const Luna::OwnedValue Shared =
      Luna::OwnedValue::Instance<Anchor>(std::make_shared<Anchor>(Anchor{4}));
  const Luna::OwnedValue Borrowed = Luna::OwnedValue::Instance<Anchor>(
      &Held, Luna::OwnershipPolicy::LuaOwned());

  static_cast<void>(Recorded.Text());
  static_cast<void>(Resolve);
  static_cast<void>(Copied.IsPendingInstance());
  static_cast<void>(Shared.IsPendingInstance());
  static_cast<void>(Borrowed.IsPendingInstance());
}
