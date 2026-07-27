// clang-format off
#include <luna/binding/class_builder.hpp>

#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
// clang-format on

namespace {

struct StandaloneWidget final {
  double Width = 0.0;

  StandaloneWidget() = default;
  explicit StandaloneWidget(double WidthValue) : Width(WidthValue) {}

  void Grow(double Factor) { Width *= Factor; }
  [[nodiscard]] int Span() const { return static_cast<int>(Width); }
  void SetSpan(int Value) { Width = static_cast<double>(Value); }
  [[nodiscard]] static int Sides() { return 4; }
};

[[nodiscard]] StandaloneWidget StandaloneMakeWidget(double Width) {
  return StandaloneWidget(Width);
}

[[nodiscard]] std::shared_ptr<StandaloneWidget> StandaloneBoxWidget() {
  return std::make_shared<StandaloneWidget>();
}

[[nodiscard]] StandaloneWidget *StandaloneEngineWidget() {
  static StandaloneWidget Engine;
  return &Engine;
}

using Builder = Luna::ClassBuilder<StandaloneWidget>;

static_assert(std::is_same_v<typename Builder::Class, StandaloneWidget>,
              "A class builder names the class it stages.");
static_assert(
    std::is_same_v<decltype(std::declval<Builder &>().Constructor<>()),
                   Builder &>,
    "A default constructor is staged through the same builder.");
static_assert(
    std::is_same_v<decltype(std::declval<Builder &>().Constructor<double>(
                       std::declval<std::string_view>())),
                   Builder &>,
    "A named parameterized constructor is staged through the same builder.");
static_assert(std::is_same_v<decltype(std::declval<Builder &>().Factory(
                                 std::declval<std::string_view>(),
                                 &StandaloneMakeWidget)),
                             Builder &>,
              "A by-value factory is staged through the same builder.");
static_assert(
    std::is_same_v<decltype(std::declval<Builder &>().Factory(
                       std::declval<std::string_view>(), &StandaloneBoxWidget)),
                   Builder &>,
    "A shared factory is staged through the same builder.");
static_assert(std::is_same_v<decltype(std::declval<Builder &>().Singleton(
                                 std::declval<std::string_view>(),
                                 &StandaloneEngineWidget)),
                             Builder &>,
              "A singleton accessor is staged through the same builder.");
static_assert(
    std::is_same_v<decltype(std::declval<Builder &>().Singleton(
                       std::declval<std::string_view>(),
                       &StandaloneEngineWidget,
                       std::declval<Luna::OwnershipPolicy>())),
                   Builder &>,
    "An explicit ownership policy is staged through the same builder.");
static_assert(
    std::is_same_v<decltype(std::declval<Builder &>().Allocator(
                       std::declval<Luna::ClassAllocator>())),
                   Builder &>,
    "A selected storage protocol is staged through the same builder.");
static_assert(std::is_same_v<decltype(std::declval<Builder &>().Commit()),
                             Luna::RegistrationResult>,
              "Committing a class plan yields a registration result.");
static_assert(
    std::is_same_v<decltype(std::declval<const Builder &>().QualifiedName()),
                   std::string_view>,
    "A class builder reports its canonical qualified name.");

static_assert(Luna::Detail::ClassPolicyFor<StandaloneWidget>().ByteCount ==
                  sizeof(StandaloneWidget),
              "A class policy captures the declared storage size.");
static_assert(Luna::Detail::ClassPolicyFor<StandaloneWidget>().IsDestructible,
              "A class policy captures whether Luna could release a value.");

[[nodiscard]] int StandaloneWidgetArea(const StandaloneWidget &Source) {
  return static_cast<int>(Source.Width);
}

static_assert(std::is_same_v<decltype(std::declval<Builder &>().Method(
                                 std::declval<std::string_view>(),
                                 &StandaloneWidget::Grow)),
                             Builder &>,
              "An instance method is staged through the same builder.");
static_assert(std::is_same_v<decltype(std::declval<Builder &>().Method(
                                 std::declval<std::string_view>(),
                                 &StandaloneWidgetArea)),
                             Builder &>,
              "An explicit wrapper method is staged through the same builder.");
static_assert(std::is_same_v<decltype(std::declval<Builder &>().StaticMethod(
                                 std::declval<std::string_view>(),
                                 &StandaloneWidget::Sides)),
                             Builder &>,
              "A static method is staged through the same builder.");
static_assert(std::is_same_v<decltype(std::declval<Builder &>().Property(
                                 std::declval<std::string_view>(),
                                 &StandaloneWidget::Span)),
                             Builder &>,
              "A read-only property is staged through the same builder.");
static_assert(
    std::is_same_v<decltype(std::declval<Builder &>().Property(
                       std::declval<std::string_view>(),
                       &StandaloneWidget::Span, &StandaloneWidget::SetSpan)),
                   Builder &>,
    "A read-write property is staged through the same builder.");
static_assert(
    std::is_same_v<decltype(std::declval<Builder &>().Property(
                       std::declval<std::string_view>(),
                       std::declval<Luna::PropertyPolicy>(),
                       &StandaloneWidget::Span)),
                   Builder &>,
    "An explicit property policy is staged through the same builder.");
static_assert(
    std::is_same_v<decltype(std::declval<Builder &>().Property(
                       std::declval<std::string_view>(),
                       std::declval<Luna::PropertyPolicy>(),
                       &StandaloneWidget::Span, &StandaloneWidget::SetSpan)),
                   Builder &>,
    "An explicitly lazy read-write property is staged through the same "
    "builder.");
static_assert(std::is_same_v<decltype(std::declval<Builder &>().Field(
                                 std::declval<std::string_view>(),
                                 &StandaloneWidget::Width)),
                             Builder &>,
              "A field is staged through the same builder.");
static_assert(std::is_same_v<decltype(std::declval<Builder &>().Field(
                                 std::declval<std::string_view>(),
                                 &StandaloneWidget::Width,
                                 std::declval<Luna::FieldPolicy>())),
                             Builder &>,
              "A field with an explicit policy is staged through the same "
              "builder.");

} // namespace
