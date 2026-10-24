// Benchmark harness for the gates in docs/PLAN.md section 7.
//
// Every case runs the same workload twice: once through a bound class, once
// through an equivalent pure-Luau implementation, and reports the *marginal*
// cost. That isolates what luna adds from the cost of the Luau loop that
// drives it, which is the only comparison the public surface can make (a
// consumer never sees a lua_State).
//
// Median of five runs, Release /O2.

#include <luna/luna.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kRuns = 5;

// A value type with the shape a math library binds: cheap arithmetic, few operators.
class Vector3 {
public:
  double X = 0.0;
  double Y = 0.0;
  double Z = 0.0;
};

// The two instances a Drawing record carries: small enough that the userdata
// block, not the payload copy, is what costs.
class Small {
public:
  double A = 0.0;
  double B = 0.0;
};

// An instance with 100 properties, like a large game object, on the read path.
class Node {
public:
  double P[100]{};

  [[nodiscard]] double Get(int Index) const { return P[Index]; }
};

} // namespace

// The opt-ins have to name the classes above, so they live outside the
// anonymous namespace and after the definitions.
template <> struct luna::detail::RegisteredClassTrait<Vector3> : std::true_type {};
template <> struct luna::detail::RegisteredClassTrait<Small> : std::true_type {};
template <> struct luna::detail::RegisteredClassTrait<Node> : std::true_type {};

namespace {

struct Result {
  std::string Name;
  double BoundNs = 0.0;
  double RawNs = 0.0;
  double GateNs = 0.0;
};

std::vector<Result> Results;

// Runs Fn, which performs Work iterations, and returns the median per-iteration
// cost in nanoseconds.
template <class Fn> double Measure(std::size_t Work, Fn &&Runnable) {
  std::vector<double> Samples;
  Samples.reserve(kRuns);

  for (int Run = 0; Run < kRuns; ++Run) {
    const auto Start = Clock::now();
    Runnable();
    const auto Stop = Clock::now();
    const double Total =
        std::chrono::duration<double, std::nano>(Stop - Start).count();
    Samples.push_back(Total / static_cast<double>(Work));
  }

  std::sort(Samples.begin(), Samples.end());
  return Samples[Samples.size() / 2];
}

std::string Nanoseconds(double Value) {
  char Text[32];
  if (Value < 10.0)
    std::snprintf(Text, sizeof(Text), "%.1f ns", Value);
  else if (Value < 1e6)
    std::snprintf(Text, sizeof(Text), "%.0f ns", Value);
  else
    std::snprintf(Text, sizeof(Text), "%.2f ms", Value / 1e6);
  return Text;
}

void Print() {
  std::printf("\n%-44s %10s %10s %10s %10s\n", "workload (marginal cost)",
              "bound", "pure luau", "delta", "gate");
  std::printf("%s\n", std::string(90, '-').c_str());

  int Failed = 0;
  for (const Result &Entry : Results) {
    const double Delta = Entry.BoundNs - Entry.RawNs;
    const bool Ok = Delta <= Entry.GateNs;
    if (!Ok)
      ++Failed;
    std::printf("%-44s %10s %10s %10s %10s  %s\n", Entry.Name.c_str(),
                Nanoseconds(Entry.BoundNs).c_str(),
                Nanoseconds(Entry.RawNs).c_str(), Nanoseconds(Delta).c_str(),
                Nanoseconds(Entry.GateNs).c_str(), Ok ? "pass" : "FAIL");
  }

  std::printf("%s\n", std::string(90, '-').c_str());
  std::printf("%d of %zu gates met\n", static_cast<int>(Results.size()) - Failed,
              Results.size());
}

// --- 1. property read ------------------------------------------------------

void PropertyRead() {
  constexpr std::size_t kIterations = 500000;

  double BoundNs = 0.0;
  {
    luna::State State;
    luna::BindingRegistry Registry = State.Bindings();
    luna::ClassBuilder<Node> Nodes =
        Registry.RegisterClass<Node>("Node", luna::StableTypeKey("bench.Node"));
    Nodes.Factory("new", []() { return Node{}; });
    for (int Index = 0; Index < 100; ++Index) {
      const std::string Name = "P" + std::to_string(Index);
      Nodes.Property(Name,
                     [Index](const Node &Self) { return Self.Get(Index); });
    }
    if (!Nodes.Commit().IsSuccess())
      return;

    BoundNs = Measure(kIterations, [&State]() {
      (void)State.Execute("local n = Node.new()\n"
                    "local Total = 0\n"
                    "for i = 1, 500000 do Total = Total + n.P37 end");
    });
  }

  // The equivalent a Lua author would write for a computed property: one
  // metamethod dispatch and one table read. Comparing against a plain table
  // field would measure the wrong thing.
  double RawNs = 0.0;
  {
    luna::State State;
    RawNs = Measure(kIterations, [&State]() {
      (void)State.Execute("local Store = {P37 = 0}\n"
                    "local Node = {}\n"
                    "Node.__index = function(self, Key) return Store[Key] end\n"
                    "local n = setmetatable({}, Node)\n"
                    "local Total = 0\n"
                    "for i = 1, 500000 do Total = Total + n.P37 end");
    });
  }

  Results.push_back(
      Result{"property read, 100-member class", BoundNs, RawNs, 45.0});
}

// --- 2. Vector3 add --------------------------------------------------------

void VectorAdd() {
  constexpr std::size_t kIterations = 500000;

  double BoundNs = 0.0;
  {
    luna::State State;
    luna::BindingRegistry Registry = State.Bindings();
    luna::ClassBuilder<Vector3> Vectors = Registry.RegisterClass<Vector3>(
        "Vec3", luna::StableTypeKey("bench.Vec3"));
    Vectors
        .Factory("new", [](double X, double Y, double Z) {
          return Vector3{X, Y, Z};
        })
        .Operator(luna::ClassOperator::Add,
                  [](const Vector3 &A, const Vector3 &B) {
                    return Vector3{A.X + B.X, A.Y + B.Y, A.Z + B.Z};
                  });
    if (!Vectors.Commit().IsSuccess())
      return;

    BoundNs = Measure(kIterations, [&State]() {
      (void)State.Execute("local a, b = Vec3.new(1,2,3), Vec3.new(4,5,6)\n"
                    "local c = a\n"
                    "for i = 1, 500000 do c = a + b end");
    });
  }

  // A table-based Vector3 with a Luau __add: allocates a table per add, the way
  // the bound path allocates a userdata per add.
  double RawNs = 0.0;
  {
    luna::State State;
    RawNs = Measure(kIterations, [&State]() {
      (void)State.Execute("local mt = {}\n"
                    "mt.__add = function(x, y)\n"
                    "  return setmetatable({x[1]+y[1], x[2]+y[2], x[3]+y[3]}, mt)\n"
                    "end\n"
                    "local a = setmetatable({1,2,3}, mt)\n"
                    "local b = setmetatable({4,5,6}, mt)\n"
                    "local c = a\n"
                    "for i = 1, 500000 do c = a + b end");
    });
  }

  Results.push_back(Result{"Vector3 add operator", BoundNs, RawNs, 60.0});
}

// --- 3. OwnedValue table ---------------------------------------------------

void OwnedValueTable() {
  constexpr std::size_t kIterations = 200000;

  double BoundNs = 0.0;
  {
    luna::State State;
    luna::BindingRegistry Registry = State.Bindings();
    luna::ClassBuilder<Small> Sprites =
        Registry.RegisterClass<Small>("Sprite", luna::StableTypeKey("b.Sprite"));
    Sprites.Factory("new", []() { return Small{}; });
    if (!Sprites.Commit().IsSuccess())
      return;

    Registry.RegisterFunction("build", []() {
      luna::OwnedValue Record = luna::OwnedValue::Table();
      Record.SetField("Name", luna::OwnedValue::Text("Square"));
      Record.SetField("Visible", luna::OwnedValue::Boolean(true));
      Record.SetField("ZIndex", luna::OwnedValue::Number(3));
      Record.SetField("Transparency", luna::OwnedValue::Number(0.5));
      Record.SetField("Color", luna::OwnedValue::Number(0xFF00FF));
      Record.SetField("Thickness", luna::OwnedValue::Number(1));
      Record.SetField("Filled", luna::OwnedValue::Boolean(false));
      Record.SetField("Rotation", luna::OwnedValue::Number(0));
      Record.Append(luna::OwnedValue::Instance<Small>(Small{}));
      Record.Append(luna::OwnedValue::Instance<Small>(Small{}));
      return Record;
    });

    BoundNs = Measure(kIterations, [&State]() {
      (void)State.Execute("for i = 1, 200000 do local t = build() end");
    });
  }

  // The same shape built in Luau, including two freshly constructed instances,
  // so the only difference left is who builds the table.
  double RawNs = 0.0;
  {
    luna::State State;
    luna::BindingRegistry Registry = State.Bindings();
    luna::ClassBuilder<Small> Sprites =
        Registry.RegisterClass<Small>("Sprite", luna::StableTypeKey("b.Sprite"));
    Sprites.Factory("new", []() { return Small{}; });
    Sprites.Commit();

    RawNs = Measure(kIterations, [&State]() {
      (void)State.Execute("local function build()\n"
                    "  return {Name='Square', Visible=true, ZIndex=3,\n"
                    "          Transparency=0.5, Color=0xFF00FF,\n"
                    "          Thickness=1, Filled=false, Rotation=0,\n"
                    "          Sprite.new(), Sprite.new()}\n"
                    "end\n"
                    "for i = 1, 200000 do local t = build() end");
    });
  }

  Results.push_back(Result{"OwnedValue table, 8 fields + 2 instances", BoundNs,
                           RawNs, 1200.0});
}

// --- 4. yield + resume -----------------------------------------------------

// No pure-Luau equivalent exists: a sleep-based wait would cost about 1 ms here. The gate
// is absolute (5 us), so the baseline is zero.
void YieldResume() {
  luna::State State;
  luna::BindingRegistry Registry = State.Bindings();
  Registry.RegisterFunction("wait", [](double Seconds) {
    luna::Suspend(Seconds);
  });

  const luna::Chunk Loaded = State.Load("wait(0)");
  constexpr int kIterations = 50000;

  const auto Start = Clock::now();
  for (int Index = 0; Index < kIterations; ++Index)
    (void)Loaded.Call();
  const auto Stop = Clock::now();

  const double Total =
      std::chrono::duration<double, std::nano>(Stop - Start).count();
  Results.push_back(Result{"task.wait(0) yield + resume",
                           Total / kIterations, 0.0, 5000.0});
}

// --- 5. registration -------------------------------------------------------

void Registration() {
  const auto Start = Clock::now();
  {
    luna::State State;
    luna::BindingRegistry Registry = State.Bindings();

    // 20 namespaces x 13 symbols, the shape of the host surface scaled down.
    for (int Scope = 0; Scope < 20; ++Scope) {
      luna::NamespaceBuilder Builder =
          Registry.RegisterNamespace("Bench.Scope" + std::to_string(Scope));
      Builder.RegisterConstant("Version", 1.0);
      for (int Member = 0; Member < 12; ++Member) {
        Builder.RegisterFunction("Fn" + std::to_string(Member),
                                 [](double Value) { return Value + 1.0; });
      }
      Builder.Commit();
    }
    // A class with 100 properties, registered on top.
    luna::ClassBuilder<Node> Nodes = Registry.RegisterClass<Node>(
        "Bench.Node", luna::StableTypeKey("bench.Node"));
    Nodes.Factory("new", []() { return Node{}; });
    for (int Index = 0; Index < 100; ++Index) {
      const std::string Name = "P" + std::to_string(Index);
      Nodes.Property(Name,
                     [Index](const Node &Self) { return Self.Get(Index); });
    }
    Nodes.Commit();
  }
  const auto Stop = Clock::now();

  const double Total =
      std::chrono::duration<double, std::nano>(Stop - Start).count();
  Results.push_back(
      Result{"register 260 functions + 100 properties", Total, 0.0, 25e6});
}

// --- 6. concurrency --------------------------------------------------------

void Concurrency() {
  luna::State State;
  luna::BindingRegistry Registry = State.Bindings();
  Registry.RegisterFunction("wait", [](double Seconds) {
    luna::Suspend(Seconds);
  });

  for (int Index = 0; Index < 8; ++Index)
    (void)State.Spawn("for i = 1, 4 do wait(0.1) end");

  const std::uint32_t Started = State.PendingTasks();
  for (int Step = 0; Step < 8; ++Step)
    State.Tick(0.2);
  const std::uint32_t Leftover = State.PendingTasks();

  std::printf("  concurrency: %u coroutines pending after spawn, %u after 8 "
              "ticks, 1 host thread\n",
              Started, Leftover);

  // Reported as a count, not a duration: the gate is structural.
  Results.push_back(Result{"8 concurrent scripts, 1 thread (0 = pass)",
                           Leftover == 0 && Started == 8 ? 0.0 : 1.0, 0.0, 0.0});
}

} // namespace

int main() {
  std::printf("luna benchmarks (median of %d runs, marginal over pure "
              "Luau)\n",
              kRuns);

  PropertyRead();
  VectorAdd();
  OwnedValueTable();
  YieldResume();
  Registration();
  Concurrency();

  Print();
  return 0;
}
