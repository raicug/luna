// clang-format off
#include "state/invocation/trampoline.hpp"

#include "state/dispatch/closure_slot.hpp"
#include "state/invocation/conversion/return_writer.hpp"
#include "state/invocation/members/receiver.hpp"
#include "state/invocation/overload/dispatch.hpp"
#include "state/invocation/parameters/parameter_binding.hpp"
#include "state/invocation/validation/validator.hpp"
#include "state/registration/record.hpp"
#include "state/testing/fault_injector.hpp"
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

  [[nodiscard]] bool IsSuccess() const noexcept { return ReturnCount >= 0; }
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

    const bool IsMember = Descriptor.Metadata().HasReceiver();
    const std::string Named = MemberContext(GlobalName, IsMember);

    if (Descriptor.Metadata().HasRichParameters()) {
      try {
        const RichInvocationResult Rich = InvokeDeclaredParameters(
            State, GlobalName, Descriptor, Types, Faults, Bound);
        if (!Rich.IsSuccess())
          return Failure(Rich.Diagnostic);
        return {.ReturnCount = Rich.ReturnCount};
      } catch (const std::exception &Error) {
        return Failure("Runtime error: " + Named + " threw: " + Error.what());
      } catch (...) {
        return Failure("Internal error: " + Named +
                       " threw an unknown C++ exception.");
      }
    }

    const int ArgumentBase = Bound != nullptr ? 2 : 1;
    auto Validated = ValidateInvocation(
        State, GlobalName, &Descriptor.Metadata(), Types, Faults, ArgumentBase);
    if (!Validated.Validation.IsSuccess()) {
      const auto *Diagnostic = Validated.Validation.Diagnostic();
      return Failure(Diagnostic ? Diagnostic->Message()
                                : "Internal invocation validation error for " +
                                      CallableContext(GlobalName) + ".");
    }

    try {
      auto Outcome =
          Bound != nullptr
              ? Descriptor.InvokeWithReceiver(*Bound, Validated.Arguments)
              : Descriptor.Invoke(Validated.Arguments);
      if (Outcome.Kind() == InvocationOutcomeKind::InternalFailure)
        return Failure("Internal error for " + Named + ": " +
                       Outcome.FailureMessage());

      auto Written = WriteInvocationReturn(
          State, Descriptor.Metadata().ReturnType(), Outcome, Types, Faults);
      if (!Written.IsSuccess()) {
        const std::string Message = Written.Diagnostic
                                        ? Written.Diagnostic->Message()
                                        : "Return handling failed.";
        return Failure("Internal error for " + Named + ": " + Message);
      }
      return {.ReturnCount = Written.ReturnCount};
    } catch (const std::exception &Error) {
      return Failure("Runtime error: " + Named + " threw: " + Error.what());
    } catch (...) {
      return Failure("Internal error: " + Named +
                     " threw an unknown C++ exception.");
    }
  } catch (const std::exception &Error) {
    return Failure("Internal error while dispatching " +
                   CallableContext(GlobalName) + ": " + Error.what());
  } catch (...) {
    return Failure("Internal error while dispatching " +
                   CallableContext(GlobalName) + ".");
  }
}

} // namespace

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

  {
    try {
      InvocationResult Result;

      const DispatchSlotId Slot = ClosureDispatchSlot(State);
      const DispatchTable *Dispatch = ObserveDispatchTable(State);
      const DispatchRetention Retained =
          Dispatch ? Dispatch->Retain(DispatchRetainer::Invocation)
                   : DispatchRetention{};
      const DispatchEntry *Entry = Retained.Find(Slot);
      BindingRecord *Record = Entry ? Entry->Target : nullptr;
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

        const std::shared_ptr<const TypeGeneration> Types =
            Entry->Types ? Entry->Types->Capture() : nullptr;
        if (!Types)
          Result = Failure("Internal error: the canonical type registry is "
                           "unavailable for " +
                           CallableContext(Entry->QualifiedName) + ".");
        else
          Result = InvokeValidated(State, *Record, *Types, *Faults);
      }

      if (Result.IsSuccess())
        return Result.ReturnCount;

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
