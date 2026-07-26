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

// The subject one dispatch names inside a sentence. Until the record's own
// candidates are known it is the foundation's wording; an instance member names
// its class and member as soon as its metadata says so.
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

// Selects the candidate of one overload set this call resolves to. A set with a
// single committed candidate resolves to exactly that declaration without
// probing anything, so one-candidate invocation keeps the foundation's arity
// and type diagnostics unchanged. A set with several candidates resolves
// through side-effect-free probing and Pareto dominance, and a refused
// resolution reports one canonical no-match or ambiguity diagnostic.
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

// The instance-member half of one overload set: the class its candidates
// operate on, and - when the set published exactly one candidate - whether that
// candidate mutates its object.
//
// A set of several candidates is validated without any mutation requirement and
// then ranks const access per candidate, so a const value of the class still
// reaches the const sibling of a non-const member. A set of exactly one
// candidate is validated with that candidate's own requirement, so its refusal
// comes straight from the access gate in the gate's own fixed order.
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
    // An instance member validates the object it operates on first. Presence,
    // origin State, layout, metatable identity, lifetime, dynamic type, and
    // const access are all decided before one ordinary argument is inspected,
    // which is what makes a dot call without a receiver fail as a receiver
    // refusal rather than as a shifted argument diagnostic.
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

    // Resolution happens before anything commits: nothing is converted, no
    // native target runs, and no state is mutated while candidates are ranked.
    SelectedOverload Selected = SelectOverload(State, Record, Types, Bound);
    if (!Selected.Candidate)
      return Failure(Selected.Diagnostic.empty()
                         ? "Internal error: no callable candidate is available "
                           "for " +
                               CallableContext(GlobalName) + "."
                         : Selected.Diagnostic);
    ErasedCallableDescriptor &Descriptor = Selected.Candidate->Descriptor;

    // A callable that declares optional, defaulted, or variadic parameters
    // binds, invokes, and publishes through its own declared shape. Exception
    // translation stays exactly the foundation's.
    // The subject every later diagnostic of this call names. One instance
    // member names its class and member here exactly as its receiver refusal,
    // its getter, and its setter already do.
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

      // The closure carries one thing: the permanent dispatch slot of its
      // canonical path. One accounted retention at invocation entry holds the
      // immutable dispatch generation that resolves it, so the target and every
      // piece of metadata this call needs stay valid for the whole call even if
      // a later publication replaces the generation meanwhile - and that
      // superseded generation cannot be reclaimed while this call still holds
      // it. A call that began under one generation therefore finishes under it;
      // the next call through the same closure resolves whatever is current
      // then.
      const DispatchSlotId Slot = ClosureDispatchSlot(State);
      const DispatchTable *Dispatch = ObserveDispatchTable(State);
      const DispatchRetention Retained =
          Dispatch ? Dispatch->Retain(DispatchRetainer::Invocation)
                   : DispatchRetention{};
      const DispatchEntry *Entry = Retained.Find(Slot);
      BindingRecord *Record = Entry ? Entry->Target : nullptr;
      if (!Entry) {
        // The slot names nothing this State ever issued, so there is no symbol
        // to name in the refusal.
        Result = Failure("Unavailable binding: this State no longer resolves "
                         "the callable this closure was installed for.");
      } else if (!Record || !Record->IsCommitted()) {
        // The slot is still the permanent identity of its canonical path, and
        // the entry still names that path; only the target is gone. A stale
        // closure of a removed symbol therefore refuses deterministically under
        // the name it was installed for instead of following released storage.
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

        // One capture at invocation entry: every conversion and diagnostic of
        // this call reads the same immutable type generation.
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

  // Only trivially destructible locals remain in this minimal error tail.
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