// clang-format off
#include <luna/binding/async_task.hpp>

#include <future>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
// clang-format on

namespace {

using IntegerTask = Luna::AsyncTask<int>;
using IntegerSource = Luna::AsyncCompletionSource<int>;

static_assert(std::is_default_constructible_v<IntegerTask>,
              "An asynchronous task must be default constructible.");
static_assert(std::is_same_v<IntegerTask::ResultType, int>,
              "An asynchronous task must publish its result type.");
static_assert(
    std::is_same_v<decltype(std::declval<const IntegerSource &>().Task()),
                   IntegerTask>,
    "A completion source must hand out its task.");
static_assert(
    std::is_same_v<decltype(std::declval<const IntegerTask &>().Stage()),
                   Luna::AsyncStage>,
    "An asynchronous task must publish its stage.");

static_assert(Luna::AsyncStageText(Luna::AsyncStage::Pending) == "pending" &&
                  Luna::AsyncStageText(Luna::AsyncStage::Ready) == "ready" &&
                  Luna::AsyncStageText(Luna::AsyncStage::Failed) == "failed" &&
                  Luna::AsyncStageText(Luna::AsyncStage::Cancelled) ==
                      "cancelled",
              "Every asynchronous stage must format canonically.");

static_assert(Luna::Detail::AsyncReturnTrait<IntegerTask>::value,
              "A Luna task must be recognised as asynchronous.");
static_assert(Luna::Detail::AsyncReturnTrait<std::future<int>>::value,
              "A standard future must be recognised as asynchronous.");
static_assert(!Luna::Detail::AsyncReturnTrait<int>::value,
              "An ordinary value must not be recognised as asynchronous.");

} // namespace

void VerifyAsyncTaskHeaderCompilesStandalone() {
  IntegerSource Source;
  IntegerTask Pending = Source.Task();
  const bool Completed = Source.Complete(7);
  const bool Repeated = Source.Fail("ignored");

  Luna::AsyncCompletionSource<void> VoidSource;
  const bool VoidCompleted = VoidSource.Complete();

  Luna::AsyncCompletionSource<Luna::ReturnPack> PackSource;
  Luna::ReturnPack Produced;
  Produced.AppendInteger(1).AppendText("two");
  const bool PackCompleted = PackSource.Complete(Produced);

  static_cast<void>(Completed);
  static_cast<void>(Repeated);
  static_cast<void>(VoidCompleted);
  static_cast<void>(PackCompleted);
  static_cast<void>(Pending.Stage());
}
