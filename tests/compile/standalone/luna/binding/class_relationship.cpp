// clang-format off
#include <luna/binding/class_relationship.hpp>

#include <string_view>
#include <type_traits>
// clang-format on

namespace {

struct Root {
  virtual ~Root() = default;
  int Value = 0;
};

struct Middle : Root {
  int Extra = 0;
};

struct Unrelated {
  int Other = 0;
};

struct Hidden : private Root {
  int Kept = 0;
};

struct AcceptsEveryRoot {
  [[nodiscard]] bool operator()(const Root &Received) const {
    return Received.Value >= 0;
  }
};

[[nodiscard]] bool DeclaredBaseIsCaptured() {
  const Luna::Detail::BaseRequest Accepted =
      Luna::Detail::MakeBaseRequest<Middle, Root>(
          Luna::StableTypeKey("studio.root"));
  const Luna::Detail::BaseRequest Foreign =
      Luna::Detail::MakeBaseRequest<Middle, Unrelated>(
          Luna::StableTypeKey("studio.unrelated"));
  const Luna::Detail::BaseRequest Inaccessible =
      Luna::Detail::MakeBaseRequest<Hidden, Root>(
          Luna::StableTypeKey("studio.root"));

  return Accepted.DeclaresBase && Accepted.IsAccessible &&
         Accepted.Upcast != nullptr && !Foreign.DeclaresBase &&
         Foreign.Upcast == nullptr && Inaccessible.DeclaresBase &&
         !Inaccessible.IsAccessible && Inaccessible.Upcast == nullptr;
}

[[nodiscard]] bool DeclaredCastIsCaptured() {
  const Luna::Detail::CastRequest Assisted =
      Luna::Detail::MakeRuntimeTypeCastRequest<Middle, Root>(
          Luna::StableTypeKey("studio.root"));
  const Luna::Detail::CastRequest Declared =
      Luna::Detail::MakeCheckedCastRequest<Middle, Root, AcceptsEveryRoot>(
          Luna::StableTypeKey("studio.root"), "studio.acceptsroot");

  return Assisted.UsesRuntimeTypeAssistance && Assisted.DeclaresBase &&
         Assisted.IsAccessible && Assisted.Compatible != nullptr &&
         Assisted.Downcast != nullptr &&
         Assisted.Policy ==
             std::string_view(Luna::Detail::RuntimeTypeCastPolicyName) &&
         !Declared.UsesRuntimeTypeAssistance &&
         Declared.Policy == "studio.acceptsroot" &&
         Declared.Compatible != nullptr && Declared.Downcast != nullptr;
}

static_assert(std::is_copy_constructible_v<Luna::Detail::BaseRequest>,
              "A declared base edge is an ordinary value.");
static_assert(std::is_copy_constructible_v<Luna::Detail::CastRequest>,
              "A declared cast policy is an ordinary value.");
static_assert(Luna::Detail::StaticallyDowncastable<Middle, Root>,
              "A public non-virtual base is statically downcastable.");
static_assert(!Luna::Detail::StaticallyDowncastable<Middle, Unrelated>,
              "An unrelated class is never downcastable.");

[[nodiscard]] bool RelationshipFactsHold() {
  return DeclaredBaseIsCaptured() && DeclaredCastIsCaptured();
}

const bool RelationshipsCompile = RelationshipFactsHold();

} // namespace

namespace Luna::Detail::StandaloneCompileChecks {

[[nodiscard]] bool ClassRelationshipHeaderCompiles() {
  return RelationshipsCompile;
}

} // namespace Luna::Detail::StandaloneCompileChecks
