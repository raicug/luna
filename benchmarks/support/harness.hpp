#pragma once

// clang-format off
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
// clang-format on

namespace LunaBenchmark {

enum class CacheMode { UnfrozenUncached, FrozenCached };

[[nodiscard]] std::string_view CacheModeText(CacheMode Mode) noexcept;

struct MeasuredConfiguration final {
  std::string BuildType;
  std::string Compiler;
  std::string Architecture;
  std::string LuauVersion;

  [[nodiscard]] bool IsComplete() const noexcept;
};

[[nodiscard]] MeasuredConfiguration BuiltConfiguration();

class Suite final {
public:
  Suite(std::string_view Scenario, int ArgumentCount,
        const char *const *ArgumentValues);

  Suite(const Suite &) = delete;
  Suite &operator=(const Suite &) = delete;
  Suite(Suite &&) = delete;
  Suite &operator=(Suite &&) = delete;

  ~Suite() = default;

  [[nodiscard]] bool IsRunnable() const noexcept;

  [[nodiscard]] std::size_t Warmup() const noexcept { return WarmupCount; }
  [[nodiscard]] std::size_t Samples() const noexcept { return SampleCount; }

  void Measure(std::string_view CaseName, std::string_view Corpus,
               CacheMode Mode, std::size_t OperationCount,
               const std::function<bool()> &Body);

  void Block(std::string_view CaseName, std::string_view Corpus, CacheMode Mode,
             std::string_view Reason);

  [[nodiscard]] int Finish() const;

  static void Consume(std::uint64_t Observed) noexcept;

private:
  void Reject(std::string Reason);

  std::string ScenarioValue;
  MeasuredConfiguration ConfigurationValue;
  std::string CorrectnessEvidence;
  std::size_t WarmupCount = 3;
  std::size_t SampleCount = 20;
  std::size_t CaseCount = 0;
  std::size_t FailureCount = 0;
  std::size_t BlockedCount = 0;
  bool OptionsValid = true;
};

} // namespace LunaBenchmark
