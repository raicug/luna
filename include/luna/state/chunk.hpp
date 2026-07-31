#pragma once

// clang-format off
#include <luna/binding/conversion.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna {

namespace Detail {
class ChunkHost;
}

class ChunkResult final {
public:
  ChunkResult() = default;

  [[nodiscard]] static ChunkResult Delivered(ValuePack Produced) {
    ChunkResult Result;
    Result.ProducedValue = std::move(Produced);
    return Result;
  }

  [[nodiscard]] static ChunkResult Failure(ErrorDiagnostic Diagnostic) {
    ChunkResult Result;
    Result.DiagnosticValue = std::move(Diagnostic);
    return Result;
  }

  [[nodiscard]] static ChunkResult Failure(ErrorCategory Category,
                                           std::string Message) {
    return Failure(ErrorDiagnostic::Create(Category, std::move(Message)));
  }

  [[nodiscard]] bool IsSuccess() const noexcept {
    return !DiagnosticValue.has_value();
  }

  [[nodiscard]] bool IsInterrupted() const noexcept {
    return DiagnosticValue &&
           DiagnosticValue->Category() == ErrorCategory::Interrupted;
  }

  [[nodiscard]] const ErrorDiagnostic *Diagnostic() const noexcept {
    return DiagnosticValue ? &*DiagnosticValue : nullptr;
  }

  [[nodiscard]] const ValuePack &Values() const noexcept {
    return ProducedValue;
  }

  [[nodiscard]] std::size_t Size() const noexcept {
    return ProducedValue.Size();
  }

  [[nodiscard]] OwnedValue At(std::size_t Index) const {
    return ProducedValue.At(Index);
  }

private:
  ValuePack ProducedValue;
  std::optional<ErrorDiagnostic> DiagnosticValue;
};

class Chunk final {
public:
  Chunk() = default;

  [[nodiscard]] static Chunk Refused(ErrorDiagnostic Diagnostic) {
    Chunk Refused;
    Refused.DiagnosticValue = std::move(Diagnostic);
    return Refused;
  }

  [[nodiscard]] static Chunk Refused(ErrorCategory Category,
                                     std::string Message) {
    return Refused(ErrorDiagnostic::Create(Category, std::move(Message)));
  }

  [[nodiscard]] bool IsLoaded() const noexcept {
    return HostValue != nullptr && !DiagnosticValue.has_value() &&
           !BytecodeValue.empty();
  }

  [[nodiscard]] const ErrorDiagnostic *Diagnostic() const noexcept {
    return DiagnosticValue ? &*DiagnosticValue : nullptr;
  }

  [[nodiscard]] std::string_view Name() const noexcept { return NameValue; }

  [[nodiscard]] std::string_view Bytecode() const noexcept {
    return BytecodeValue;
  }

  [[nodiscard]] ChunkResult Invoke() const;
  [[nodiscard]] ChunkResult Invoke(const ValuePack &Arguments) const;

private:
  friend class State;

  Chunk(std::shared_ptr<Detail::ChunkHost> Host, std::string Bytecode,
        std::string Name)
      : HostValue(std::move(Host)), BytecodeValue(std::move(Bytecode)),
        NameValue(std::move(Name)) {}

  std::shared_ptr<Detail::ChunkHost> HostValue;
  std::string BytecodeValue;
  std::string NameValue;
  std::optional<ErrorDiagnostic> DiagnosticValue;
};

} // namespace Luna
