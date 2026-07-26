#pragma once

// Deterministic validation of one planned declaration inside one outermost
// transaction. Validation is the second phase of a transaction: it reads the
// entry capture and the committed-plus-pending symbol view, never mutable
// State, and returns the first failure in one documented precedence.
//
// Two precedence profiles exist. The foundation's root `Register` /
// `RegisterFunction` keeps the order the foundation established:
//
//   1. invalid identifier
//   2. wrong thread
//   3. non-ready or frozen lifecycle
//   4. null or invalid target
//   5. duplicate candidate or incompatible category
//   6. missing or conflicting type or converter
//   7. malformed callable metadata or incomplete descriptor
//
// Every other operation rejects a wrong thread and a non-ready or frozen
// lifecycle before it looks at a name:
//
//   1. wrong thread
//   2. non-ready or frozen lifecycle
//   3. invalid identifier or stable key
//   4. stale or wrong scope
//   5. null or invalid target
//   6. duplicate candidate or incompatible category
//   7. missing or conflicting type or converter
//   8. malformed metadata or incomplete descriptor
//
// Preparation, protected installation, and internal consistency failures follow
// validation and are reported by their own phases, so they always rank after
// everything here. Within one category, canonical qualified-name order decides.

// clang-format off
#include <luna/core/diagnostics/error_diagnostic.hpp>

#include "state/registration/plan.hpp"
#include "state/transaction/capture.hpp"
#include "state/transaction/generation_set.hpp"
#include "state/type/type_generation.hpp"

#include <optional>
#include <string>
#include <string_view>
// clang-format on

namespace Luna::Detail {

enum class RegistrationPrecedence { FoundationRootFunction, GeneralOperation };

struct RegistrationValidationRequest final {
  RegistrationPrecedence Precedence = RegistrationPrecedence::GeneralOperation;

  // The canonical name the consumer asked for. For a root-scope function this
  // is the single global identifier; otherwise it is the qualified name.
  std::string_view Name;

  // The planned declaration. A null or incomplete entry is a descriptor
  // completeness failure, which ranks last.
  const DescriptorPlanEntry *Entry = nullptr;

  PlanEntryKind Category = PlanEntryKind::Function;

  // The native target of the declaration exists and is invocable.
  bool HasTarget = false;

  // The canonical virtual-machine path is already occupied by a committed Luna
  // symbol that the committed symbol table does not describe yet. The
  // foundation's root-scope binding store answers this question today.
  bool VmPathIsOccupied = false;

  // The parent scope is a Luna-owned scope of this State, of the right
  // category, and still belongs to the captured lifecycle generation.
  bool ScopeIsOwned = true;
  bool ScopeIsCurrent = true;
  std::string_view ParentQualifiedName;

  // Diagnostic wording of the attempted symbol kind. Several categories share
  // one plan entry kind - a constant and every later installed value, a
  // namespace and an enumeration scope - so a request may name the kind a
  // consumer actually asked for. Empty means the plan entry kind names itself.
  std::string_view SubjectKindText;

  // The exact canonical path already holds a value Luna does not own as the
  // requested symbol: a script-created table, a foreign value, or a stale Luna
  // table. It is a collision at the duplicate precedence step, never an
  // adoption or a replacement.
  bool VmPathHoldsUnownedValue = false;
  std::string_view VmPathValueKindText;

  // The declaration is one more candidate of the overload set its qualified
  // name already owns, and its canonical signature differs from every candidate
  // of that set. Sharing a name is then a grouping, not a duplicate. A
  // candidate no call could tell apart from an existing one never sets this, so
  // indistinguishable declarations keep reporting the duplicate diagnostic in
  // exactly the foundation's precedence.
  bool JoinsOverloadSet = false;
};

// The first deterministic failure of one planned declaration, or no value when
// the declaration passes every validation phase.
[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateRegistration(const RegistrationValidationRequest &Request,
                     const TransactionCapture &Capture,
                     const SymbolView &Symbols);

// The entry-capture half of the general precedence: a wrong thread and a
// non-ready or frozen lifecycle rank before every name, scope, and duplicate
// decision. A plan whose declarations were staged earlier reports these two
// failures first, so a stale builder on a frozen State still reports the
// lifecycle failure ahead of anything its plan recorded.
[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateTransactionEntry(const TransactionCapture &Capture,
                         std::string_view Subject);

// Diagnostic subject of one request: the attempted symbol kind and its name.
[[nodiscard]] std::string
RequestSubject(const RegistrationValidationRequest &Request);

// True when the canonical type is available for conversion in the given
// immutable type generation. Availability is a registry question now: the
// migrated foundation generation answers it for void, bool, signed 32-bit int,
// double, and string, and a later generation answers it for whatever it
// declares.
[[nodiscard]] bool IsAvailableCanonicalType(const TypeGeneration &Types,
                                            const TypeDescriptor &Type,
                                            bool AllowVoid) noexcept;

// The same question against the migrated foundation generation.
[[nodiscard]] bool IsAvailableCanonicalType(const TypeDescriptor &Type,
                                            bool AllowVoid) noexcept;

} // namespace Luna::Detail
