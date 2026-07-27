// clang-format off
#include "state/invocation/conversion/argument_reader.hpp"

#include <luna/binding/value.hpp>

#include "state/invocation/overload/instrumentation.hpp"
#include "state/type/conversion_outcome.hpp"
#include "state/type/type_generation.hpp"
#include "state/type/type_record.hpp"

#include <memory>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] ArgumentReadResult InternalFailure() noexcept {
  return {.Status = ArgumentReadStatus::InternalFailure};
}

} // namespace

ArgumentReadResult ReadArgument(const TypeGeneration &Types, lua_State *State,
                                int StackIndex, ValueKind ExpectedKind,
                                bool InjectInspectionFailure) noexcept {
  if (!State || InjectInspectionFailure)
    return InternalFailure();

  try {
    const TypeRecord *Record = Types.Find(ExpectedKind);
    if (!Record || !Record->IsReadable || !Record->Read)
      return InternalFailure();

    RecordCommittingArgumentRead();
    return Record->Read(State, StackIndex);
  } catch (...) {
    return InternalFailure();
  }
}

ArgumentReadResult ReadArgument(lua_State *State, int StackIndex,
                                ValueKind ExpectedKind,
                                bool InjectInspectionFailure) noexcept {
  const std::shared_ptr<const TypeGeneration> Types =
      TypeGeneration::Foundation();
  if (!Types)
    return InternalFailure();
  return ReadArgument(*Types, State, StackIndex, ExpectedKind,
                      InjectInspectionFailure);
}

const char *ValueKindName(ValueKind Kind) noexcept {
  const std::shared_ptr<const TypeGeneration> Types =
      TypeGeneration::Foundation();
  if (!Types)
    return "unknown";
  const TypeRecord *Record = Types->Find(Kind);
  return Record ? Record->PublicName.c_str() : "unknown";
}

} // namespace Luna::Detail
