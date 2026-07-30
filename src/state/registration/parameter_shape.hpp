#pragma once

// clang-format off
#include <luna/binding/callable_metadata.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/reflection/reflection_record.hpp>

#include "state/identity/symbol_descriptor.hpp"
#include "state/reflection/storage.hpp"
#include "state/type/type_record.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

[[nodiscard]] CallableSignatureDescriptor
CanonicalDeclaredSignature(const CallableMetadata &Metadata);

[[nodiscard]] CallableSignatureDescriptor
WithCanonicalReceiver(const CallableMetadata &Metadata,
                      CallableSignatureDescriptor Signature);

[[nodiscard]] std::optional<ErrorDiagnostic>
CheckDeclaredParameterShape(const CallableMetadata &Metadata,
                            const CallableSignatureDescriptor &Signature,
                            std::string_view Subject);

[[nodiscard]] std::vector<ReflectionParameterFields>
MakeReflectedParameters(const CallableMetadata &Metadata);

[[nodiscard]] std::vector<TypeRecord>
MakeParameterTypeConversions(const CallableMetadata &Metadata);

[[nodiscard]] std::vector<ReflectionReturnFields>
MakeReflectedReturns(const CallableMetadata &Metadata);

[[nodiscard]] ReturnShape
ReflectedReturnShape(const CallableMetadata &Metadata);

[[nodiscard]] std::string
CanonicalSignatureText(const CallableSignatureDescriptor &Signature);

} // namespace Luna::Detail
