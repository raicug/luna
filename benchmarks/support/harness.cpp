// clang-format off
#include "support/harness.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

#ifndef LUNA_BENCHMARK_BUILD_TYPE
#define LUNA_BENCHMARK_BUILD_TYPE ""
#endif
#ifndef LUNA_BENCHMARK_COMPILER
#define LUNA_BENCHMARK_COMPILER ""
#endif
#ifndef LUNA_BENCHMARK_ARCHITECTURE
#define LUNA_BENCHMARK_ARCHITECTURE ""
#endif
#ifndef LUNA_BENCHMARK_LUAU_VERSION
#define LUNA_BENCHMARK_LUAU_VERSION ""
#endif

namespace LunaBenchmark {
namespace {

volatile std::uint64_t ObservedSink = 0;

[[nodiscard]] bool ParseCount(std::string_view Text, std::size_t &Parsed) {
  if (Text.empty())
    return false;
  std::size_t Value = 0;
  const char *First = Text.data();
  const char *Last = Text.data() + Text.size();
  const std::from_chars_result Outcome = std::from_chars(First, Last, Value);
  if (Outcome.ec != std::errc() || Outcome.ptr != Last)
    return false;
  Parsed = Value;
  return true;
}

[[nodiscard]] std::string Quoted(std::string_view Text) {
  std::string Result;
  Result.reserve(Text.size() + 2);
  Result.push_back('"');
  Result.append(Text);
  Result.push_back('"');
  return Result;
}

} // namespace

std::string_view CacheModeText(CacheMode Mode) noexcept {
  switch (Mode) {
  case CacheMode::UnfrozenUncached:
    return "unfrozen-uncached";
  case CacheMode::FrozenCached:
    return "frozen-cached";
  }
  return "unknown";
}

bool MeasuredConfiguration::IsComplete() const noexcept {
  return !BuildType.empty() && !Compiler.empty() && !Architecture.empty() &&
         !LuauVersion.empty();
}

MeasuredConfiguration BuiltConfiguration() {
  MeasuredConfiguration Measured;
  Measured.BuildType = LUNA_BENCHMARK_BUILD_TYPE;
  Measured.Compiler = LUNA_BENCHMARK_COMPILER;
  Measured.Architecture = LUNA_BENCHMARK_ARCHITECTURE;
  Measured.LuauVersion = LUNA_BENCHMARK_LUAU_VERSION;
  return Measured;
}

Suite::Suite(std::string_view Scenario, int ArgumentCount,
             const char *const *ArgumentValues)
    : ScenarioValue(Scenario), ConfigurationValue(BuiltConfiguration()) {
  for (int Index = 1; Index < ArgumentCount; ++Index) {
    const std::string_view Argument =
        ArgumentValues[Index] ? ArgumentValues[Index] : "";
    if (Argument.starts_with("--samples=")) {
      std::size_t Parsed = 0;
      if (!ParseCount(Argument.substr(10), Parsed) || Parsed == 0) {
        Reject("--samples requires a positive count");
        continue;
      }
      SampleCount = Parsed;
      continue;
    }
    if (Argument.starts_with("--warmup=")) {
      std::size_t Parsed = 0;
      if (!ParseCount(Argument.substr(9), Parsed)) {
        Reject("--warmup requires a count");
        continue;
      }
      WarmupCount = Parsed;
      continue;
    }
    if (Argument.starts_with("--correctness-evidence=")) {
      CorrectnessEvidence = std::string(Argument.substr(23));
      continue;
    }
    Reject(std::string("unknown option ").append(Argument));
  }

  if (!ConfigurationValue.IsComplete())
    Reject("the build recorded no complete benchmark configuration");
}

bool Suite::IsRunnable() const noexcept {
  return OptionsValid && ConfigurationValue.IsComplete();
}

void Suite::Reject(std::string Reason) {
  OptionsValid = false;
  ++FailureCount;
  std::cerr << "luna-benchmark rejected scenario=" << ScenarioValue
            << " reason=" << Quoted(Reason) << '\n';
}

void Suite::Consume(std::uint64_t Observed) noexcept {
  ObservedSink = ObservedSink + Observed;
}

void Suite::Block(std::string_view CaseName, std::string_view Corpus,
                  CacheMode Mode, std::string_view Reason) {
  ++CaseCount;
  ++BlockedCount;
  std::cout << "luna-benchmark blocked"
            << " scenario=" << ScenarioValue << " case=" << CaseName
            << " mode=" << CacheModeText(Mode) << " corpus=" << Quoted(Corpus)
            << " reason=" << Quoted(Reason) << " claimable=false" << '\n';
}

void Suite::Measure(std::string_view CaseName, std::string_view Corpus,
                    CacheMode Mode, std::size_t OperationCount,
                    const std::function<bool()> &Body) {
  ++CaseCount;
  if (!IsRunnable()) {
    std::cerr << "luna-benchmark skipped scenario=" << ScenarioValue
              << " case=" << CaseName
              << " reason=\"configuration or options incomplete\"\n";
    return;
  }
  if (OperationCount == 0) {
    ++FailureCount;
    std::cerr
        << "luna-benchmark invalid scenario=" << ScenarioValue
        << " case=" << CaseName
        << " reason=\"a measured case performs at least one operation\"\n";
    return;
  }

  for (std::size_t Warm = 0; Warm < WarmupCount; ++Warm) {
    if (Body())
      continue;
    ++FailureCount;
    std::cerr << "luna-benchmark functional-failure scenario=" << ScenarioValue
              << " case=" << CaseName << " phase=warmup\n";
    return;
  }

  std::vector<std::uint64_t> Samples;
  Samples.reserve(SampleCount);
  for (std::size_t Sample = 0; Sample < SampleCount; ++Sample) {
    const std::chrono::steady_clock::time_point Started =
        std::chrono::steady_clock::now();
    const bool Correct = Body();
    const std::chrono::steady_clock::time_point Ended =
        std::chrono::steady_clock::now();
    if (!Correct) {
      ++FailureCount;
      std::cerr << "luna-benchmark functional-failure scenario="
                << ScenarioValue << " case=" << CaseName << " phase=sample\n";
      return;
    }
    const auto Elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Ended - Started);
    const std::uint64_t Nanoseconds =
        Elapsed.count() < 0 ? 0 : static_cast<std::uint64_t>(Elapsed.count());
    Consume(Nanoseconds);
    Samples.push_back(Nanoseconds);
  }

  std::sort(Samples.begin(), Samples.end());
  std::uint64_t Total = 0;
  for (const std::uint64_t Nanoseconds : Samples)
    Total += Nanoseconds;
  const std::uint64_t Mean = Total / static_cast<std::uint64_t>(Samples.size());
  const std::uint64_t Median = Samples[Samples.size() / 2];
  const std::uint64_t PerOperation =
      Median / static_cast<std::uint64_t>(OperationCount);

  std::cout << "luna-benchmark result"
            << " scenario=" << ScenarioValue << " case=" << CaseName
            << " mode=" << CacheModeText(Mode) << " corpus=" << Quoted(Corpus)
            << " operations=" << OperationCount << " warmup=" << WarmupCount
            << " samples=" << SampleCount << " min_ns=" << Samples.front()
            << " median_ns=" << Median << " mean_ns=" << Mean
            << " max_ns=" << Samples.back()
            << " median_ns_per_operation=" << PerOperation
            << " build_type=" << Quoted(ConfigurationValue.BuildType)
            << " compiler=" << Quoted(ConfigurationValue.Compiler)
            << " architecture=" << Quoted(ConfigurationValue.Architecture)
            << " luau_version=" << Quoted(ConfigurationValue.LuauVersion)
            << " functional=pass"
            << " claimable=" << (CorrectnessEvidence.empty() ? "false" : "true")
            << '\n';
}

int Suite::Finish() const {
  const bool Complete = ConfigurationValue.IsComplete();
  const bool Claimable =
      !CorrectnessEvidence.empty() && BlockedCount == 0 && FailureCount == 0;
  std::cout << "luna-benchmark summary"
            << " scenario=" << ScenarioValue << " cases=" << CaseCount
            << " failures=" << FailureCount << " blocked=" << BlockedCount
            << " configuration_complete=" << (Complete ? "true" : "false")
            << " build_type=" << Quoted(ConfigurationValue.BuildType)
            << " compiler=" << Quoted(ConfigurationValue.Compiler)
            << " architecture=" << Quoted(ConfigurationValue.Architecture)
            << " luau_version=" << Quoted(ConfigurationValue.LuauVersion)
            << " warmup=" << WarmupCount << " samples=" << SampleCount << '\n';
  std::cout << "luna-benchmark guard"
            << " scenario=" << ScenarioValue
            << " requires=\"functional deterministic-output stack-safety "
               "recovery compatibility suites unchanged\""
            << " correctness_evidence="
            << Quoted(CorrectnessEvidence.empty() ? "none"
                                                  : CorrectnessEvidence)
            << " blocked=" << BlockedCount
            << " claimable=" << (Claimable ? "true" : "false") << '\n';
  if (FailureCount != 0 || !Complete)
    return 1;
  return 0;
}

} // namespace LunaBenchmark
