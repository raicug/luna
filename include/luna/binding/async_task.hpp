#pragma once

// clang-format off
#include <luna/binding/return_pack.hpp>
#include <luna/binding/value.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
// clang-format on

namespace Luna {

enum class AsyncStage { Pending, Ready, Failed, Cancelled };

[[nodiscard]] constexpr std::string_view
AsyncStageText(AsyncStage Stage) noexcept {
  switch (Stage) {
  case AsyncStage::Pending:
    return "pending";
  case AsyncStage::Ready:
    return "ready";
  case AsyncStage::Failed:
    return "failed";
  case AsyncStage::Cancelled:
    return "cancelled";
  }
  return "pending";
}

namespace Detail {

class AsyncSharedState final {
public:
  AsyncSharedState() = default;

  AsyncSharedState(const AsyncSharedState &) = delete;
  AsyncSharedState &operator=(const AsyncSharedState &) = delete;

  [[nodiscard]] AsyncStage Stage() const {
    const std::lock_guard<std::mutex> Guard(Barrier);
    return StageValue;
  }

  [[nodiscard]] bool IsSettled() const {
    const std::lock_guard<std::mutex> Guard(Barrier);
    return StageValue != AsyncStage::Pending;
  }

  [[nodiscard]] bool IsCancellationRequested() const {
    const std::lock_guard<std::mutex> Guard(Barrier);
    return CancellationRequestedValue;
  }

  void RequestCancellation() {
    {
      const std::lock_guard<std::mutex> Guard(Barrier);
      CancellationRequestedValue = true;
    }
    Settled.notify_all();
  }

  [[nodiscard]] bool Complete(std::vector<Value> Produced) {
    return Settle(AsyncStage::Ready, std::move(Produced), std::string());
  }

  [[nodiscard]] bool Fail(std::string Reason) {
    if (Reason.empty())
      Reason = "The asynchronous work failed without a reason.";
    return Settle(AsyncStage::Failed, std::vector<Value>(), std::move(Reason));
  }

  [[nodiscard]] bool Cancel(std::string Reason) {
    if (Reason.empty())
      Reason = "The asynchronous work was cancelled.";
    return Settle(AsyncStage::Cancelled, std::vector<Value>(),
                  std::move(Reason));
  }

  [[nodiscard]] std::vector<Value> TakeValues() {
    const std::lock_guard<std::mutex> Guard(Barrier);
    return std::move(ValuesValue);
  }

  [[nodiscard]] std::string Message() const {
    const std::lock_guard<std::mutex> Guard(Barrier);
    return MessageValue;
  }

  [[nodiscard]] AsyncStage WaitUntilSettled() {
    std::unique_lock<std::mutex> Guard(Barrier);
    Settled.wait(Guard, [this] {
      return StageValue != AsyncStage::Pending || CancellationRequestedValue;
    });
    return StageValue;
  }

private:
  [[nodiscard]] bool Settle(AsyncStage Reached, std::vector<Value> Produced,
                            std::string Reason) {
    {
      const std::lock_guard<std::mutex> Guard(Barrier);
      if (StageValue != AsyncStage::Pending)
        return false;
      StageValue = Reached;
      ValuesValue = std::move(Produced);
      MessageValue = std::move(Reason);
    }
    Settled.notify_all();
    return true;
  }

  mutable std::mutex Barrier;
  std::condition_variable Settled;
  AsyncStage StageValue = AsyncStage::Pending;
  bool CancellationRequestedValue = false;
  std::vector<Value> ValuesValue;
  std::string MessageValue;
};

template <class Result, class Produced>
[[nodiscard]] std::vector<Value> AsyncValueOf(Produced &&Supplied) {
  if constexpr (std::is_same_v<Result, ReturnPack>) {
    const std::span<const Value> Elements = Supplied.Values();
    return std::vector<Value>(Elements.begin(), Elements.end());
  } else {
    std::vector<Value> Staged;
    Staged.push_back(
        Value(std::in_place_type<Result>, std::forward<Produced>(Supplied)));
    return Staged;
  }
}

template <class Result, class... Produced>
[[nodiscard]] std::vector<Value> AsyncValuesOf(Produced &&...Supplied) {
  if constexpr (sizeof...(Produced) == 0) {
    static_assert(std::is_void_v<Result>,
                  "Completing a value-producing Luna task requires a value.");
    return std::vector<Value>();
  } else {
    static_assert(!std::is_void_v<Result>,
                  "Completing a void Luna task publishes no value.");
    return AsyncValueOf<Result>(std::forward<Produced>(Supplied)...);
  }
}

} // namespace Detail

template <class Result> class AsyncTask final {
public:
  using ResultType = Result;

  AsyncTask() = default;

  explicit AsyncTask(std::shared_ptr<Detail::AsyncSharedState> Shared) noexcept
      : SharedValue(std::move(Shared)) {}

  [[nodiscard]] bool IsValid() const noexcept { return SharedValue != nullptr; }

  [[nodiscard]] AsyncStage Stage() const {
    return SharedValue ? SharedValue->Stage() : AsyncStage::Failed;
  }

  [[nodiscard]] const std::shared_ptr<Detail::AsyncSharedState> &
  Shared() const noexcept {
    return SharedValue;
  }

private:
  std::shared_ptr<Detail::AsyncSharedState> SharedValue;
};

template <class Result> class AsyncCompletionSource final {
public:
  using ResultType = Result;

  AsyncCompletionSource()
      : SharedValue(std::make_shared<Detail::AsyncSharedState>()) {}

  [[nodiscard]] AsyncTask<Result> Task() const {
    return AsyncTask<Result>(SharedValue);
  }

  [[nodiscard]] AsyncStage Stage() const {
    return SharedValue ? SharedValue->Stage() : AsyncStage::Failed;
  }

  [[nodiscard]] bool IsCancellationRequested() const {
    return SharedValue && SharedValue->IsCancellationRequested();
  }

  template <class... Produced>
  [[nodiscard]] bool Complete(Produced &&...Supplied) {
    if (!SharedValue)
      return false;
    return SharedValue->Complete(
        Detail::AsyncValuesOf<Result>(std::forward<Produced>(Supplied)...));
  }

  [[nodiscard]] bool Fail(std::string Reason) {
    return SharedValue && SharedValue->Fail(std::move(Reason));
  }

  [[nodiscard]] bool Cancel(std::string Reason) {
    return SharedValue && SharedValue->Cancel(std::move(Reason));
  }

private:
  std::shared_ptr<Detail::AsyncSharedState> SharedValue;
};

namespace Detail {

class PendingAsyncWork {
public:
  PendingAsyncWork() = default;
  virtual ~PendingAsyncWork() = default;

  PendingAsyncWork(const PendingAsyncWork &) = delete;
  PendingAsyncWork &operator=(const PendingAsyncWork &) = delete;

  [[nodiscard]] virtual AsyncStage Poll() = 0;
  [[nodiscard]] virtual AsyncStage Await() = 0;
  [[nodiscard]] virtual bool IsCancellable() const noexcept = 0;
  virtual void RequestCancellation() noexcept = 0;
  [[nodiscard]] virtual bool Cancel(std::string Reason) noexcept = 0;
  [[nodiscard]] virtual std::vector<Value> TakeValues() = 0;
  [[nodiscard]] virtual std::string Message() const = 0;
};

template <class Result> class TaskAsyncWork final : public PendingAsyncWork {
public:
  explicit TaskAsyncWork(AsyncTask<Result> Pending) noexcept
      : SharedValue(Pending.Shared()) {}

  [[nodiscard]] AsyncStage Poll() override {
    if (!SharedValue)
      return AsyncStage::Failed;
    return SharedValue->Stage();
  }

  [[nodiscard]] AsyncStage Await() override {
    if (!SharedValue)
      return AsyncStage::Failed;
    return SharedValue->WaitUntilSettled();
  }

  [[nodiscard]] bool IsCancellable() const noexcept override { return true; }

  void RequestCancellation() noexcept override {
    if (SharedValue)
      SharedValue->RequestCancellation();
  }

  [[nodiscard]] bool Cancel(std::string Reason) noexcept override {
    if (!SharedValue)
      return false;
    return SharedValue->Cancel(std::move(Reason));
  }

  [[nodiscard]] std::vector<Value> TakeValues() override {
    if (!SharedValue)
      return std::vector<Value>();
    return SharedValue->TakeValues();
  }

  [[nodiscard]] std::string Message() const override {
    if (!SharedValue)
      return std::string("the callable produced no asynchronous task");
    return SharedValue->Message();
  }

private:
  std::shared_ptr<AsyncSharedState> SharedValue;
};

template <class Result> class FutureAsyncWork final : public PendingAsyncWork {
public:
  explicit FutureAsyncWork(std::future<Result> Pending) noexcept
      : FutureValue(std::move(Pending)) {}

  [[nodiscard]] AsyncStage Poll() override {
    if (StageValue != AsyncStage::Pending)
      return StageValue;
    if (!FutureValue.valid())
      return Abandon("the callable produced no asynchronous result");

    const std::future_status Status =
        FutureValue.wait_for(std::chrono::seconds(0));
    if (Status == std::future_status::timeout)
      return AsyncStage::Pending;
    return Harvest();
  }

  [[nodiscard]] AsyncStage Await() override {
    if (StageValue != AsyncStage::Pending)
      return StageValue;
    if (!FutureValue.valid())
      return Abandon("the callable produced no asynchronous result");

    FutureValue.wait();
    return Harvest();
  }

  [[nodiscard]] bool IsCancellable() const noexcept override { return false; }

  void RequestCancellation() noexcept override {}

  [[nodiscard]] bool Cancel(std::string Reason) noexcept override {
    if (StageValue != AsyncStage::Pending)
      return false;
    StageValue = AsyncStage::Cancelled;
    try {
      MessageValue = std::move(Reason);
    } catch (...) {
      MessageValue.clear();
    }
    return true;
  }

  [[nodiscard]] std::vector<Value> TakeValues() override {
    return std::move(ValuesValue);
  }

  [[nodiscard]] std::string Message() const override { return MessageValue; }

private:
  [[nodiscard]] AsyncStage Abandon(std::string Reason) {
    StageValue = AsyncStage::Failed;
    MessageValue = std::move(Reason);
    return StageValue;
  }

  [[nodiscard]] AsyncStage Harvest() {
    try {
      if constexpr (std::is_void_v<Result>) {
        FutureValue.get();
        ValuesValue.clear();
      } else {
        ValuesValue = AsyncValueOf<Result>(FutureValue.get());
      }
      StageValue = AsyncStage::Ready;
      return StageValue;
    } catch (const std::exception &Error) {
      return Abandon(std::string("the asynchronous result reported: ") +
                     Error.what());
    } catch (...) {
      return Abandon("the asynchronous result reported an unknown failure");
    }
  }

  std::future<Result> FutureValue;
  AsyncStage StageValue = AsyncStage::Pending;
  std::vector<Value> ValuesValue;
  std::string MessageValue;
};

template <class Type> struct AsyncReturnTrait : std::false_type {
  using ResultType = void;
};

template <class Result>
struct AsyncReturnTrait<AsyncTask<Result>> : std::true_type {
  using ResultType = Result;
};

template <class Result>
struct AsyncReturnTrait<std::future<Result>> : std::true_type {
  using ResultType = Result;
};

template <class Result>
[[nodiscard]] std::unique_ptr<PendingAsyncWork>
MakePendingAsyncWork(AsyncTask<Result> Pending) {
  return std::make_unique<TaskAsyncWork<Result>>(std::move(Pending));
}

template <class Result>
[[nodiscard]] std::unique_ptr<PendingAsyncWork>
MakePendingAsyncWork(std::future<Result> Pending) {
  return std::make_unique<FutureAsyncWork<Result>>(std::move(Pending));
}

} // namespace Detail

} // namespace Luna
