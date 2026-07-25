// clang-format off
#include <luna/binding/callable_descriptor.hpp>
#include <luna/binding/callable_metadata.hpp>
#include <luna/binding/supported_callable.hpp>
#include <luna/binding/value.hpp>
#include <luna/detail/callable_adapter.hpp>

#include <array>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
// clang-format on

namespace {

bool Negate(bool Value) { return !Value; }
int AddOne(int Value) { return Value + 1; }
double Half(double Value) noexcept { return Value / 2.0; }
std::string Echo(std::string Value) { return Value; }
void Consume(bool, int, double, std::string) {}
int Variadic(int Value, ...) { return Value; }
int ReferenceParameter(int &Value) { return Value; }
const std::string &ReferenceReturn(std::string Value) {
  static std::string Stored;
  Stored = std::move(Value);
  return Stored;
}
std::optional<int> OptionalReturn() { return 1; }
int OptionalParameter(std::optional<int> Value) { return Value.value_or(0); }
unsigned int UnsupportedReturn() { return 1U; }
int UnsupportedPointerParameter(const int *Value) {
  return Value == nullptr ? 0 : *Value;
}
int ConstReferenceParameter(const int &Value) { return Value; }
int RvalueReferenceParameter(std::string &&Value) {
  return static_cast<int>(Value.size());
}

struct OverloadedCallable {
  int operator()(int Value) const { return Value; }
  double operator()(double Value) const { return Value; }
};

struct Owner {
  int Data = 0;
  int Method(int Value) { return Value; }
};

static_assert(Luna::SupportedValue<bool>);
static_assert(Luna::SupportedValue<int>);
static_assert(Luna::SupportedValue<double>);
static_assert(Luna::SupportedValue<std::string>);
static_assert(!Luna::SupportedValue<const int>);
static_assert(!Luna::SupportedValue<int &>);
static_assert(!Luna::SupportedValue<unsigned int>);

static_assert(Luna::SupportedCallable<decltype(Negate)>);
static_assert(Luna::SupportedCallable<decltype(&Negate)>);
static_assert(Luna::SupportedCallable<decltype(&AddOne)>);
static_assert(Luna::SupportedCallable<decltype(&Half)>);
static_assert(Luna::SupportedCallable<decltype(&Echo)>);
static_assert(Luna::SupportedCallable<decltype(&Consume)>);
static_assert(!Luna::SupportedCallable<decltype(&Variadic)>);
static_assert(!Luna::SupportedCallable<decltype(&ReferenceParameter)>);
static_assert(!Luna::SupportedCallable<decltype(&ConstReferenceParameter)>);
static_assert(!Luna::SupportedCallable<decltype(&RvalueReferenceParameter)>);
static_assert(!Luna::SupportedCallable<decltype(&ReferenceReturn)>);
static_assert(!Luna::SupportedCallable<decltype(&OptionalParameter)>);
static_assert(!Luna::SupportedCallable<decltype(&OptionalReturn)>);
static_assert(!Luna::SupportedCallable<decltype(&UnsupportedReturn)>);
static_assert(!Luna::SupportedCallable<decltype(&UnsupportedPointerParameter)>);
static_assert(!Luna::SupportedCallable<decltype(&Owner::Data)>);
static_assert(!Luna::SupportedCallable<decltype(&Owner::Method)>);
static_assert(!Luna::SupportedCallable<OverloadedCallable>);

template <class Descriptor>
bool HasReturn(const Descriptor &Value, Luna::ReturnDisposition Disposition,
               const Luna::ValueKind *Kind) {
  const auto &Return = Value.Metadata().ReturnType();
  if (Return.Disposition() != Disposition)
    return false;
  if (Kind == nullptr)
    return Return.Kind() == nullptr;
  return Return.Kind() != nullptr && *Return.Kind() == *Kind;
}

} // namespace

int RunCallableModelTests() {
  const auto Generic = [](auto Value) { return Value; };
  const auto Overloaded = OverloadedCallable{};
  const auto Reference = [](const std::string &Value) { return Value.size(); };
  const auto Unsupported = [](unsigned int Value) { return Value; };
  const auto VariadicLambda = [](int Value, ...) { return Value; };
  const auto MutableLambda = [Total = 0](int Value) mutable noexcept {
    Total += Value;
    return Total;
  };
  const auto Concrete =
      [Prefix = std::string("value:")](bool Enabled, int Count, double Number,
                                       std::string Text) {
        return Prefix + (Enabled ? "true:" : "false:") + std::to_string(Count) +
               ":" + std::to_string(Number) + ":" + Text;
      };

  static_assert(!Luna::SupportedCallable<decltype(Generic)>);
  static_assert(!Luna::SupportedCallable<decltype(Overloaded)>);
  static_assert(!Luna::SupportedCallable<decltype(Reference)>);
  static_assert(!Luna::SupportedCallable<decltype(Unsupported)>);
  static_assert(!Luna::SupportedCallable<decltype(VariadicLambda)>);
  static_assert(Luna::SupportedCallable<decltype(MutableLambda)>);
  static_assert(Luna::SupportedCallable<decltype(Concrete)>);

  auto ConcreteDescriptor =
      Luna::Detail::MakeErasedCallableDescriptor(Concrete);
  const std::array ExpectedKinds{
      Luna::ValueKind::Boolean, Luna::ValueKind::Integer,
      Luna::ValueKind::Number, Luna::ValueKind::String};
  const auto ParameterTypes = ConcreteDescriptor.Metadata().ParameterTypes();
  if (ParameterTypes.size() != ExpectedKinds.size())
    return 1;
  for (std::size_t Index = 0; Index < ExpectedKinds.size(); ++Index) {
    if (ParameterTypes[Index] != ExpectedKinds[Index])
      return 2;
  }

  const auto StringKind = Luna::ValueKind::String;
  if (!HasReturn(ConcreteDescriptor, Luna::ReturnDisposition::Value,
                 &StringKind))
    return 3;

  const std::array<Luna::Value, 4> Arguments{Luna::Value{true}, Luna::Value{7},
                                             Luna::Value{2.5},
                                             Luna::Value{std::string("text")}};
  const auto ConcreteOutcome = ConcreteDescriptor.Invoke(Arguments);
  if (ConcreteOutcome.Kind() != Luna::InvocationOutcomeKind::Value ||
      ConcreteOutcome.ReturnedValue() == nullptr ||
      std::get<std::string>(*ConcreteOutcome.ReturnedValue()) !=
          "value:true:7:2.500000:text")
    return 4;

  auto MoveOnly = [Owned = std::make_unique<int>(40)](int Value) {
    return *Owned + Value;
  };
  static_assert(Luna::SupportedCallable<decltype(MoveOnly)>);
  static_assert(!Luna::SupportedCallable<decltype(MoveOnly) &>);
  auto MoveOnlyDescriptor =
      Luna::Detail::MakeErasedCallableDescriptor(std::move(MoveOnly));
  const std::array<Luna::Value, 1> IntegerArgument{Luna::Value{2}};
  const auto MoveOnlyOutcome = MoveOnlyDescriptor.Invoke(IntegerArgument);
  if (MoveOnlyOutcome.ReturnedValue() == nullptr ||
      std::get<int>(*MoveOnlyOutcome.ReturnedValue()) != 42)
    return 5;

  auto BooleanDescriptor = Luna::Detail::MakeErasedCallableDescriptor(Negate);
  auto IntegerDescriptor = Luna::Detail::MakeErasedCallableDescriptor(&AddOne);
  auto NumberDescriptor = Luna::Detail::MakeErasedCallableDescriptor(&Half);
  auto StringDescriptor = Luna::Detail::MakeErasedCallableDescriptor(&Echo);
  auto VoidDescriptor = Luna::Detail::MakeErasedCallableDescriptor(&Consume);
  const auto BooleanKind = Luna::ValueKind::Boolean;
  const auto IntegerKind = Luna::ValueKind::Integer;
  const auto NumberKind = Luna::ValueKind::Number;
  if (!HasReturn(BooleanDescriptor, Luna::ReturnDisposition::Value,
                 &BooleanKind) ||
      !HasReturn(IntegerDescriptor, Luna::ReturnDisposition::Value,
                 &IntegerKind) ||
      !HasReturn(NumberDescriptor, Luna::ReturnDisposition::Value,
                 &NumberKind) ||
      !HasReturn(StringDescriptor, Luna::ReturnDisposition::Value,
                 &StringKind) ||
      !HasReturn(VoidDescriptor, Luna::ReturnDisposition::Void, nullptr))
    return 6;

  const std::array<Luna::Value, 4> VoidArguments{
      Luna::Value{false}, Luna::Value{0}, Luna::Value{0.0},
      Luna::Value{std::string{}}};
  if (VoidDescriptor.Invoke(VoidArguments).Kind() !=
      Luna::InvocationOutcomeKind::Void)
    return 7;

  using FunctionPointer = int (*)(int);
  FunctionPointer NullTarget = nullptr;
  auto NullDescriptor = Luna::Detail::MakeErasedCallableDescriptor(NullTarget);
  if (NullDescriptor.HasTarget() ||
      NullDescriptor.Invoke(IntegerArgument).Kind() !=
          Luna::InvocationOutcomeKind::InternalFailure)
    return 8;

  auto ThrowingDescriptor = Luna::Detail::MakeErasedCallableDescriptor(
      [](int) -> int { throw std::runtime_error("expected failure"); });
  try {
    static_cast<void>(ThrowingDescriptor.Invoke(IntegerArgument));
    return 9;
  } catch (const std::runtime_error &Error) {
    if (std::string(Error.what()) != "expected failure")
      return 10;
  }

  auto MovedDescriptor = std::move(IntegerDescriptor);
  if (!MovedDescriptor.HasTarget() || IntegerDescriptor.HasTarget())
    return 11;

  const auto MissingArguments = MovedDescriptor.Invoke({});
  if (MissingArguments.Kind() != Luna::InvocationOutcomeKind::InternalFailure ||
      MissingArguments.FailureMessage().empty())
    return 12;

  return 0;
}
