// clang-format off
#include <luna/state/chunk.hpp>

#include <string>
#include <type_traits>
#include <utility>
// clang-format on

namespace {

static_assert(std::is_default_constructible_v<Luna::Chunk>,
              "A chunk must be default constructible.");
static_assert(std::is_copy_constructible_v<Luna::Chunk>,
              "A chunk must be storable by value.");
static_assert(std::is_move_constructible_v<Luna::Chunk>,
              "A chunk must be movable.");
static_assert(std::is_default_constructible_v<Luna::ChunkResult>,
              "A chunk result must be default constructible.");

} // namespace

void VerifyChunkHeaderCompilesStandalone() {
  const Luna::Chunk Empty;
  const Luna::Chunk Refused = Luna::Chunk::Refused(
      Luna::ErrorCategory::Compilation, "compiler rejected the source.");

  Luna::ValuePack Produced;
  Produced.Append(Luna::OwnedValue::Number(3.0));

  const Luna::ChunkResult Delivered =
      Luna::ChunkResult::Delivered(std::move(Produced));
  const Luna::ChunkResult Failed = Luna::ChunkResult::Failure(
      Luna::ErrorCategory::Runtime, "the chunk failed.");

  static_cast<void>(Empty.IsLoaded());
  static_cast<void>(Empty.Name().empty());
  static_cast<void>(Empty.Bytecode().empty());
  static_cast<void>(Refused.Diagnostic() != nullptr);
  static_cast<void>(Delivered.IsSuccess());
  static_cast<void>(Delivered.Size());
  static_cast<void>(Delivered.At(0).Kind());
  static_cast<void>(Delivered.Values().Size());
  static_cast<void>(Failed.IsInterrupted());
  static_cast<void>(Failed.Diagnostic() != nullptr);
}
