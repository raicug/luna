// clang-format off
#include "state/invocation/delegate/vm_delegate.hpp"

#include <luna/binding/value.hpp>

#include "state/invocation/conversion/argument_reader.hpp"
#include "state/type/conversion_outcome.hpp"
#include "state/type/owned_value_bridge.hpp"
#include "state/type/type_generation.hpp"
#include "state/type/type_record.hpp"
#include "state/vm/stack_checkpoint.hpp"

#include <lua.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
// clang-format on

namespace Luna::Detail {
namespace {

constexpr const char *DelegateRegistrySlot = "Luna.Delegates";

[[nodiscard]] std::string HandlerFailureText(lua_State *State) {
  if (!State)
    return "the subscribed handler failed without a reason.";
  if (const char *Reported = lua_tostring(State, -1))
    return std::string("the subscribed handler reported: ") + Reported;
  return "the subscribed handler failed without a reason.";
}

class VmDelegateTarget final : public DelegateTarget {
public:
  VmDelegateTarget(std::shared_ptr<DelegateLink> Link, int Reference,
                   std::uint64_t Epoch, std::uint64_t Identity,
                   DelegateShape Declared) noexcept
      : LinkValue(std::move(Link)), ReferenceValue(Reference),
        EpochValue(Epoch), IdentityValue(Identity),
        ShapeValue(std::move(Declared)) {}

  ~VmDelegateTarget() override { Release(); }

  [[nodiscard]] bool IsLive() const noexcept override {
    if (!LinkValue || ReleasedValue)
      return false;
    const std::lock_guard<std::mutex> Guard(LinkValue->Barrier);
    return LinkValue->Alive && LinkValue->Epoch == EpochValue;
  }

  [[nodiscard]] std::uint64_t Identity() const noexcept override {
    return IdentityValue;
  }

  [[nodiscard]] DelegateCallResult
  Call(std::span<const Value> Arguments) override {
    lua_State *State = nullptr;
    if (std::optional<DelegateCallResult> Refused = Enter(State))
      return std::move(*Refused);

    if (Arguments.size() != ShapeValue.Parameters.size())
      return Refuse(DelegateStatus::ResultMismatch,
                    "the subscribed handler received an argument count its "
                    "declared shape does not accept.");
    if (ShapeValue.CarriesObjects())
      return Refuse(DelegateStatus::ResultMismatch,
                    "this subscribed handler declares arguments carrying "
                    "objects, which are staged as owned values.");

    const std::shared_ptr<const TypeGeneration> Types =
        TypeGeneration::Foundation();
    if (!Types)
      return Refuse(DelegateStatus::HandlerFailed,
                    "the subscribed handler has no captured type registry.");

    try {
      StackCheckpoint Checkpoint(State);
      const int Requested = static_cast<int>(Arguments.size()) + 2;
      if (!lua_checkstack(State, Requested))
        return Refuse(DelegateStatus::HandlerFailed,
                      "the subscribed handler could not reserve stack "
                      "capacity.");

      lua_getref(State, ReferenceValue);
      if (lua_type(State, -1) != LUA_TFUNCTION)
        return Refuse(DelegateStatus::Released,
                      "the subscribed handler is no longer a function.");

      for (std::size_t Index = 0; Index < Arguments.size(); ++Index) {
        const TypeRecord *Record =
            Types->Find(ShapeValue.Parameters[Index].Kind);
        if (!Record || !Record->IsWritable || !Record->Write)
          return Refuse(DelegateStatus::HandlerFailed,
                        "the subscribed handler declares an argument type "
                        "that is unavailable in the type registry.");
        if (!Record->Write(State, Arguments[Index]))
          return Refuse(DelegateStatus::HandlerFailed,
                        "the subscribed handler could not receive argument " +
                            std::to_string(Index + 1) + ".");
      }

      const int ResultCount = ShapeValue.Result ? 1 : 0;
      if (lua_pcall(State, static_cast<int>(Arguments.size()), ResultCount,
                    0) != LUA_OK)
        return Refuse(DelegateStatus::HandlerFailed, HandlerFailureText(State));

      if (!ShapeValue.Result)
        return Deliver(std::nullopt);

      const ArgumentReadResult Read =
          ReadArgument(*Types, State, -1, *ShapeValue.Result);
      if (!Read.IsSuccess())
        return Refuse(DelegateStatus::ResultMismatch,
                      "the subscribed handler published no value of its "
                      "declared result type.");
      return Deliver(*Read.ConvertedValue);
    } catch (const std::exception &Error) {
      return Refuse(DelegateStatus::HandlerFailed,
                    std::string("the subscribed handler reported: ") +
                        Error.what());
    } catch (...) {
      return Refuse(DelegateStatus::HandlerFailed,
                    "the subscribed handler reported an unknown failure.");
    }
  }

  [[nodiscard]] DelegateCallResult
  CallOwned(std::span<const OwnedValue> Arguments) override {
    lua_State *State = nullptr;
    if (std::optional<DelegateCallResult> Refused = Enter(State))
      return std::move(*Refused);

    if (Arguments.size() < ShapeValue.FixedParameterCount() ||
        (!ShapeValue.CarriesPack() &&
         Arguments.size() != ShapeValue.Parameters.size()))
      return Refuse(DelegateStatus::ResultMismatch,
                    "the subscribed handler received an argument count its "
                    "declared shape does not accept.");

    const std::shared_ptr<const TypeGeneration> Types =
        CaptureOwnedValueTypes(State);
    if (!Types)
      return Refuse(DelegateStatus::HandlerFailed,
                    "the subscribed handler has no captured type registry.");

    for (std::size_t Index = 0; Index < Arguments.size(); ++Index) {
      const std::string Refusal =
          ClassifyPendingInstances(Arguments[Index], *Types);
      if (!Refusal.empty())
        return Refuse(DelegateStatus::HandlerFailed,
                      "the subscribed handler cannot receive argument " +
                          std::to_string(Index + 1) + ": " + Refusal);
    }

    try {
      StackCheckpoint Checkpoint(State);
      const int Requested = static_cast<int>(Arguments.size()) + 3;
      if (!lua_checkstack(State, Requested))
        return Refuse(DelegateStatus::HandlerFailed,
                      "the subscribed handler could not reserve stack "
                      "capacity.");

      lua_getref(State, ReferenceValue);
      if (lua_type(State, -1) != LUA_TFUNCTION)
        return Refuse(DelegateStatus::Released,
                      "the subscribed handler is no longer a function.");

      for (std::size_t Index = 0; Index < Arguments.size(); ++Index) {
        if (!PushOwnedValueToStack(State, Arguments[Index], *Types))
          return Refuse(DelegateStatus::HandlerFailed,
                        "the subscribed handler could not receive argument " +
                            std::to_string(Index + 1) + ".");
      }

      const int ResultCount = ShapeValue.Result ? 1 : 0;
      if (lua_pcall(State, static_cast<int>(Arguments.size()), ResultCount,
                    0) != LUA_OK)
        return Refuse(DelegateStatus::HandlerFailed, HandlerFailureText(State));

      if (!ShapeValue.Result)
        return Deliver(std::nullopt);

      const ArgumentReadResult Read =
          ReadArgument(*Types, State, -1, *ShapeValue.Result);
      if (!Read.IsSuccess())
        return Refuse(DelegateStatus::ResultMismatch,
                      "the subscribed handler published no value of its "
                      "declared result type.");
      return Deliver(*Read.ConvertedValue);
    } catch (const std::exception &Error) {
      return Refuse(DelegateStatus::HandlerFailed,
                    std::string("the subscribed handler reported: ") +
                        Error.what());
    } catch (...) {
      return Refuse(DelegateStatus::HandlerFailed,
                    "the subscribed handler reported an unknown failure.");
    }
  }

  void Release() noexcept override {
    if (!LinkValue || ReleasedValue)
      return;
    ReleasedValue = true;

    const std::lock_guard<std::mutex> Guard(LinkValue->Barrier);

    if (LinkValue->Epoch != EpochValue)
      return;

    const auto Found = std::find(LinkValue->Outstanding.begin(),
                                 LinkValue->Outstanding.end(), ReferenceValue);
    if (Found == LinkValue->Outstanding.end())
      return;
    LinkValue->Outstanding.erase(Found);
    LinkValue->Counters.Released += 1;
    if (LinkValue->Alive && LinkValue->Thread &&
        std::this_thread::get_id() == LinkValue->Owner)
      lua_unref(LinkValue->Thread, ReferenceValue);
  }

private:
  [[nodiscard]] std::optional<DelegateCallResult> Enter(lua_State *&State) {
    if (!LinkValue)
      return DelegateCallResult::Refused(
          DelegateStatus::Released,
          "the subscribed handler is no longer available.");

    {
      const std::lock_guard<std::mutex> Guard(LinkValue->Barrier);
      if (ReleasedValue || !LinkValue->Alive ||
          LinkValue->Epoch != EpochValue) {
        LinkValue->Counters.Failures += 1;
        return DelegateCallResult::Refused(
            DelegateStatus::Released,
            "the subscribed handler was released before this call.");
      }
      if (std::this_thread::get_id() != LinkValue->Owner) {
        LinkValue->Counters.ForeignThreadRefusals += 1;
        LinkValue->Counters.Failures += 1;
        return DelegateCallResult::Refused(
            DelegateStatus::ForeignThread,
            "a subscribed handler runs only on the thread that owns its "
            "State.");
      }
      State = LinkValue->Thread;
      LinkValue->Counters.Invocations += 1;
    }

    if (!State)
      return Refuse(DelegateStatus::Released,
                    "the subscribed handler has no virtual machine.");
    return std::nullopt;
  }

  [[nodiscard]] DelegateCallResult Deliver(std::optional<Value> Produced) {
    return DelegateCallResult::Delivered(std::move(Produced));
  }

  [[nodiscard]] DelegateCallResult Refuse(DelegateStatus Status,
                                          std::string Diagnostic) {
    if (LinkValue) {
      const std::lock_guard<std::mutex> Guard(LinkValue->Barrier);
      LinkValue->Counters.Failures += 1;
    }
    return DelegateCallResult::Refused(Status, std::move(Diagnostic));
  }

  std::shared_ptr<DelegateLink> LinkValue;
  int ReferenceValue = 0;
  std::uint64_t EpochValue = 0;
  std::uint64_t IdentityValue = 0;
  DelegateShape ShapeValue;
  bool ReleasedValue = false;
};

} // namespace

VmDelegateRegistry::VmDelegateRegistry()
    : LinkValue(std::make_shared<DelegateLink>()) {}

VmDelegateRegistry::~VmDelegateRegistry() { Retire(); }

void VmDelegateRegistry::Bind(lua_State *Root) noexcept {
  if (!LinkValue)
    return;
  const std::lock_guard<std::mutex> Guard(LinkValue->Barrier);
  LinkValue->Thread = Root;
  LinkValue->Owner = std::this_thread::get_id();
  LinkValue->Alive = Root != nullptr;
}

std::shared_ptr<DelegateTarget>
VmDelegateRegistry::Adopt(lua_State *State, int StackIndex,
                          const DelegateShape &Declared) {
  if (!LinkValue || !State)
    return nullptr;

  StackCheckpoint Checkpoint(State);
  if (!lua_checkstack(State, 2))
    return nullptr;
  if (lua_type(State, StackIndex) != LUA_TFUNCTION)
    return nullptr;

  lua_pushvalue(State, StackIndex);
  const int Reference = lua_ref(State, -1);
  if (Reference <= 0)
    return nullptr;

  std::uint64_t Epoch = 0;
  std::uint64_t Identity = 0;
  {
    const std::lock_guard<std::mutex> Guard(LinkValue->Barrier);
    if (!LinkValue->Alive) {
      lua_unref(State, Reference);
      return nullptr;
    }
    Epoch = LinkValue->Epoch;
    Identity = LinkValue->NextIdentity;
    LinkValue->NextIdentity += 1;
    LinkValue->Outstanding.push_back(Reference);
    LinkValue->Counters.Adopted += 1;
  }

  return std::make_shared<VmDelegateTarget>(LinkValue, Reference, Epoch,
                                            Identity, Declared);
}

std::size_t VmDelegateRegistry::InvalidateEverything() noexcept {
  if (!LinkValue)
    return 0;
  const std::lock_guard<std::mutex> Guard(LinkValue->Barrier);
  const std::size_t Invalidated = LinkValue->Outstanding.size();
  if (LinkValue->Alive && LinkValue->Thread &&
      std::this_thread::get_id() == LinkValue->Owner) {
    for (const int Reference : LinkValue->Outstanding)
      lua_unref(LinkValue->Thread, Reference);
  }
  LinkValue->Outstanding.clear();
  LinkValue->Epoch += 1;
  LinkValue->Counters.Invalidated += Invalidated;
  return Invalidated;
}

void VmDelegateRegistry::Retire() noexcept {
  if (!LinkValue)
    return;
  const std::lock_guard<std::mutex> Guard(LinkValue->Barrier);
  LinkValue->Counters.Invalidated += LinkValue->Outstanding.size();
  LinkValue->Outstanding.clear();
  LinkValue->Alive = false;
  LinkValue->Thread = nullptr;
}

std::size_t VmDelegateRegistry::OutstandingCount() const noexcept {
  if (!LinkValue)
    return 0;
  const std::lock_guard<std::mutex> Guard(LinkValue->Barrier);
  return LinkValue->Outstanding.size();
}

DelegateCounters VmDelegateRegistry::Counters() const noexcept {
  if (!LinkValue)
    return DelegateCounters();
  const std::lock_guard<std::mutex> Guard(LinkValue->Barrier);
  return LinkValue->Counters;
}

bool PublishDelegateRegistry(lua_State *State,
                             VmDelegateRegistry *Registry) noexcept {
  if (!State || !Registry)
    return false;
  if (!lua_checkstack(State, 4))
    return false;

  StackCheckpoint Checkpoint(State);
  lua_pushlightuserdata(State, Registry);
  lua_rawsetfield(State, LUA_REGISTRYINDEX, DelegateRegistrySlot);
  return true;
}

VmDelegateRegistry *ObserveDelegateRegistry(lua_State *State) noexcept {
  if (!State || !lua_checkstack(State, 2))
    return nullptr;

  StackCheckpoint Checkpoint(State);
  lua_rawgetfield(State, LUA_REGISTRYINDEX, DelegateRegistrySlot);
  return static_cast<VmDelegateRegistry *>(lua_tolightuserdata(State, -1));
}

} // namespace Luna::Detail
