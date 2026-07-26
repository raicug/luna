#pragma once

// The registration half of one declared parameter shape.
//
// A callable that declares optional, defaulted, or variadic parameters is not a
// second registration path: it produces the same canonical signature descriptor
// every other callable produces, with the shape carried by the two fields the
// canonical model already reserves for it - the required parameter count and
// the variadic flag - and with each parameter's canonical value type in
// declared order. A variadic tail owns no fixed position and therefore
// contributes no parameter type.
//
// Default metadata is validated here, at registration, before anything is
// installed: a default that names the wrong type, a required parameter after an
// omittable one, and a variadic parameter that is not final are all refused
// with one deterministic malformed-metadata diagnostic.

// clang-format off
#include <luna/binding/callable_metadata.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/reflection/reflection_record.hpp>

#include "state/identity/symbol_descriptor.hpp"
#include "state/reflection/storage.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

// The canonical signature of one declared shape.
[[nodiscard]] CallableSignatureDescriptor
CanonicalDeclaredSignature(const CallableMetadata &Metadata);

// The same signature, carrying the receiver one instance member declares. The
// object a member operates on is part of its call shape rather than one of its
// parameters, so every canonical signature builder adds it exactly here.
[[nodiscard]] CallableSignatureDescriptor
WithCanonicalReceiver(const CallableMetadata &Metadata,
                      CallableSignatureDescriptor Signature);

// One deterministic refusal of the declared shape, or nothing when the shape
// and the canonical signature agree.
[[nodiscard]] std::optional<ErrorDiagnostic>
CheckDeclaredParameterShape(const CallableMetadata &Metadata,
                            const CallableSignatureDescriptor &Signature,
                            std::string_view Subject);

// The reflected parameters of one declared shape, in declared order, carrying
// each parameter's disposition and immutable default text.
[[nodiscard]] std::vector<ReflectionParameterFields>
MakeReflectedParameters(const CallableMetadata &Metadata);

// The reflected returned values of one callable, in declaration order. A void
// callable returns none, and a scalar callable returns exactly one.
[[nodiscard]] std::vector<ReflectionReturnFields>
MakeReflectedReturns(const CallableMetadata &Metadata);

// The reflected return shape of one callable: zero values for void, one for a
// scalar, and several for an ordered pack.
[[nodiscard]] ReturnShape
ReflectedReturnShape(const CallableMetadata &Metadata);

// Canonical text of one candidate signature, in declaration order. The shape
// travels with the text: an omittable parameter is marked, and a variadic tail
// is spelled as the final ellipsis, so two candidates that differ only in shape
// never reflect one identical signature.
[[nodiscard]] std::string
CanonicalSignatureText(const CallableSignatureDescriptor &Signature);

} // namespace Luna::Detail
