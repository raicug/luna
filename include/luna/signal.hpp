#pragma once

// Signals: host-fired events that a script can connect to and wait on.

#include <luna/bound.hpp>
#include <luna/fwd.hpp>
#include <luna/state.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <tuple>
#include <utility>
#include <type_traits>
#include <vector>

namespace luna {
// Parks the running chunk until State::Wake(Key); ticking alone never resumes
// it.
void WaitForSignal(const void *Key);

class Connection {
public:
  Connection() = default;
  explicit Connection(std::function<void()> Detach) : Fn(std::move(Detach)) {}

  [[nodiscard]] bool Connected() const noexcept {
    return static_cast<bool>(Fn);
  }

  void Disconnect() {
    if (Fn) {
      std::function<void()> Once = std::move(Fn);
      Once();
    }
  }

private:
  std::function<void()> Fn;
};

struct FireReport {
  std::size_t Invoked = 0;
  std::size_t Failed = 0;
  std::string FirstError;
  [[nodiscard]] bool IsClean() const noexcept { return Failed == 0; }
};

template <class Signature> class Signal;

template <class... A> class Signal<void(A...)> {
public:
  using Handler = std::function<void(A...)>;
  using ScriptHandler = Delegate<void(A...)>;
  Signal() = default;
  Signal(const Signal &) = delete;
  Signal &operator=(const Signal &) = delete;
  Connection Connect(Handler Callback) {
    return Add([Callback = std::move(Callback)](A... Args) -> std::string {
      try {
        Callback(std::forward<A>(Args)...);
        return std::string();
      } catch (const std::exception &Failure) {
        return Failure.what();
      } catch (...) {
        return "the handler threw an unknown exception";
      }
    });
  }

  Connection Connect(ScriptHandler Callback) {
    return Add([Callback = std::move(Callback)](A... Args) -> std::string {
      Callback.Spawn(std::forward<A>(Args)...);
      return std::string();
    });
  }

  std::size_t ConnectScript(ScriptHandler Callback) {
    const std::size_t Id = Next;
    Slots.push_back(Slot{Next++, Make(Callback)});
    return Id;
  }

  bool DisconnectScript(std::size_t Id) {
    for (std::size_t Index = 0; Index < Slots.size(); ++Index) {
      if (Slots[Index].Id != Id)
        continue;
      Slots.erase(Slots.begin() + static_cast<std::ptrdiff_t>(Index));
      return true;
    }
    return false;
  }

  FireReport Fire(A... Args) {
    FireReport Report;
    const std::vector<Slot> Snapshot = Slots;
    for (const Slot &Entry : Snapshot) {
      if (!IsLive(Entry.Id))
        continue;
      std::string Failure;
      try {
        // Each handler gets its own copy, so one handler cannot consume the
        // value.
        Failure = Entry.Callback(Args...);
      } catch (const std::exception &Thrown) {
        Failure = Thrown.what();
      } catch (...) {
        Failure = "the handler threw an unknown exception";
      }
      ++Report.Invoked;
      if (!Failure.empty()) {
        ++Report.Failed;
        if (Report.FirstError.empty())
          Report.FirstError = std::move(Failure);
      }
    }

    if (State *Host = AttachedState()) {
      if (Host->HasWaiters(this)) {
        Host->Wake(this, [Fired = std::tuple<A...>(Args...)](void *VM) {
          auto *L = static_cast<lua_State *>(VM);
          lua_checkstack(L, static_cast<int>(sizeof...(A)) * 4 + 8);
          int Pushed = 0;
          std::apply(
              [&](const auto &...Value) {
                ((Pushed += detail::WriteValue(L, Value, NoType)), ...);
              },
              Fired);
          return Pushed;
        });
      }
    }

    return Report;
  }

  [[nodiscard]] std::size_t HandlerCount() const noexcept {
    return Slots.size();
  }

  void Clear() noexcept { Slots.clear(); }

  // Held through the runtime and its liveness token, so moving the State is
  // fine and a State that has gone is noticed rather than called.
  void Attach(State &InHost) noexcept {
    HostCore = InHost.GetRuntime();
    HostLife = InHost.Life();
  }

  [[nodiscard]] State *AttachedState() const noexcept {
    if (!HostCore || !HostLife ||
        !HostLife->Alive.load(std::memory_order_acquire))
      return nullptr;
    return detail::OwnerOf(HostCore);
  }

private:
  using Sink = std::function<std::string(A...)>;
  struct Slot {
    std::size_t Id = 0;
    Sink Callback;
  };
  Connection Add(Sink Callback) {
    const std::size_t Id = Next++;
    Slots.push_back(Slot{Id, std::move(Callback)});
    return Connection([this, Id] { Erase(Id); });
  }

  static Sink Make(ScriptHandler Callback) {
    return [Callback = std::move(Callback)](A... Args) -> std::string {
      Callback.Spawn(std::forward<A>(Args)...);
      return std::string();
    };
  }

  [[nodiscard]] bool IsLive(std::size_t Id) const noexcept {
    for (const Slot &Entry : Slots)
      if (Entry.Id == Id)
        return true;
    return false;
  }

  void Erase(std::size_t Id) {
    for (std::size_t Index = 0; Index < Slots.size(); ++Index) {
      if (Slots[Index].Id == Id) {
        Slots.erase(Slots.begin() + static_cast<std::ptrdiff_t>(Index));
        return;
      }
    }
  }

  std::vector<Slot> Slots;
  std::size_t Next = 1;
  detail::Runtime *HostCore = nullptr;
  std::shared_ptr<detail::Liveness> HostLife;
};

template <class... A>
struct detail::RegisteredClassTrait<Signal<void(A...)>> : std::true_type {};

template <class... A>
void ExposeSignal(ClassBuilder<Signal<void(A...)>> &Builder) {
  Builder.Method("Connect", &Signal<void(A...)>::ConnectScript);
  Builder.Method("Disconnect", &Signal<void(A...)>::DisconnectScript);
  Builder.Method("Wait",
                 [](Signal<void(A...)> &Self) { WaitForSignal(&Self); });
  Builder.Method("GetHandlerCount", &Signal<void(A...)>::HandlerCount);
}

template <class... A>
void RegisterSignal(BindingRegistry Registry, std::string_view Name,
                    StableTypeKey Key, Signal<void(A...)> &Target) {
  ClassBuilder<Signal<void(A...)>> Builder =
      Registry.RegisterClass<Signal<void(A...)>>(Name, Key);
  ExposeSignal<A...>(Builder);
  Builder.Commit();
  Registry.RegisterValue(
      Name, [&Target]() -> Signal<void(A...)> * { return &Target; },
      OwnershipPolicy::Borrowed(LifetimeHandle()));
}

namespace detail {
struct SignalCore;
}

// A connection a script holds: it can disconnect it and ask if it is still
// live.
class ScriptConnection {
public:
  ScriptConnection() = default;
  void Disconnect() const;
  [[nodiscard]] bool Connected() const;
  bool operator==(const ScriptConnection &Other) const;

private:
  friend class ScriptSignal;
  std::weak_ptr<detail::SignalCore> Owner;
  std::uint64_t Id = 0;
};

// A signal whose payload is decided at fire time, shaped for scripts: they
// connect with Connect or Once, get a connection back, and Wait for the next
// fire. A script handler runs as a task of its own, so it may wait. Copies
// share one list of handlers, so handing one to a script is cheap.
class ScriptSignal {
public:
  explicit ScriptSignal(std::string Name = std::string());

  [[nodiscard]] const std::string &Name() const;
  void Fire(ValuePack Payload) const;
  template <class... A> void Fire(A &&...Values) const {
    ValuePack Payload;
    (Payload.Append(OwnedValue(std::forward<A>(Values))), ...);
    Fire(std::move(Payload));
  }

  ScriptConnection Connect(std::function<void(const ValuePack &)> Handler,
                           bool Once = false) const;
  ScriptConnection Connect(Delegate<void(ValuePack)> Handler,
                           bool Once = false) const;
  [[nodiscard]] std::size_t ConnectionCount() const;
  void DisconnectAll() const;

  // Tells the signal which state a script waits in, so Fire can wake it. A
  // script-facing call does this itself; call it for a signal only the host
  // uses.
  void Bind(void *VM) const;
  [[nodiscard]] const void *Key() const;
  bool operator==(const ScriptSignal &Other) const;

private:
  std::shared_ptr<detail::SignalCore> Core;
};

// The names the two classes take in a script, and what typeof reports for them.
struct ScriptSignalNames {
  std::string Signal = "Signal";
  std::string Connection = "Connection";
  std::string SignalType;
  std::string ConnectionType;
};

// Registers the signal and connection classes. A script can then make one with
// Signal.new(), connect, wait and fire; the host hands signals out by value.
RegistrationResult
RegisterScriptSignals(BindingRegistry Registry,
                      const ScriptSignalNames &Names = ScriptSignalNames());
} // namespace luna

template <>
struct luna::detail::RegisteredClassTrait<luna::ScriptSignal> : std::true_type {
};
template <>
struct luna::detail::RegisteredClassTrait<luna::ScriptConnection>
    : std::true_type {};
