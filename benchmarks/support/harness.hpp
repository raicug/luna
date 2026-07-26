#pragma once

// The repeatable measurement harness every Luna benchmark target uses.
//
// A benchmark result is only evidence when the configuration that produced it
// is recorded next to it, so this harness refuses to print any timing until it
// knows the build type, the compiler, the architecture, the Luau version, the
// corpus, the warmup count, the sample count, and the cache/freeze mode of the
// measurement. Every result line carries all of them.
//
// A benchmark result is also only admissible as an optimization decision while
// the correctness suites are unchanged, which no benchmark process can observe
// for itself. The harness therefore marks every result `claimable=false` unless
// the runner supplies that evidence explicitly with
// `--correctness-evidence=<text>`, and prints the guard requirement with the
// summary of every run.
//
// Nothing here links or includes a virtual machine: benchmarks are ordinary
// Luna consumers that reach Luna through `Luna::Luna` and its public headers.

// clang-format off
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
// clang-format on

namespace LunaBenchmark {

// Which lookup regime one measurement was taken in. Every scenario measures
// both, because a frozen State answers through published caches and an unfrozen
// one answers through the live committed model.
enum class CacheMode { UnfrozenUncached, FrozenCached };

[[nodiscard]] std::string_view CacheModeText(CacheMode Mode) noexcept;

// The measured configuration of one benchmark process. Every field is supplied
// by the build, never guessed at runtime.
struct MeasuredConfiguration final {
  std::string BuildType;
  std::string Compiler;
  std::string Architecture;
  std::string LuauVersion;

  [[nodiscard]] bool IsComplete() const noexcept;
};

// The configuration this benchmark binary was compiled with.
[[nodiscard]] MeasuredConfiguration BuiltConfiguration();

// One benchmark scenario process: it parses the repeatability options, measures
// cases, prints one fully qualified result line per case, and reports whether
// the run itself stayed functionally correct.
class Suite final {
public:
  Suite(std::string_view Scenario, int ArgumentCount,
        const char *const *ArgumentValues);

  Suite(const Suite &) = delete;
  Suite &operator=(const Suite &) = delete;
  Suite(Suite &&) = delete;
  Suite &operator=(Suite &&) = delete;

  ~Suite() = default;

  // The harness knows its complete configuration and its options parsed, so
  // measuring is meaningful.
  [[nodiscard]] bool IsRunnable() const noexcept;

  [[nodiscard]] std::size_t Warmup() const noexcept { return WarmupCount; }
  [[nodiscard]] std::size_t Samples() const noexcept { return SampleCount; }

  // Measures one case. `Body` performs exactly `OperationCount` operations of
  // the described corpus and returns whether the work it performed was
  // functionally correct; a functionally incorrect body suppresses the timing
  // and fails the run, because a wrong result is never a fast result.
  void Measure(std::string_view CaseName, std::string_view Corpus,
               CacheMode Mode, std::size_t OperationCount,
               const std::function<bool()> &Body);

  // Records one case whose corpus this mode cannot prepare at all, with the
  // reason. A blocked case is never timed and never claimable: nothing was
  // measured, so nothing may be claimed about it.
  void Block(std::string_view CaseName, std::string_view Corpus, CacheMode Mode,
             std::string_view Reason);

  // Prints the run summary and the optimization guard, and reports the process
  // exit status.
  [[nodiscard]] int Finish() const;

  // Keeps a measured value observable so the work under measurement is not
  // discarded.
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
