// clang-format off
#include "state/invocation/testing/test_hooks.hpp"

#include "state/testing/fault_injector.hpp"

#include <lua.h>
#include <lualib.h>

#include <type_traits>
#include <utility>
// clang-format on

namespace Luna::Detail {
namespace {

class TestState final {
public:
  TestState() : Handle(luaL_newstate()) {}
  ~TestState() {
    if (Handle)
      lua_close(Handle);
  }

  TestState(const TestState &) = delete;
  TestState &operator=(const TestState &) = delete;

  [[nodiscard]] lua_State *Get() const noexcept { return Handle; }

private:
  lua_State *Handle = nullptr;
};

[[nodiscard]] bool Push(lua_State *State, const InvocationTestValue &Input) {
  if (!State || !lua_checkstack(State, 1))
    return false;

  std::visit(
      [State](const auto &TypedValue) {
        using Type = std::decay_t<decltype(TypedValue)>;
        if constexpr (std::is_same_v<Type, std::monostate>)
          lua_pushnil(State);
        else if constexpr (std::is_same_v<Type, bool>)
          lua_pushboolean(State, TypedValue ? 1 : 0);
        else if constexpr (std::is_same_v<Type, int>)
          lua_pushnumber(State, static_cast<double>(TypedValue));
        else if constexpr (std::is_same_v<Type, double>)
          lua_pushnumber(State, TypedValue);
        else
          lua_pushlstring(State, TypedValue.data(), TypedValue.size());
      },
      Input);
  return true;
}

} // namespace

ArgumentReadResult
InvocationPrimitiveTestHooks::Read(const InvocationTestValue &Input,
                                   ValueKind ExpectedKind,
                                   bool InjectInspectionFailure) {
  TestState State;
  if (!Push(State.Get(), Input))
    return {.Status = ArgumentReadStatus::InternalFailure};
  return ReadArgument(State.Get(), 1, ExpectedKind, InjectInspectionFailure);
}

ValidationObservation InvocationPrimitiveTestHooks::Validate(
    const std::vector<InvocationTestValue> &Inputs, std::string GlobalName,
    const CallableMetadata *Metadata, std::size_t MissingMetadataFaults,
    std::size_t ArgumentInspectionFaults) {
  TestState State;
  FaultInjector Faults;
  Faults.Inject(StateFaultPoint::MissingMetadata, MissingMetadataFaults);
  Faults.Inject(StateFaultPoint::ArgumentInspection, ArgumentInspectionFaults);

  bool PushSucceeded = State.Get() != nullptr;
  for (const auto &Input : Inputs)
    PushSucceeded = PushSucceeded && Push(State.Get(), Input);

  ValidationObservation Observation{
      .Invocation = ValidateInvocation(PushSucceeded ? State.Get() : nullptr,
                                       GlobalName, Metadata, Faults),
      .PendingMissingMetadataFaults =
          Faults.Pending(StateFaultPoint::MissingMetadata),
      .PendingArgumentInspectionFaults =
          Faults.Pending(StateFaultPoint::ArgumentInspection)};
  return Observation;
}

ReturnWriteObservation InvocationPrimitiveTestHooks::Write(
    const ReturnMetadata &Metadata, const InvocationOutcome &Outcome,
    std::size_t ReturnWriteFaults, std::size_t VoidFinalizationFaults,
    std::size_t ReturnStackCapacityFaults, std::size_t InitialStackDepth) {
  TestState State;
  FaultInjector Faults;
  Faults.Inject(StateFaultPoint::ReturnStackCapacity,
                ReturnStackCapacityFaults);
  Faults.Inject(StateFaultPoint::ReturnWrite, ReturnWriteFaults);
  Faults.Inject(StateFaultPoint::VoidFinalization, VoidFinalizationFaults);

  bool SeedSucceeded = State.Get() != nullptr;
  for (std::size_t Index = 0; Index < InitialStackDepth; ++Index)
    SeedSucceeded = SeedSucceeded && Push(State.Get(), std::monostate{});

  ReturnWriteObservation Observation;
  Observation.Result = WriteInvocationReturn(
      SeedSucceeded ? State.Get() : nullptr, Metadata, Outcome, Faults);
  Observation.StackDepth = State.Get() ? lua_gettop(State.Get()) : 0;

  if (Observation.Result.Status == ReturnWriteStatus::ValueWritten &&
      Metadata.Kind()) {
    auto ReadBack = ReadArgument(State.Get(), -1, *Metadata.Kind());
    if (ReadBack.IsSuccess())
      Observation.WrittenValue = std::move(*ReadBack.ConvertedValue);
  }
  return Observation;
}

} // namespace Luna::Detail
