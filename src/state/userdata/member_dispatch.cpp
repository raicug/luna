// clang-format off
#include "state/userdata/member_dispatch.hpp"

#include "state/testing/fault_injector.hpp"
#include "state/testing/fault_point.hpp"
#include "state/type/conversion_outcome.hpp"
#include "state/type/structured_conversion.hpp"
#include "state/type/type_generation.hpp"
#include "state/type/type_record.hpp"
#include "state/userdata/access.hpp"
#include "state/userdata/class_operators.hpp"
#include "state/userdata/class_registry.hpp"
#include "state/userdata/exposure.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/member_access.hpp"
#include "state/userdata/member_diagnostics.hpp"

#include <lua.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna::Detail {
namespace {

constexpr int ClassTableUpvalue = 1;
constexpr int ClassNameUpvalue = 2;

constexpr int ReceiverPosition = 1;
constexpr int KeyPosition = 2;
constexpr int IncomingPosition = 3;

[[nodiscard]] MemberDispatchObservation
Refuse(MemberDispatchStage Stage, MemberSideEffectBoundary Boundary,
       bool Reading, std::string Diagnostic) {
  MemberDispatchObservation Observed;
  Observed.Attempted = true;
  Observed.Reading = Reading;
  Observed.Stage = Stage;
  Observed.Boundary = Boundary;
  Observed.Diagnostic = std::move(Diagnostic);
  return Observed;
}

[[nodiscard]] MemberDispatchStage
StageOf(MemberAccessFailure Failure) noexcept {
  switch (Failure) {
  case MemberAccessFailure::None:
    return MemberDispatchStage::Published;
  case MemberAccessFailure::UnavailableRequest:
    return MemberDispatchStage::Request;
  case MemberAccessFailure::UnknownMember:
    return MemberDispatchStage::UnknownMember;
  case MemberAccessFailure::RefusedReceiver:
    return MemberDispatchStage::Receiver;
  case MemberAccessFailure::UnreadableMember:
  case MemberAccessFailure::UnwritableMember:
    return MemberDispatchStage::Direction;
  case MemberAccessFailure::IncompatibleValue:
    return MemberDispatchStage::Value;
  case MemberAccessFailure::RefusedTarget:
  case MemberAccessFailure::ContainedException:
    return MemberDispatchStage::Target;
  }
  return MemberDispatchStage::Request;
}

[[nodiscard]] std::string ClassNameOf(lua_State *State) {
  const char *Name = lua_tostring(State, lua_upvalueindex(ClassNameUpvalue));
  return Name != nullptr ? std::string(Name) : std::string();
}

[[nodiscard]] std::string KeyTextOf(lua_State *State) {
  if (lua_type(State, KeyPosition) == LUA_TSTRING) {
    const char *Text = lua_tostring(State, KeyPosition);
    return Text != nullptr ? std::string(Text) : std::string();
  }
  const char *Name = lua_typename(State, lua_type(State, KeyPosition));
  return Name != nullptr ? std::string(Name) : std::string("?");
}

[[nodiscard]] bool PushClassTableEntry(lua_State *State) {
  lua_pushvalue(State, lua_upvalueindex(ClassTableUpvalue));
  lua_pushvalue(State, KeyPosition);
  lua_rawget(State, -2);
  if (lua_isnil(State, -1)) {
    lua_pop(State, 2);
    return false;
  }
  lua_remove(State, -2);
  return true;
}

[[nodiscard]] UserdataHeader *MutableReceiverHeader(lua_State *State) noexcept {
  if (lua_type(State, ReceiverPosition) != LUA_TUSERDATA)
    return nullptr;
  void *Block = lua_touserdata(State, ReceiverPosition);
  const auto ByteCount =
      static_cast<std::size_t>(lua_objlen(State, ReceiverPosition));
  if (InspectUserdataHeader(Block, ByteCount) == nullptr)
    return nullptr;
  return static_cast<UserdataHeader *>(Block);
}

struct ResolvedMember final {
  const UserdataAccessContext *Context = nullptr;
  const RegisteredClass *Registered = nullptr;
  const RegisteredMember *Member = nullptr;
  UserdataHeader *Header = nullptr;
  std::shared_ptr<const TypeGeneration> Types;
  std::string ClassName;
  std::string MemberName;
  std::string QualifiedName;

  bool Resolved = false;
  MemberDispatchObservation Refusal;
};

[[nodiscard]] ResolvedMember ResolveMember(lua_State *State, bool Reading) {
  ResolvedMember Resolved;
  Resolved.ClassName = ClassNameOf(State);
  Resolved.MemberName = KeyTextOf(State);
  Resolved.QualifiedName = Resolved.ClassName + "." + Resolved.MemberName;

  Resolved.Context = ObserveUserdataAccessContext(State);
  if (Resolved.Context == nullptr || Resolved.Context->Classes == nullptr) {
    Resolved.Refusal =
        Refuse(MemberDispatchStage::Request,
               MemberSideEffectBoundary::BeforeUserCode, Reading,
               DescribeMemberInternalRefusal(
                   Resolved.QualifiedName,
                   "was reached without the access context of its own state."));
    return Resolved;
  }

  Resolved.Registered = Resolved.Context->Classes->Find(Resolved.ClassName);
  if (Resolved.Registered == nullptr || !Resolved.Registered->IsComplete()) {
    Resolved.Refusal = Refuse(
        MemberDispatchStage::Request, MemberSideEffectBoundary::BeforeUserCode,
        Reading,
        DescribeMemberInternalRefusal(Resolved.QualifiedName,
                                      "belongs to no registered class of this "
                                      "state."));
    return Resolved;
  }

  if (lua_type(State, KeyPosition) != LUA_TSTRING ||
      Resolved.Registered->FindMember(Resolved.MemberName) == nullptr) {
    Resolved.Refusal =
        Refuse(MemberDispatchStage::UnknownMember,
               MemberSideEffectBoundary::BeforeUserCode, Reading,
               DescribeUnknownMember(Resolved.ClassName, Resolved.MemberName));
    return Resolved;
  }
  Resolved.Member = Resolved.Registered->FindMember(Resolved.MemberName);

  Resolved.Header = MutableReceiverHeader(State);
  if (Resolved.Header == nullptr) {
    Resolved.Refusal =
        Refuse(MemberDispatchStage::Receiver,
               MemberSideEffectBoundary::BeforeUserCode, Reading,
               DescribeMemberReceiverRefusal(
                   Resolved.Member->QualifiedName, Resolved.ClassName,
                   UserdataAccessFailure::ForeignLayout));
    Resolved.Refusal.Receiver = UserdataAccessFailure::ForeignLayout;
    return Resolved;
  }

  Resolved.Types = Resolved.Context->Types != nullptr
                       ? Resolved.Context->Types->Capture()
                       : TypeGeneration::Foundation();
  if (!Resolved.Types) {
    Resolved.Refusal =
        Refuse(MemberDispatchStage::Request,
               MemberSideEffectBoundary::BeforeUserCode, Reading,
               DescribeMemberInternalRefusal(
                   Resolved.Member->QualifiedName,
                   "was reached without a canonical type registry."));
    return Resolved;
  }

  Resolved.Resolved = true;
  return Resolved;
}

[[nodiscard]] MemberAccessContext
AccessContextFor(const ResolvedMember &Resolved) {
  MemberAccessContext Access;
  Access.Receiver.Origin = Resolved.Context->Origin;
  Access.Receiver.Metatable = Resolved.Registered->Metatable;
  Access.Receiver.RequestedType = Resolved.Registered->Type;
  Access.Receiver.HandleProbe = Resolved.Context->HandleProbe;
  if (Resolved.Context->Classes != nullptr)
    Access.Receiver.Relationships = &Resolved.Context->Classes->Relationships();
  Access.Lazy = Resolved.Context->Lazy;
  Access.Types = Resolved.Types.get();
  Access.DispatchGeneration = Resolved.Context->DispatchGeneration;
  return Access;
}

[[nodiscard]] MemberDispatchObservation DispatchRead(lua_State *State) {
  const ResolvedMember Resolved = ResolveMember(State, true);
  if (!Resolved.Resolved) {
    MemberDispatchObservation Observed = Resolved.Refusal;
    Observed.ClassName = Resolved.ClassName;
    Observed.MemberName = Resolved.MemberName;
    return Observed;
  }

  MemberAccessContext Access = AccessContextFor(Resolved);
  const MemberReadResult Read =
      ReadClassMember(Access, *Resolved.Header, *Resolved.Member);

  MemberDispatchObservation Observed;
  Observed.Attempted = true;
  Observed.Reading = true;
  Observed.ClassName = Resolved.ClassName;
  Observed.MemberName = Resolved.MemberName;
  Observed.Failure = Read.Failure;
  Observed.Receiver = Read.Receiver;
  Observed.ServedFromCache = Read.ServedFromCache;
  Observed.Recorded = Read.Recorded;
  Observed.Stage = StageOf(Read.Failure);
  Observed.Boundary = MemberSideEffectBoundaryOf(Read.Failure);
  if (!Read.IsSuccess()) {
    Observed.Diagnostic = Read.Refusal;
    return Observed;
  }

  const std::string Declared = std::string(
      Resolved.Types->PublicNameOf(Resolved.Member->ValueDescriptor));
  const TypeRecord *Record = Resolved.Types->Find(Resolved.Member->ValueType);
  const bool Injected = Resolved.Context->Faults != nullptr &&
                        Resolved.Context->Faults->Consume(
                            StateFaultPoint::MemberValuePublication);
  if (Injected || Record == nullptr || !Record->IsWritable ||
      Record->Write == nullptr || !lua_checkstack(State, 2) ||
      !Record->Write(State, Read.Produced)) {
    Observed.Stage = MemberDispatchStage::Publication;
    Observed.Boundary = Read.ServedFromCache
                            ? MemberSideEffectBoundary::BeforeUserCode
                            : MemberSideEffectBoundary::AfterUserCode;
    Observed.Diagnostic = DescribeMemberPublicationRefusal(
        Resolved.Member->QualifiedName,
        Declared.empty() ? std::string("its declared type") : Declared);
    return Observed;
  }

  Observed.Succeeded = true;
  Observed.Stage = MemberDispatchStage::Published;
  Observed.PublishedCount = 1;
  return Observed;
}

[[nodiscard]] MemberDispatchObservation DispatchWrite(lua_State *State) {
  const ResolvedMember Resolved = ResolveMember(State, false);
  if (!Resolved.Resolved) {
    MemberDispatchObservation Observed = Resolved.Refusal;
    Observed.ClassName = Resolved.ClassName;
    Observed.MemberName = Resolved.MemberName;
    return Observed;
  }

  MemberAccessContext Access = AccessContextFor(Resolved);
  const std::string Qualified = Resolved.Member->QualifiedName;
  const TypeGeneration &Types = *Resolved.Types;
  const TypeId Declared = Resolved.Member->ValueType;

  const MemberValueSource Source = [State, &Types, &Declared,
                                    &Qualified]() -> MemberValueOutcome {
    const TypeRecord *Record = Types.Find(Declared);
    if (Record == nullptr || !Record->IsReadable || Record->Read == nullptr)
      return MemberValueOutcome::Refuse(DescribeMemberInternalRefusal(
          Qualified, "declares a value type this generation cannot read."));

    const ArgumentReadResult Read = Record->Read(State, IncomingPosition);
    if (Read.IsSuccess())
      return MemberValueOutcome::Accept(*Read.ConvertedValue);

    const StructuredDiagnostic Diagnostic =
        StructuredDiagnosticFrom(Read, Record->PublicName, std::string());
    return MemberValueOutcome::Refuse(
        DescribeMemberValueRefusal(Qualified, Diagnostic));
  };

  const MemberWriteResult Written =
      WriteClassMember(Access, *Resolved.Header, *Resolved.Member, Source);

  MemberDispatchObservation Observed;
  Observed.Attempted = true;
  Observed.Reading = false;
  Observed.ClassName = Resolved.ClassName;
  Observed.MemberName = Resolved.MemberName;
  Observed.Failure = Written.Failure;
  Observed.Receiver = Written.Receiver;
  Observed.Invalidated = Written.Invalidated;
  Observed.Stage = StageOf(Written.Failure);
  Observed.Boundary = MemberSideEffectBoundaryOf(Written.Failure);
  if (!Written.IsSuccess()) {
    Observed.Diagnostic = Written.Refusal;
    return Observed;
  }

  Observed.Succeeded = true;
  Observed.Stage = MemberDispatchStage::Published;
  return Observed;
}

constexpr std::size_t LocalDiagnosticCapacity = 512;

[[nodiscard]] MemberDispatchObservation
RefuseMethodAssignment(lua_State *State) {
  const std::string ClassName = ClassNameOf(State);
  const std::string MemberName = KeyTextOf(State);
  MemberDispatchObservation Observed =
      Refuse(MemberDispatchStage::UnknownMember,
             MemberSideEffectBoundary::BeforeUserCode, false,
             DescribeMemberMethodAssignment(ClassName + "." + MemberName));
  Observed.ClassName = ClassName;
  Observed.MemberName = MemberName;
  return Observed;
}

[[nodiscard]] bool PushDeclaredKeyOperator(lua_State *State, bool Reading) {
  std::string_view Segment;
  {
    const UserdataAccessContext *Context = ObserveUserdataAccessContext(State);
    if (Context == nullptr || Context->Classes == nullptr)
      return false;
    const RegisteredClass *Registered =
        Context->Classes->Find(ClassNameOf(State));
    if (Registered == nullptr || !Registered->IsComplete())
      return false;
    if (lua_type(State, KeyPosition) == LUA_TSTRING &&
        Registered->FindMember(KeyTextOf(State)) != nullptr)
      return false;

    const ClassOperator Selected =
        Reading ? ClassOperator::Index : ClassOperator::Assign;
    if (Registered->FindOperator(Selected) == nullptr)
      return false;
    const ClassOperatorDescriptor *Described = FindClassOperator(Selected);
    if (Described == nullptr)
      return false;

    Segment = Described->Segment;
  }

  if (!lua_checkstack(State, 6))
    return false;
  lua_pushvalue(State, lua_upvalueindex(ClassTableUpvalue));
  lua_pushlstring(State, Segment.data(), Segment.size());
  lua_rawget(State, -2);
  lua_remove(State, -2);
  if (!lua_isfunction(State, -1)) {
    lua_pop(State, 1);
    return false;
  }
  return true;
}

template <bool Reading> [[nodiscard]] int DispatchMember(lua_State *State) {
  if (State == nullptr)
    return 0;

  const int EntryDepth = lua_gettop(State);
  char Local[LocalDiagnosticCapacity]{};
  char *Prepared = Local;
  std::size_t Length = 0;
  bool HeapDiagnostic = false;
  FaultInjector *Faults = nullptr;
  MemberDispatchRecorder *Dispatch = nullptr;
  int Published = -1;
  bool ForwardToOperator = false;

  {
    try {
      bool ClassTableHolds = false;
      if (PushClassTableEntry(State)) {
        if constexpr (Reading)
          return 1;
        lua_pop(State, 1);
        ClassTableHolds = true;
      }

      const UserdataAccessContext *Context =
          ObserveUserdataAccessContext(State);
      if (Context != nullptr) {
        Faults = Context->Faults;
        Dispatch = Context->Dispatch;
      }

      if (!ClassTableHolds && PushDeclaredKeyOperator(State, Reading)) {
        MemberDispatchObservation Observed;
        Observed.Attempted = true;
        Observed.Reading = Reading;
        Observed.Succeeded = true;
        Observed.ServedByOperator = true;
        Observed.Stage = MemberDispatchStage::Published;
        Observed.ClassName = ClassNameOf(State);
        Observed.MemberName = KeyTextOf(State);
        Observed.EntryDepth = EntryDepth;
        Observed.RestoredDepth = EntryDepth;
        Observed.PublishedCount = Reading ? 1 : 0;
        if (Dispatch != nullptr)
          Dispatch->Record(std::move(Observed));
        ForwardToOperator = true;
      } else {
        MemberDispatchObservation Observed;
        if constexpr (Reading) {
          Observed = DispatchRead(State);
        } else if (ClassTableHolds) {
          Observed = RefuseMethodAssignment(State);
        } else {
          Observed = DispatchWrite(State);
        }

        Observed.EntryDepth = EntryDepth;
        Observed.RestoredDepth = EntryDepth;
        if (Observed.Succeeded) {
          Published = Observed.PublishedCount;
        } else {
          Length = Observed.Diagnostic.size();
          if (Length > LocalDiagnosticCapacity) {
            Prepared = static_cast<char *>(std::malloc(Length));
            HeapDiagnostic = Prepared != nullptr;
          }
          if (Prepared == nullptr)
            Length = 0;
          else if (Length != 0)
            std::memcpy(Prepared, Observed.Diagnostic.data(), Length);
        }
        if (Dispatch != nullptr)
          Dispatch->Record(std::move(Observed));
      }
    } catch (...) {
      Published = -1;
      Length = 0;
      ForwardToOperator = false;
      if (HeapDiagnostic)
        std::free(Prepared);
      Prepared = Local;
      HeapDiagnostic = false;
    }
  }

  if (ForwardToOperator) {
    const int Forwarded = Reading ? 2 : 3;
    const int Results = Reading ? 1 : 0;
    lua_pushvalue(State, ReceiverPosition);
    lua_pushvalue(State, KeyPosition);
    if constexpr (!Reading)
      lua_pushvalue(State, IncomingPosition);
    lua_call(State, Forwarded, Results);
    return Results;
  }

  if (Published >= 0) {
    lua_settop(State, EntryDepth + Published);
    return Published;
  }

  if (Length == 0) {
    static constexpr char Fallback[] = "Luna: the member access was refused.";
    Prepared = Local;
    Length = sizeof(Fallback) - 1;
    std::memcpy(Prepared, Fallback, Length);
  }

  lua_settop(State, EntryDepth);
  const int RestoredDepth = lua_gettop(State);
  lua_pushlstring(State, Prepared, Length);
  if (Faults != nullptr)
    Faults->RecordCallbackStackRestoration(EntryDepth, RestoredDepth,
                                           lua_gettop(State));
  if (Dispatch != nullptr)
    Dispatch->NoteRestoredDepth(RestoredDepth);
  if (HeapDiagnostic)
    std::free(Prepared);
  lua_error(State);
  return 0;
}

[[nodiscard]] int IndexClassValue(lua_State *State) {
  return DispatchMember<true>(State);
}

[[nodiscard]] int AssignClassValue(lua_State *State) {
  return DispatchMember<false>(State);
}

} // namespace

std::string_view MemberDispatchStageText(MemberDispatchStage Stage) noexcept {
  switch (Stage) {
  case MemberDispatchStage::Published:
    return "published";
  case MemberDispatchStage::Request:
    return "request";
  case MemberDispatchStage::UnknownMember:
    return "unknown_member";
  case MemberDispatchStage::Receiver:
    return "receiver";
  case MemberDispatchStage::Direction:
    return "direction";
  case MemberDispatchStage::Value:
    return "value";
  case MemberDispatchStage::Target:
    return "target";
  case MemberDispatchStage::Publication:
    return "publication";
  }
  return "request";
}

void MemberDispatchRecorder::Clear() noexcept { LastObservation.reset(); }

void MemberDispatchRecorder::Record(MemberDispatchObservation Observed) {
  LastObservation = std::move(Observed);
}

void MemberDispatchRecorder::NoteRestoredDepth(int RestoredDepth) noexcept {
  if (LastObservation)
    LastObservation->RestoredDepth = RestoredDepth;
}

const MemberDispatchObservation *MemberDispatchRecorder::Last() const noexcept {
  return LastObservation ? &*LastObservation : nullptr;
}

bool InstallClassMemberDispatch(lua_State *State, int MetatableIndex,
                                int ClassTableIndex,
                                const std::string &QualifiedName) {
  if (State == nullptr || !lua_checkstack(State, 6))
    return false;

  lua_pushvalue(State, ClassTableIndex);
  lua_pushstring(State, QualifiedName.c_str());
  lua_pushcclosure(State, IndexClassValue, "Luna.ClassMemberIndex", 2);
  lua_rawsetfield(State, MetatableIndex, "__index");

  lua_pushvalue(State, ClassTableIndex);
  lua_pushstring(State, QualifiedName.c_str());
  lua_pushcclosure(State, AssignClassValue, "Luna.ClassMemberAssign", 2);
  lua_rawsetfield(State, MetatableIndex, "__newindex");
  return true;
}

} // namespace Luna::Detail
