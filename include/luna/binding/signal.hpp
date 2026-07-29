#pragma once

// clang-format off
#include <luna/binding/delegate.hpp>

#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>
// clang-format on

namespace Luna {

// What one emission delivered. Skipped counts subscribers a handler removed
// or released while the emission was in flight.
struct SignalEmission final {
  std::size_t Delivered = 0;
  std::size_t Skipped = 0;
  std::size_t Failed = 0;
  std::string Diagnostic;

  [[nodiscard]] bool IsComplete() const noexcept { return Failed == 0; }
};

// A native event source. It owns subscribed handlers as ordinary delegates,
// so subscribing, unsubscribing, and emitting stay ordinary reflected
// callables rather than a parallel callback system.
//
// Emitting takes one snapshot of its subscribers, so a handler may subscribe
// or unsubscribe while the emission runs: a handler removed during the
// emission is never called, a handler added during the emission is delivered
// by the next emission, and no handler is called twice.
template <class Signature> class Signal;

template <class Return, class... Parameters>
class Signal<Return(Parameters...)> final {
public:
  using HandlerType = Delegate<Return(Parameters...)>;

  Signal() = default;

  Signal(const Signal &) = delete;
  Signal &operator=(const Signal &) = delete;
  Signal(Signal &&) noexcept = default;
  Signal &operator=(Signal &&) noexcept = default;

  // Returns the subscription token, or zero when the handler names no live
  // subscriber or the token space is exhausted.
  [[nodiscard]] int Subscribe(HandlerType Handler) {
    if (!Handler.IsValid())
      return 0;
    if (NextTokenValue == std::numeric_limits<int>::max())
      return 0;
    const int Token = NextTokenValue;
    NextTokenValue += 1;
    EntriesValue.push_back(Entry{Token, std::move(Handler)});
    return Token;
  }

  // Releases the handler's own virtual-machine reference before dropping the
  // subscription, so every copy of the unsubscribed handler observes the
  // release and a later call through any of them reports it deterministically.
  [[nodiscard]] bool Unsubscribe(int Token) {
    for (std::size_t Index = 0; Index < EntriesValue.size(); ++Index) {
      if (EntriesValue[Index].Token != Token)
        continue;
      EntriesValue[Index].Handler.Release();
      EntriesValue.erase(EntriesValue.begin() +
                         static_cast<std::ptrdiff_t>(Index));
      return true;
    }
    return false;
  }

  [[nodiscard]] bool IsSubscribed(int Token) const noexcept {
    for (const Entry &Subscribed : EntriesValue) {
      if (Subscribed.Token == Token)
        return true;
    }
    return false;
  }

  [[nodiscard]] std::size_t SubscriberCount() const noexcept {
    return EntriesValue.size();
  }

  [[nodiscard]] std::size_t LiveSubscriberCount() const noexcept {
    std::size_t Live = 0;
    for (const Entry &Subscribed : EntriesValue) {
      if (Subscribed.Handler.IsValid())
        ++Live;
    }
    return Live;
  }

  [[nodiscard]] std::size_t EmitDepth() const noexcept { return DepthValue; }

  // Releases every subscribed handler's own reference before dropping it.
  void Clear() noexcept {
    for (Entry &Subscribed : EntriesValue)
      Subscribed.Handler.Release();
    EntriesValue.clear();
  }

  [[nodiscard]] SignalEmission Emit(Parameters... Arguments) {
    SignalEmission Reported;
    const std::vector<Entry> Snapshot = EntriesValue;
    DepthValue += 1;

    for (const Entry &Subscribed : Snapshot) {
      if (!IsSubscribed(Subscribed.Token) || !Subscribed.Handler.IsValid()) {
        Reported.Skipped += 1;
        continue;
      }

      const DelegateCallResult Result = Subscribed.Handler.Invoke(Arguments...);
      if (Result.IsSuccess()) {
        Reported.Delivered += 1;
        continue;
      }
      Reported.Failed += 1;
      if (Reported.Diagnostic.empty())
        Reported.Diagnostic = Result.Diagnostic();
    }

    DepthValue -= 1;
    if (DepthValue == 0)
      DropReleasedSubscribers();
    return Reported;
  }

private:
  struct Entry final {
    int Token = 0;
    HandlerType Handler;
  };

  void DropReleasedSubscribers() noexcept {
    std::size_t Kept = 0;
    for (std::size_t Index = 0; Index < EntriesValue.size(); ++Index) {
      if (!EntriesValue[Index].Handler.IsValid())
        continue;
      if (Kept != Index)
        EntriesValue[Kept] = std::move(EntriesValue[Index]);
      ++Kept;
    }
    EntriesValue.resize(Kept);
  }

  std::vector<Entry> EntriesValue;
  int NextTokenValue = 1;
  std::size_t DepthValue = 0;
};

} // namespace Luna
