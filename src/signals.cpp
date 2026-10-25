#include "runtime.hpp"

#include <luna/luna.hpp>

#include <algorithm>
#include <optional>

namespace luna {
namespace detail {
struct Subscription {
  std::uint64_t Id = 0;
  bool Once = false;
  bool Alive = true;
  Delegate<void(ValuePack)> Script;
  std::function<void(const ValuePack &)> Host;
};

struct SignalCore {
  std::string Name;
  std::vector<Subscription> Subscriptions;
  std::uint64_t Next = 1;
  Runtime *Waiters = nullptr;
  std::shared_ptr<Liveness> Life;
};
} // namespace detail

namespace {
void Prune(detail::SignalCore &Core) {
  Core.Subscriptions.erase(
      std::remove_if(
          Core.Subscriptions.begin(), Core.Subscriptions.end(),
          [](const detail::Subscription &Entry) { return !Entry.Alive; }),
      Core.Subscriptions.end());
}

bool IsAlive(const detail::SignalCore &Core, std::uint64_t Id) {
  for (const detail::Subscription &Entry : Core.Subscriptions) {
    if (Entry.Id == Id)
      return Entry.Alive;
  }
  return false;
}

ScriptConnection Add(const std::shared_ptr<detail::SignalCore> &Core,
                     detail::Subscription Entry, ScriptConnection Handle) {
  Entry.Id = Core->Next++;
  Core->Subscriptions.push_back(std::move(Entry));
  return Handle;
}
} // namespace

void ScriptConnection::Disconnect() const {
  const std::shared_ptr<detail::SignalCore> Core = Owner.lock();
  if (!Core)
    return;
  for (detail::Subscription &Entry : Core->Subscriptions) {
    if (Entry.Id == Id)
      Entry.Alive = false;
  }
}

bool ScriptConnection::Connected() const {
  const std::shared_ptr<detail::SignalCore> Core = Owner.lock();
  return Core && IsAlive(*Core, Id);
}

bool ScriptConnection::operator==(const ScriptConnection &Other) const {
  return Owner.lock() == Other.Owner.lock() && Id == Other.Id;
}

ScriptSignal::ScriptSignal(std::string Name)
    : Core(std::make_shared<detail::SignalCore>()) {
  Core->Name = std::move(Name);
}

const std::string &ScriptSignal::Name() const {
  return Core->Name;
}
const void *ScriptSignal::Key() const {
  return Core.get();
}

bool ScriptSignal::operator==(const ScriptSignal &Other) const {
  return Core == Other.Core;
}

std::size_t ScriptSignal::ConnectionCount() const {
  return static_cast<std::size_t>(std::count_if(
      Core->Subscriptions.begin(), Core->Subscriptions.end(),
      [](const detail::Subscription &Entry) { return Entry.Alive; }));
}

void ScriptSignal::DisconnectAll() const {
  for (detail::Subscription &Entry : Core->Subscriptions)
    Entry.Alive = false;
}

void ScriptSignal::Bind(void *VM) const {
  State *Host = StateFromVM(VM);
  if (!Host)
    return;
  Core->Waiters = Host->GetRuntime();
  Core->Life = Host->Life();
}

ScriptConnection
ScriptSignal::Connect(std::function<void(const ValuePack &)> Handler,
                      bool Once) const {
  detail::Subscription Entry;
  Entry.Once = Once;
  Entry.Host = std::move(Handler);
  ScriptConnection Handle;
  Handle.Owner = Core;
  Handle.Id = Core->Next;
  return Add(Core, std::move(Entry), Handle);
}

ScriptConnection ScriptSignal::Connect(Delegate<void(ValuePack)> Handler,
                                       bool Once) const {
  detail::Subscription Entry;
  Entry.Once = Once;
  Entry.Script = std::move(Handler);
  ScriptConnection Handle;
  Handle.Owner = Core;
  Handle.Id = Core->Next;
  return Add(Core, std::move(Entry), Handle);
}

void ScriptSignal::Fire(ValuePack Payload) const {
  // Handlers connect and disconnect while they run, so they run from a copy.
  const std::shared_ptr<detail::SignalCore> Keep = Core;
  const std::vector<detail::Subscription> Snapshot = Core->Subscriptions;
  for (const detail::Subscription &Entry : Snapshot) {
    if (!IsAlive(*Keep, Entry.Id))
      continue;
    if (Entry.Once) {
      for (detail::Subscription &Live : Keep->Subscriptions) {
        if (Live.Id == Entry.Id)
          Live.Alive = false;
      }
    }

    if (Entry.Script.IsBound()) {
      Entry.Script.Spawn(Payload);
    } else if (Entry.Host) {
      try {
        Entry.Host(Payload);
      } catch (...) {
      }
    }
  }
  Prune(*Keep);

  // Whoever is parked on Wait resumes holding what was fired.
  if (!Keep->Waiters || !Keep->Life ||
      !Keep->Life->Alive.load(std::memory_order_acquire))
    return;
  State *Host = detail::OwnerOf(Keep->Waiters);
  if (!Host || !Host->HasWaiters(Key()))
    return;
  Host->Wake(Key(), [Payload](void *VM) {
    auto *L = static_cast<lua_State *>(VM);
    lua_checkstack(L, static_cast<int>(Payload.Size()) + 4);
    return detail::PushValuePack(L, Payload);
  });
}

RegistrationResult RegisterScriptSignals(BindingRegistry Registry,
                                         const ScriptSignalNames &Names) {
  const std::string &SignalType =
      Names.SignalType.empty() ? Names.Signal : Names.SignalType;
  const std::string &ConnectionType =
      Names.ConnectionType.empty() ? Names.Connection : Names.ConnectionType;

  ClassBuilder<ScriptConnection> Connection =
      Registry.RegisterClass<ScriptConnection>(
          Names.Connection, StableTypeKey("luna.ScriptConnection"));
  Connection.TypeName(ConnectionType)
      .Method("Disconnect",
              [](const ScriptConnection &Self) { Self.Disconnect(); })
      .Property("Connected",
                [](const ScriptConnection &Self) { return Self.Connected(); })
      .Operator(ClassOperator::Equal,
                [](const ScriptConnection &A, const ScriptConnection &B) {
                  return A == B;
                })
      .Operator(ClassOperator::ToText,
                [ConnectionType](const ScriptConnection &) {
                  return ConnectionType;
                });
  RegistrationResult Result = Connection.Commit();
  if (!Result.IsSuccess())
    return Result;

  ClassBuilder<ScriptSignal> Signal = Registry.RegisterClass<ScriptSignal>(
      Names.Signal, StableTypeKey("luna.ScriptSignal"));
  const auto Connect = [](const ScriptSignal &Self,
                          Delegate<void(ValuePack)> Handler) {
    return Self.Connect(std::move(Handler), false);
  };
  Signal.TypeName(SignalType)
      .Factory("new",
               [](std::optional<std::string> Name) {
                 return ScriptSignal(Name.value_or(std::string()));
               })
      .Method("Connect", Connect)
      .Method("ConnectParallel", Connect)
      .Method("Once",
              [](const ScriptSignal &Self, Delegate<void(ValuePack)> Handler) {
                return Self.Connect(std::move(Handler), true);
              })
      .Method("Wait",
              [](const ScriptSignal &Self, ArgumentPack Args) {
                Self.Bind(Args.GetVM());
                WaitForSignal(Self.Key());
              })
      .Method("Fire",
              [](const ScriptSignal &Self, ArgumentPack Args) {
                ValuePack Payload;
                for (std::size_t Index = 0; Index < Args.Size(); ++Index)
                  Payload.Append(Args.At(Index));
                Self.Fire(std::move(Payload));
              })
      .Method("DisconnectAll",
              [](const ScriptSignal &Self) { Self.DisconnectAll(); })
      .Property("ConnectionCount",
                [](const ScriptSignal &Self) { return Self.ConnectionCount(); })
      .Property("Name", [](const ScriptSignal &Self) { return Self.Name(); })
      .Operator(
          ClassOperator::Equal,
          [](const ScriptSignal &A, const ScriptSignal &B) { return A == B; })
      .Operator(ClassOperator::ToText, [SignalType](const ScriptSignal &Self) {
        return SignalType + " " + Self.Name();
      });
  return Signal.Commit();
}
} // namespace luna
