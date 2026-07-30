// clang-format off
#include "state/invocation/trampoline.hpp"

#include "state/dispatch/closure_slot.hpp"
#include "state/identity/identity_registry.hpp"
#include "state/invocation/async/suspended_call.hpp"
#include "state/invocation/conversion/return_writer.hpp"
#include "state/invocation/members/receiver.hpp"
#include "state/invocation/overload/dispatch.hpp"
#include "state/invocation/parameters/parameter_binding.hpp"
#include "state/invocation/validation/validator.hpp"
#include "state/registration/record.hpp"
#include "state/testing/fault_injector.hpp"
#include "state/tooling/profiling_registry.hpp"
#include "state/type/structured_conversion.hpp"
#include "state/type/type_generation.hpp"

#include <lua.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna::Detail {
namespace {

struct InvocationResult final {
  int ReturnCount = -1;
  std::string Diagnostic;
  std::unique_ptr<StartedAsyncCall> Suspension;

  SymbolId Symbol;
  TypeId ReceiverType;

  [[nodiscard]] bool IsSuccess() const noexcept { return ReturnCount >= 0; }
  [[nodiscard]] bool IsSuspended() const noexcept {
    return Suspension != nullptr;
  }
};

[[nodiscard]] std::string CallableContext(std::string_view GlobalName) {
  return DescribeConversionSubjectContext(
      SubjectForCallable(GlobalName, false));
}

[[nodiscard]] std::string MemberContext(std::string_view GlobalName,
                                        bool IsInstanceMember) {
  return DescribeConversionSubjectContext(
      SubjectForCallable(GlobalName, IsInstanceMember));
}

[[nodiscard]] InvocationResult Failure(std::string Message) {
  if (Message.empty())
    Message = "Internal Luna invocation error.";
  return {.ReturnCount = -1, .Diagnostic = std::move(Message)};
}

[[nodiscard]] ArgumentPack RetainedArguments(std::span<const Value> Supplied,
                                             std::size_t FirstPosition) {
  ValuePack Owned;
  for (const Value &Element : Supplied)
    Owned.Append(OwnedValue::FromValue(Element));
  return ArgumentPack(std::move(Owned), FirstPosition);
}

[[nodiscard]] std::unique_ptr<StartedAsyncCall>
StartedCallFrom(InvocationOutcome &Outcome, const ReturnMetadata &Awaited,
                ArgumentPack Arguments, const SymbolId &Symbol) {
  auto Started = std::make_unique<StartedAsyncCall>();
  Started->Work = Outcome.TakeSuspendedWork();
  Started->Arguments = std::move(Arguments);
  Started->Awaited = Awaited;
  Started->Symbol = Symbol;
  return Started;
}

struct SelectedOverload final {
  OverloadCandidate *Candidate = nullptr;
  std::string Diagnostic;
};

[[nodiscard]] SelectedOverload
SelectOverload(lua_State *State, BindingRecord &Record,
               const TypeGeneration &Types, const InstanceReceiver *Receiver) {
  SelectedOverload Selected;
  if (Record.CommittedCandidateCount() > 1) {
    const OverloadDispatchResult Resolved =
        ResolveOverloadedCall(Record, State, Types, Receiver);
    if (!Resolved.HasSelection()) {
      Selected.Diagnostic = Resolved.Diagnostic;
      return Selected;
    }
    Selected.Candidate = Record.CandidateAt(Resolved.SelectedCandidate);
  } else {
    Selected.Candidate = Record.PrimaryCandidate();
  }
  return Selected;
}

struct MemberDispatch final {
  const TypeDescriptor *Class = nullptr;
  bool RequiresMutation = false;

  [[nodiscard]] bool ExpectsReceiver() const noexcept {
    return Class != nullptr;
  }
};

[[nodiscard]] MemberDispatch DescribeMember(const BindingRecord &Record) {
  MemberDispatch Described;
  const std::size_t Committed = Record.CommittedCandidateCount();
  for (std::size_t Index = 0; Index < Record.CandidateCount(); ++Index) {
    const OverloadCandidate *Candidate = Record.CandidateAt(Index);
    if (!Candidate || !Candidate->IsCommitted)
      continue;
    const auto &Receiver = Candidate->Signature.ReceiverType;
    if (!Receiver)
      continue;
    if (!Described.Class)
      Described.Class = &*Receiver;
    if (Committed == 1)
      Described.RequiresMutation = !Candidate->Signature.ReceiverIsConst;
  }
  return Described;
}

[[nodiscard]] InvocationResult InvokeValidated(lua_State *State,
                                               BindingRecord &Record,
                                               const TypeGeneration &Types,
                                               FaultInjector &Faults) {
  const std::string_view GlobalName = Record.GlobalName();
  try {
    const MemberDispatch Member = DescribeMember(Record);
    ValidatedReceiver Receiver;
    const InstanceReceiver *Bound = nullptr;
    if (Member.ExpectsReceiver()) {
      Receiver = ValidateInstanceReceiver(
          State, GlobalName, Types, *Member.Class, Member.RequiresMutation, 1);
      if (!Receiver.IsBound())
        return Failure(Receiver.Diagnostic);
      Bound = &Receiver.Bound;
    }

    SelectedOverload Selected = SelectOverload(State, Record, Types, Bound);
    if (!Selected.Candidate)
      return Failure(Selected.Diagnostic.empty()
                         ? "Internal error: no callable candidate is available "
                           "for " +
                               CallableContext(GlobalName) + "."
                         : Selected.Diagnostic);
    ErasedCallableDescriptor &Descriptor = Selected.Candidate->Descriptor;

    const SymbolId SelectedSymbol = Selected.Candidate->Identity;
    const TypeId SelectedReceiverType =
        Member.ExpectsReceiver()
            ? TypeIdentityRegistry::ComputeIdentity(*Member.Class)
                  .value_or(TypeId())
            : TypeId();

    const bool IsMember = Descriptor.Metadata().HasReceiver();
    const std::string Named = MemberContext(GlobalName, IsMember);

    if (Descriptor.Metadata().HasRichParameters()) {
      try {
        RichInvocationResult Rich = InvokeDeclaredParameters(
            State, GlobalName, Descriptor, Types, Faults, Bound);
        if (Rich.Suspension) {
          Rich.Suspension->Symbol = Selected.Candidate->Identity;
          return {.ReturnCount = -1,
                  .Diagnostic = std::string(),
                  .Suspension = std::move(Rich.Suspension),
                  .Symbol = SelectedSymbol,
                  .ReceiverType = SelectedReceiverType};
        }
        if (!Rich.IsSuccess()) {
          InvocationResult Failed = Failure(Rich.Diagnostic);
          Failed.Symbol = SelectedSymbol;
          Failed.ReceiverType = SelectedReceiverType;
          return Failed;
        }
        return {.ReturnCount = Rich.ReturnCount,
                .Diagnostic = std::string(),
                .Suspension = nullptr,
                .Symbol = SelectedSymbol,
                .ReceiverType = SelectedReceiverType};
      } catch (const std::exception &Error) {
        InvocationResult Failed =
            Failure("Runtime error: " + Named + " threw: " + Error.what());
        Failed.Symbol = SelectedSymbol;
        Failed.ReceiverType = SelectedReceiverType;
        return Failed;
      } catch (...) {
        InvocationResult Failed = Failure("Internal error: " + Named +
                                          " threw an unknown C++ exception.");
        Failed.Symbol = SelectedSymbol;
        Failed.ReceiverType = SelectedReceiverType;
        return Failed;
      }
    }

    const int ArgumentBase = Bound != nullptr ? 2 : 1;
    auto Validated = ValidateInvocation(
        State, GlobalName, &Descriptor.Metadata(), Types, Faults, ArgumentBase);
    if (!Validated.Validation.IsSuccess()) {
      const auto *Diagnostic = Validated.Validation.Diagnostic();
      InvocationResult Failed =
          Failure(Diagnostic ? Diagnostic->Message()
                             : "Internal invocation validation error for " +
                                   CallableContext(GlobalName) + ".");
      Failed.Symbol = SelectedSymbol;
      Failed.ReceiverType = SelectedReceiverType;
      return Failed;
    }

    try {
      auto Outcome =
          Bound != nullptr
              ? Descriptor.InvokeWithReceiver(*Bound, Validated.Arguments)
              : Descriptor.Invoke(Validated.Arguments);
      if (Outcome.Kind() == InvocationOutcomeKind::InternalFailure) {
        InvocationResult Failed = Failure("Internal error for " + Named + ": " +
                                          Outcome.FailureMessage());
        Failed.Symbol = SelectedSymbol;
        Failed.ReceiverType = SelectedReceiverType;
        return Failed;
      }

      if (Outcome.Kind() == InvocationOutcomeKind::Suspended)
        return {.ReturnCount = -1,
                .Diagnostic = std::string(),
                .Suspension = StartedCallFrom(
                    Outcome, Descriptor.Metadata().ReturnType(),
                    RetainedArguments(Validated.Arguments,
                                      static_cast<std::size_t>(ArgumentBase)),
                    Selected.Candidate->Identity),
                .Symbol = SelectedSymbol,
                .ReceiverType = SelectedReceiverType};

      auto Written = WriteInvocationReturn(
          State, Descriptor.Metadata().ReturnType(), Outcome, Types, Faults);
      if (!Written.IsSuccess()) {
        const std::string Message = Written.Diagnostic
                                        ? Written.Diagnostic->Message()
                                        : "Return handling failed.";
        InvocationResult Failed =
            Failure("Internal error for " + Named + ": " + Message);
        Failed.Symbol = SelectedSymbol;
        Failed.ReceiverType = SelectedReceiverType;
        return Failed;
      }
      return {.ReturnCount = Written.ReturnCount,
              .Diagnostic = std::string(),
              .Suspension = nullptr,
              .Symbol = SelectedSymbol,
              .ReceiverType = SelectedReceiverType};
    } catch (const std::exception &Error) {
      InvocationResult Failed =
          Failure("Runtime error: " + Named + " threw: " + Error.what());
      Failed.Symbol = SelectedSymbol;
      Failed.ReceiverType = SelectedReceiverType;
      return Failed;
    } catch (...) {
      InvocationResult Failed = Failure("Internal error: " + Named +
                                        " threw an unknown C++ exception.");
      Failed.Symbol = SelectedSymbol;
      Failed.ReceiverType = SelectedReceiverType;
      return Failed;
    }
  } catch (const std::exception &Error) {
    return Failure("Internal error while dispatching " +
                   CallableContext(GlobalName) + ": " + Error.what());
  } catch (...) {
    return Failure("Internal error while dispatching " +
                   CallableContext(GlobalName) + ".");
  }
}

[[nodiscard]] bool RegisterSuspension(
    lua_State *State, InvocationResult &Result, const DispatchEntry &Entry,
    std::shared_ptr<const TypeGeneration> Types, DispatchRetention &Retained,
    int EntryDepth, std::string &Refusal) {
  AsyncCallRegistry *Registry = ObserveAsyncRegistry(State);
  if (!Registry || !Registry->PermitsSuspension(State) ||
      lua_isyieldable(State) == 0) {
    if (Registry)
      Registry->RecordRefusal();
    if (Result.Suspension && Result.Suspension->Work) {
      Result.Suspension->Work->RequestCancellation();
      static_cast<void>(Result.Suspension->Work->Cancel(
          "the call site cannot suspend, so the work was cancelled"));
    }
    Refusal =
        "Unsupported suspension: " + CallableContext(Entry.QualifiedName) +
        " delivers its value later, so it can only be called directly "
        "from the chunk Luna is executing.";
    return false;
  }

  SuspendedCall Started;
  Started.Thread = State;
  Started.EntryStackDepth = EntryDepth;
  Started.Slot = Entry.Slot;
  Started.QualifiedName = Entry.QualifiedName;
  Started.Symbol = Result.Suspension->Symbol;
  Started.ReceiverType = Result.ReceiverType;
  Started.Arguments = std::move(Result.Suspension->Arguments);
  Started.Awaited = Result.Suspension->Awaited;
  Started.Types = std::move(Types);
  Started.Retained = std::move(Retained);
  Started.Faults = Entry.Faults;
  Started.Work = std::move(Result.Suspension->Work);

  if (ProfilingRegistry *Profiling = ObserveProfilingRegistry(State)) {
    Profiling->Report({.Kind = ProfilingEventKind::Suspended,
                       .Symbol = Started.Symbol,
                       .ReceiverType = Started.ReceiverType,
                       .QualifiedName = Started.QualifiedName});
  }

  static_cast<void>(Registry->Suspend(std::move(Started)));
  return true;
}

} // namespace

int NativeTrampolineContinuation(lua_State *State, int Status) {
  if (!State)
    return 0;

  constexpr std::size_t ContinuationDiagnosticCapacity = 512;
  char LocalDiagnostic[ContinuationDiagnosticCapacity]{};
  std::size_t DiagnosticLength = 0;
  int EntryDepth = lua_gettop(State);
  FaultInjector *Faults = nullptr;

  const auto Reject = [&](std::string_view Message) {
    DiagnosticLength = Message.size();
    if (DiagnosticLength > ContinuationDiagnosticCapacity)
      DiagnosticLength = ContinuationDiagnosticCapacity;
    std::memcpy(LocalDiagnostic, Message.data(), DiagnosticLength);
  };

  SymbolId ResumedSymbol;
  TypeId ResumedReceiverType;
  std::string ResumedQualifiedName;
  ProfilingEventKind ResumedKind = ProfilingEventKind::Failed;

  try {
    AsyncCallRegistry *Registry = ObserveAsyncRegistry(State);
    std::optional<SuspendedCall> Resumed =
        Registry ? Registry->Take(State) : std::nullopt;
    if (!Resumed) {
      Reject("Internal error: Luna resumed a call it never suspended.");
    } else {
      EntryDepth = Resumed->EntryStackDepth;
      Faults = Resumed->Faults;
      ResumedSymbol = Resumed->Symbol;
      ResumedReceiverType = Resumed->ReceiverType;
      ResumedQualifiedName = Resumed->QualifiedName;
      ResumedKind = ProfilingEventKind::Resumed;
      const std::string Named = CallableContext(Resumed->QualifiedName);

      if (ProfilingRegistry *Profiling = ObserveProfilingRegistry(State)) {
        Profiling->Report({.Kind = ProfilingEventKind::Resumed,
                           .Symbol = ResumedSymbol,
                           .ReceiverType = ResumedReceiverType,
                           .QualifiedName = ResumedQualifiedName});
      }

      if (Status != 0) {
        ResumedKind = ProfilingEventKind::Failed;
        Reject("Runtime error: " + Named +
               " was resumed with a failure status.");
      } else if (Resumed->Stage == AsyncStage::Ready) {
        if (!Resumed->Awaited || !Resumed->Types || !Faults) {
          ResumedKind = ProfilingEventKind::Failed;
          Reject("Internal error: " + Named +
                 " lost the metadata its resumption needs.");
        } else {
          InvocationOutcome Completed = InvocationOutcome::Void();
          const ReturnMetadata &Awaited = *Resumed->Awaited;
          bool Consistent = true;
          switch (Awaited.Disposition()) {
          case ReturnDisposition::Void:
            Consistent = Resumed->Produced.empty();
            break;
          case ReturnDisposition::Value:
            Consistent = Resumed->Produced.size() == 1;
            if (Consistent)
              Completed =
                  InvocationOutcome::WithValue(std::move(Resumed->Produced[0]));
            break;
          case ReturnDisposition::Pack:
            Completed =
                InvocationOutcome::WithValues(std::move(Resumed->Produced));
            break;
          case ReturnDisposition::Suppress:
          case ReturnDisposition::Instance:
          case ReturnDisposition::Owned:
          case ReturnDisposition::OwnedPack:

            Consistent = false;
            break;
          }

          if (!Consistent) {
            ResumedKind = ProfilingEventKind::Failed;
            Reject("Internal error for " + Named +
                   ": the completed values do not match its declared return "
                   "shape.");
          } else {
            lua_settop(State, EntryDepth);
            const ReturnWriteResult Written = WriteInvocationReturn(
                State, Awaited, Completed, *Resumed->Types, *Faults);
            if (Written.IsSuccess()) {
              if (ProfilingRegistry *Profiling =
                      ObserveProfilingRegistry(State)) {
                Profiling->Report({.Kind = ProfilingEventKind::Completed,
                                   .Symbol = ResumedSymbol,
                                   .ReceiverType = ResumedReceiverType,
                                   .QualifiedName = ResumedQualifiedName});
              }
              return Written.ReturnCount;
            }
            ResumedKind = ProfilingEventKind::Failed;
            Reject("Internal error for " + Named + ": " +
                   (Written.Diagnostic ? Written.Diagnostic->Message()
                                       : std::string("Return handling "
                                                     "failed.")));
          }
        }
      } else if (Resumed->Stage == AsyncStage::Cancelled) {
        ResumedKind = ProfilingEventKind::Cancelled;
        Reject("Cancelled call: " + Named + " was cancelled because " +
               Resumed->Diagnostic + ".");
      } else {
        ResumedKind = ProfilingEventKind::Failed;
        Reject("Runtime error: " + Named + " failed asynchronously because " +
               Resumed->Diagnostic + ".");
      }
    }
  } catch (...) {
    ResumedKind = ProfilingEventKind::Failed;
    Reject("Internal error: Luna could not resume a suspended call.");
  }

  if (DiagnosticLength == 0) {
    static constexpr char Fallback[] =
        "Internal error: Luna could not resume a suspended call.";
    DiagnosticLength = sizeof(Fallback) - 1;
    std::memcpy(LocalDiagnostic, Fallback, DiagnosticLength);
  }

  if (ProfilingRegistry *Profiling = ObserveProfilingRegistry(State)) {
    Profiling->Report({.Kind = ResumedKind,
                       .Symbol = ResumedSymbol,
                       .ReceiverType = ResumedReceiverType,
                       .QualifiedName = ResumedQualifiedName});
  }

  lua_settop(State, EntryDepth);
  const int RestoredDepth = lua_gettop(State);
  lua_pushlstring(State, LocalDiagnostic, DiagnosticLength);
  if (Faults)
    Faults->RecordCallbackStackRestoration(EntryDepth, RestoredDepth,
                                           lua_gettop(State));
  lua_error(State);
  return 0;
}

int NativeTrampoline(lua_State *State) {
  if (!State)
    return 0;

  constexpr std::size_t LocalDiagnosticCapacity = 512;
  const int EntryDepth = lua_gettop(State);
  char LocalDiagnostic[LocalDiagnosticCapacity]{};
  char *PreparedDiagnostic = LocalDiagnostic;
  std::size_t DiagnosticLength = 0;
  bool HeapDiagnostic = false;
  FaultInjector *Faults = nullptr;
  SymbolId FailedSymbol;
  TypeId FailedReceiverType;
  std::string FailedQualifiedName;

  {
    try {
      InvocationResult Result;

      const DispatchSlotId Slot = ClosureDispatchSlot(State);
      const DispatchTable *Dispatch = ObserveDispatchTable(State);
      DispatchRetention Retained =
          Dispatch ? Dispatch->Retain(DispatchRetainer::Invocation)
                   : DispatchRetention{};
      const DispatchEntry *Entry = Retained.Find(Slot);
      BindingRecord *Record = Entry ? Entry->Target : nullptr;
      bool Suspend = false;
      if (!Entry) {
        Result = Failure("Unavailable binding: this State no longer resolves "
                         "the callable this closure was installed for.");
      } else if (!Record || !Record->IsCommitted()) {
        Result = Failure(
            "Unavailable binding: " + CallableContext(Entry->QualifiedName) +
            " is no longer available in this State.");
      } else if (!Entry->Faults) {
        Result = Failure(
            "Internal error: binding fault context is unavailable for " +
            CallableContext(Entry->QualifiedName) + ".");
      } else {
        Faults = Entry->Faults;
        Faults->ClearCallbackStackRestoration();

        std::shared_ptr<const TypeGeneration> Types =
            Entry->Types ? Entry->Types->Capture() : nullptr;
        if (!Types) {
          Result = Failure("Internal error: the canonical type registry is "
                           "unavailable for " +
                           CallableContext(Entry->QualifiedName) + ".");
        } else {
          Result = InvokeValidated(State, *Record, *Types, *Faults);
          if (Result.IsSuspended()) {
            std::string Refusal;
            const SymbolId SuspendedSymbol = Result.Symbol;
            const TypeId SuspendedReceiverType = Result.ReceiverType;
            Suspend =
                RegisterSuspension(State, Result, *Entry, std::move(Types),
                                   Retained, EntryDepth, Refusal);
            if (!Suspend) {
              Result = Failure(std::move(Refusal));
              Result.Symbol = SuspendedSymbol;
              Result.ReceiverType = SuspendedReceiverType;
            }
          }
        }
      }

      if (Suspend)
        return lua_yield(State, 0);

      if (Result.IsSuccess()) {
        if (ProfilingRegistry *Profiling = ObserveProfilingRegistry(State)) {
          Profiling->Report(
              {.Kind = ProfilingEventKind::Completed,
               .Symbol = Result.Symbol,
               .ReceiverType = Result.ReceiverType,
               .QualifiedName = Entry ? Entry->QualifiedName : std::string()});
        }
        return Result.ReturnCount;
      }

      FailedSymbol = Result.Symbol;
      FailedReceiverType = Result.ReceiverType;
      FailedQualifiedName = Entry ? Entry->QualifiedName : std::string();

      DiagnosticLength = Result.Diagnostic.size();
      if (DiagnosticLength > LocalDiagnosticCapacity) {
        PreparedDiagnostic = static_cast<char *>(std::malloc(DiagnosticLength));
        HeapDiagnostic = PreparedDiagnostic != nullptr;
      }
      if (!PreparedDiagnostic) {
        static constexpr char Fallback[] =
            "Internal Luna invocation error: diagnostic allocation failed.";
        PreparedDiagnostic = LocalDiagnostic;
        DiagnosticLength = sizeof(Fallback) - 1;
        std::memcpy(PreparedDiagnostic, Fallback, DiagnosticLength);
      } else if (DiagnosticLength != 0) {
        std::memcpy(PreparedDiagnostic, Result.Diagnostic.data(),
                    DiagnosticLength);
      }
    } catch (...) {
      static constexpr char Fallback[] = "Internal Luna invocation error.";
      PreparedDiagnostic = LocalDiagnostic;
      DiagnosticLength = sizeof(Fallback) - 1;
      std::memcpy(PreparedDiagnostic, Fallback, DiagnosticLength);
    }
  }

  if (DiagnosticLength == 0) {
    static constexpr char Fallback[] = "Internal Luna invocation error.";
    PreparedDiagnostic = LocalDiagnostic;
    DiagnosticLength = sizeof(Fallback) - 1;
    std::memcpy(PreparedDiagnostic, Fallback, DiagnosticLength);
  }

  if (ProfilingRegistry *Profiling = ObserveProfilingRegistry(State)) {
    Profiling->Report({.Kind = ProfilingEventKind::Failed,
                       .Symbol = FailedSymbol,
                       .ReceiverType = FailedReceiverType,
                       .QualifiedName = FailedQualifiedName});
  }

  lua_settop(State, EntryDepth);
  const int RestoredDepth = lua_gettop(State);
  lua_pushlstring(State, PreparedDiagnostic, DiagnosticLength);
  if (Faults)
    Faults->RecordCallbackStackRestoration(EntryDepth, RestoredDepth,
                                           lua_gettop(State));
  if (HeapDiagnostic)
    std::free(PreparedDiagnostic);
  lua_error(State);
  return 0;
}

} // namespace Luna::Detail
