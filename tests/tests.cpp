// Behavioural tests for luna. Each case is a script that exercises one
// contract; the assertions live in C++ where the host can see them.

#include <luna/luna.hpp>

#include <cstdio>
#include <string>
#include <vector>

// A consumer class and its trait opt-in have to sit at namespace scope; the
// opt-in is a specialisation of a luna template, so it cannot be hidden in an
// anonymous namespace.
struct Point {
  double X = 0.0;
  double Y = 0.0;
  int Sets = 0;
  double Tag = 0.0;

  [[nodiscard]] double Sum() const { return X + Y; }
  [[nodiscard]] Point Scaled(double By) const {
    return Point{X * By, Y * By, 0};
  }
  [[nodiscard]] std::string Text() const { return "Point"; }
};

template <> struct luna::detail::RegisteredClassTrait<Point> : std::true_type {};

// A class that answers unbound keys itself, through the Index and NewIndex
// operators rather than a hand-written __index.
struct Bag {
  double Known = 1.0;
  double Dynamic = 0.0;
};

// A class whose getter asks to suspend, which a metamethod cannot do.
struct Lazy {
  double Value = 1.0;
};

template <> struct luna::detail::RegisteredClassTrait<Bag> : std::true_type {};
template <> struct luna::detail::RegisteredClassTrait<Lazy> : std::true_type {};

namespace {

int Failures = 0;
int Cases = 0;

void Check(bool Condition, const std::string &What) {
  ++Cases;
  if (Condition)
    return;
  std::printf("  FAIL  %s\n", What.c_str());
  ++Failures;
}

struct Fixture {
  luna::State State;

  Fixture() = default;

  bool Run(const char *Source, std::string *Error = nullptr) {
    const luna::ExecutionResult Result = State.Execute(Source);
    if (Result.IsSuccess())
      return true;
    if (Error) {
      const luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
      *Error = Diagnostic ? Diagnostic->Message() : "";
    }
    return false;
  }
};

// --- functions -------------------------------------------------------------

void TestFunctions() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();

  Registry.Register("Add", [](double A, double B) { return A + B; });
  Registry.Register("Nil", []() { return 42; });
  Registry.Register("Nothing", []() {});
  Registry.Register("Pair",
                    [](double A) { return std::pair<double, double>{A, A * 2}; });
  Registry.Register("Greeting", [](std::string Name) {
    return std::string("hi ") + Name;
  });
  Registry.Register("Maybe", [](std::optional<double> Value) {
    return Value.value_or(-1.0);
  });

  Check(F.Run("assert(Add(2, 3) == 5)"), "two-argument call");
  Check(F.Run("assert(Nil() == 42)"), "zero-argument call");
  Check(F.Run("local a, b = Pair(4); assert(a == 4 and b == 8)"),
        "pair return");
  Check(F.Run("assert(Greeting('bob') == 'hi bob')"), "string round trip");
  Check(F.Run("assert(Maybe() == -1 and Maybe(3) == 3)"), "optional default");

  std::string Error;
  Check(!F.Run("Add('x', 1)", &Error), "a mistyped argument fails");
  Check(Error.find("argument 1") != std::string::npos,
        "the diagnostic names the argument position");
  Check(Error.find("Add") != std::string::npos,
        "the diagnostic names the symbol");
}

// --- suspension ------------------------------------------------------------

void TestSuspension() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();

  double Elapsed = 0.0;
  Registry.Register("wait", [&Elapsed](double Seconds) {
    Elapsed += Seconds;
    luna::Suspend(Seconds);
  });

  Check(F.Run("wait(1.5)\nwait(2.0)"), "a chunk suspends and resumes");
  Check(Elapsed == 3.5, "both suspensions ran");
  Check(F.State.PendingTasks() == 0, "no task is left pending");
}

// --- values ----------------------------------------------------------------

void TestValues() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();

  std::vector<std::string> Seen;

  Registry.Register("collect", [&Seen](luna::ArgumentPack Args) {
    Seen.clear();
    for (std::size_t Index = 0; Index < Args.Size(); ++Index)
      Seen.emplace_back(Args.View(Index).Category() == luna::ValueCategory::Number
                            ? "n"
                            : "s");
  });

  Registry.Register("table3", []() {
    luna::OwnedValue Result = luna::OwnedValue::Table();
    Result.SetField("name", luna::OwnedValue::Text("x"));
    Result.SetField("count", luna::OwnedValue::Number(3));
    Result.Append(luna::OwnedValue::Boolean(true));
    return Result;
  });

  Check(F.Run("collect(1, 'a', 2, 'b')"), "variadic call");
  Check(Seen.size() == 4 && Seen[0] == "n" && Seen[1] == "s",
        "variadic argument categories");

  Check(F.Run("local t = table3()\n"
              "assert(t.name == 'x')\n"
              "assert(t.count == 3)\n"
              "assert(#t == 1 and t[1] == true)"),
        "a built table arrives with fields and items");
}

// --- namespaces, constants, enums -----------------------------------------

void TestNamespaces() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();

  luna::NamespaceBuilder Outer = Registry.RegisterNamespace("Outer");
  Outer.RegisterFunction("Ping", []() { return 7; });
  Outer.RegisterConstant("Answer", 42);
  Outer.RegisterNamespace("Inner").RegisterFunction("Pong", []() { return 8; });

  enum class Level : int { Low = 1, High = 2 };
  luna::EnumBuilder<Level> Levels =
      Outer.RegisterEnum<Level>("Level", luna::StableTypeKey("t.Level"));
  Levels.Value("Low", Level::Low).Value("High", Level::High).Alias("Top", "High");
  Check(Levels.Commit().IsSuccess(), "an enum commits");
  Check(Outer.Commit().IsSuccess(), "a namespace commits");

  Check(F.Run("assert(Outer.Ping() == 7)"), "namespaced function");
  Check(F.Run("assert(Outer.Answer == 42)"), "namespaced constant");
  Check(F.Run("assert(Outer.Inner.Pong() == 8)"), "nested namespace");
  Check(F.Run("assert(Outer.Level.Low == 1 and Outer.Level.Top == 2)"),
        "enum values and aliases");
}

// --- classes ---------------------------------------------------------------

void TestClasses() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();

  luna::ClassBuilder<Point> Points =
      Registry.RegisterClass<Point>("Point", luna::StableTypeKey("t.Point"));

  Points.Factory("new", [](double X, double Y) { return Point{X, Y, 0}; })
      .Method("Sum", &Point::Sum)
      .Method("Scaled", &Point::Scaled)
      .Property("X", [](const Point &Self) { return Self.X; },
                [](Point &Self, double Value) { Self.X = Value; },
                luna::PropertyPolicy::Notify(
                    [](Point &Self, std::string_view) { ++Self.Sets; }))
      .Property("Y", [](const Point &Self) { return Self.Y; })
      .Property("Sets", [](const Point &Self) { return Self.Sets; })
      .Field("Tag", &Point::Tag)
      .Operator(luna::ClassOperator::ToText, &Point::Text)
      .Operator(luna::ClassOperator::Add, [](const Point &A, const Point &B) {
        return Point{A.X + B.X, A.Y + B.Y, 0};
      });

  Check(Points.Commit().IsSuccess(), "a class commits");

  Check(F.Run("local p = Point.new(2, 3)\nassert(p:Sum() == 5)"),
        "factory and method");
  Check(F.Run("local p = Point.new(2, 3)\nassert(p.X == 2 and p.Y == 3)"),
        "readable properties");
  Check(F.Run("local p = Point.new(1, 1)\np.X = 9\nassert(p.X == 9)"),
        "a writable property writes through");
  Check(F.Run("local p = Point.new(1, 2)\nassert(tostring(p) == 'Point')"),
        "a ToText operator becomes __tostring");
  Check(F.Run("local p = Point.new(1, 2)\n"
              "local q = p + Point.new(3, 4)\n"
              "assert(q.X == 4 and q.Y == 6)"),
        "a binary operator");
  Check(F.Run("local p = Point.new(2, 2)\nassert(typeof(p) == 'Point')"),
        "typeof reads the metatable");
  Check(F.Run("local p = Point.new(2, 3)\n"
              "local s = p:Scaled(2)\n"
              "assert(s.X == 4 and s.Y == 6 and typeof(s) == 'Point')"),
        "a method returning a class instance");

  // The write hook is what lets a host drop its notifying wrapper: the
  // binding observes the write without the host re-reading the property.
  Check(F.Run("local p = Point.new(1, 1)\n"
              "p.X = 2\np.X = 3\n"
              "assert(p.Sets == 2)"),
        "a write hook runs once per successful write");

  Check(F.Run("local p = Point.new(0, 0)\n"
              "p.Tag = 4.5\n"
              "assert(p.Tag == 4.5)"),
        "a field reads and writes straight through");

  std::string Error;
  Check(!F.Run("local p = Point.new(1, 1)\np.nonsense = 1", &Error),
        "writing a name with no binding is refused");
  Check(Error.find("not a writable member") != std::string::npos,
        "the refusal says which kind of write was rejected");
}

// --- ownership and lifetime ------------------------------------------------

void TestOwnership() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();

  // A published instance is only readable once its class exists, exactly as a
  // consumer would register game before exposing it.
  luna::ClassBuilder<Point> Points =
      Registry.RegisterClass<Point>("Point", luna::StableTypeKey("t.Point"));
  Points.Factory("new", [](double X, double Y) { return Point{X, Y, 0}; })
      .Property("X", [](const Point &Self) { return Self.X; });
  Check(Points.Commit().IsSuccess(), "the class commits before publication");

  luna::LifetimeHandle Lifetime;
  Point Root{1.0, 2.0, 0};

  Check(Registry
            .RegisterValue("root", [&Root]() { return &Root; },
                           luna::OwnershipPolicy::Borrowed(Lifetime))
            .IsSuccess(),
        "a borrowed instance publishes");

  Check(F.Run("assert(root.X == 1)"), "a borrowed instance is readable");

  Lifetime.Invalidate();
  std::string Error;
  Check(!F.Run("return root.X", &Error), "a dead lifetime refuses access");
  Check(Error.find("no longer available") != std::string::npos,
        "the refusal explains the lifetime");
}

// --- interruption ----------------------------------------------------------

void TestInterrupt() {
  Fixture F;
  F.State.RequestInterrupt("stopped by the host");
  Check(F.State.IsInterruptPending(), "an interrupt is pending");

  const luna::ExecutionResult Result = F.State.Execute("while true do end");
  Check(!Result.IsSuccess(), "a runaway chunk is stopped");
  Check(Result.IsInterrupted(), "the failure is categorised as interrupted");

  F.State.ClearInterrupt();
  Check(!F.State.IsInterruptPending(), "the interrupt clears");
  Check(F.Run("local n = 0\nfor i = 1, 10 do n = n + i end\nassert(n == 55)"),
        "execution resumes after clearing");
}

// --- chunks ----------------------------------------------------------------

void TestChunks() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();

  double Total = 0.0;
  Registry.Register("add", [&Total](double Value) { Total += Value; });

  const luna::Chunk Loaded = F.State.Load("add(3)");
  Check(Loaded.IsValid(), "a chunk compiles");

  Total = 0.0;
  const luna::ExecutionResult First = Loaded.Call();
  const luna::ExecutionResult Second = Loaded.Call();
  Check(First.IsSuccess() && Second.IsSuccess(), "a chunk is re-invocable");
  Check(Total == 6.0, "each invocation ran");
}

// --- reflection ------------------------------------------------------------

void TestReflection() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();

  Registry.Register("free", []() { return 1; });
  luna::NamespaceBuilder Scope = Registry.RegisterNamespace("Scope");
  Scope.RegisterConstant("One", 1);
  Scope.Commit();

  const luna::SymbolRange Symbols = F.State.Symbols();
  Check(Symbols.Size() >= 3, "the palette has entries");

  bool SawFree = false;
  bool SawConstant = false;
  for (const luna::Symbol &Entry : Symbols) {
    if (Entry.Name() == "free" && Entry.Kind() == luna::SymbolKind::Function)
      SawFree = true;
    if (Entry.Name() == "One" && Entry.Kind() == luna::SymbolKind::Constant)
      SawConstant = true;
  }
  Check(SawFree, "a function appears in the palette");
  Check(SawConstant, "a constant appears in the palette");

  const luna::ReflectionSnapshot Snapshot = F.State.Reflection();
  Check(Snapshot.IsValid() && Snapshot.Size() == Symbols.Size(),
        "a snapshot matches the live palette");
}

// --- validation ------------------------------------------------------------

void TestValidation() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();

  luna::NamespaceBuilder Scope = Registry.RegisterNamespace("Dup");
  Scope.RegisterFunction("Twice", []() { return 1; });
  Scope.RegisterFunction("Twice", []() { return 2; });
  const luna::RegistrationResult Committed = Scope.Commit();
  Check(!Committed.IsSuccess(), "a duplicate name in one commit is refused");
  Check(Committed.Diagnostic() != nullptr, "the refusal carries a diagnostic");

  // A rejected commit publishes nothing, so the name stays free.
  Check(F.Run("assert(Dup == nil)"), "a rejected commit leaves the VM alone");
}

// --- custom index / newindex ----------------------------------------------

void TestIndexFallback() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();

  luna::ClassBuilder<Bag> Bags =
      Registry.RegisterClass<Bag>("Bag", luna::StableTypeKey("t.Bag"));

  Bags.Factory("new", []() { return Bag{}; })
      .Property("Known", [](const Bag &Self) { return Self.Known; })
      .Property("Dynamic", [](const Bag &Self) { return Self.Dynamic; })
      .Operator(luna::ClassOperator::Index,
                [](const Bag &Self, luna::OwnedValue Key) {
                  // A container answers numeric keys as well as named ones, so
                  // the key is taken as a value rather than a string.
                  if (Key.IsNumber())
                    return Key.AsNumber() == 1.0 ? Self.Known : 0.0;
                  return Key.IsString() && Key.AsString() == "doubled"
                             ? Self.Known * 2.0
                             : 0.0;
                })
      .Operator(luna::ClassOperator::NewIndex,
                [](Bag &Self, std::string_view Key, double Value) {
                  if (Key == "doubled")
                    Self.Dynamic = Value;
                });

  Check(Bags.Commit().IsSuccess(), "a class with index operators commits");

  std::string Error;
  auto Run = [&](const char *What, const char *Source) {
    Error.clear();
    Check(F.Run(Source, &Error), std::string(What) + ": " + Error);
  };

  Run("an Index operator answers an unbound key",
      "local b = Bag.new()\nassert(b.doubled == 2)");
  Run("a NewIndex operator receives the write",
      "local b = Bag.new()\nb.doubled = 7\nassert(b.Dynamic == 7)");
  Run("a declared property still wins over the fallback",
      "local b = Bag.new()\nassert(b.Known == 1)");
  // A numeric key can never name a member, so it must reach the operator
  // instead of falling through to nil.
  Run("an Index operator answers a numeric key",
      "local b = Bag.new()\nassert(b[1] == 1 and b[2] == 0)");
}

// --- iteration -------------------------------------------------------------

void TestIteration() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();

  // The iterate operator is a cursor: (receiver, control) -> value or nil.
  F.State.Bindings().RegisterFunction("ignore", []() {});
  luna::ClassBuilder<Point> Points =
      Registry.RegisterClass<Point>("Point2", luna::StableTypeKey("t.Point2"));
  Points.Factory("new", [](double X, double Y) { return Point{X, Y, 0}; })
      .Operator(luna::ClassOperator::Iterate,
                [](const Point &Self, double Control) -> luna::OwnedValue {
                  if (Control == 0.0)
                    return luna::OwnedValue::Number(Self.X);
                  if (Control == 1.0)
                    return luna::OwnedValue::Number(Self.Y);
                  return luna::OwnedValue();
                });
  Check(Points.Commit().IsSuccess(), "a class with an iterate operator commits");

  // The cursor returns (index, value) exactly as next() does, so a use site
  // spells the loop with two variables.
  std::string Error;
  const bool Ok = F.Run("local p = Point2.new(3, 4)\n"
                        "local Sum, Count = 0, 0\n"
                        "for Index, Value in p do\n"
                        "  Sum = Sum + Value\n"
                        "  Count = Count + 1\n"
                        "  assert(Index == Count)\n"
                        "end\n"
                        "assert(Sum == 7 and Count == 2)",
                        &Error);
  Check(Ok, "a generic for drives the iterate operator: " + Error);
}

// --- scheduler -------------------------------------------------------------

void TestScheduler() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();

  std::vector<std::string> Log;
  Registry.Register("log", [&Log](std::string What) {
    Log.push_back(std::move(What));
  });
  Registry.Register("wait", [](double Seconds) { luna::Suspend(Seconds); });

  const std::uint64_t Task = F.State.Spawn(
      "log('a')\nwait(1)\nlog('b')\nwait(1)\nlog('c')");

  Check(Task != 0, "a task starts");
  Check(Log.size() == 1 && Log[0] == "a",
        "a task runs to its first suspension");
  Check(F.State.PendingTasks() == 1, "one task is pending");

  F.State.Tick(0.5);
  Check(Log.size() == 1, "half a delay is not enough");
  F.State.Tick(0.6);
  Check(Log.size() == 2 && Log[1] == "b", "the task resumes when due");
  F.State.Tick(1.0);
  Check(Log.size() == 3 && Log[2] == "c", "the task finishes");
  Check(F.State.PendingTasks() == 0, "nothing is left pending");

  const std::uint64_t Cancelled = F.State.Spawn("wait(10)\nlog('never')");
  Check(Cancelled != 0 && F.State.Cancel(Cancelled), "a task cancels");
  F.State.Tick(100.0);
  Check(Log.size() == 3, "a cancelled task never resumes");
  Check(F.State.PendingTasks() == 0, "a cancelled task is gone");
}

// --- enumerators as objects ------------------------------------------------

void TestEnumObjects() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();

  enum class Colour : int { Red = 1, Green = 2 };

  luna::NamespaceBuilder Scope = Registry.RegisterNamespace("Palette");
  luna::EnumBuilder<Colour> Colours =
      Scope.RegisterEnum<Colour>("Colour", luna::StableTypeKey("t.Colour"));
  Colours.Value("Red", Colour::Red).Value("Green", Colour::Green).AsObjects();
  Check(Colours.Commit().IsSuccess(), "an object enum commits");

  Check(F.Run("local R = Palette.Colour.Red\n"
              "assert(R.Name == 'Red' and R.Value == 1)\n"
              "assert(R.EnumName == 'Palette.Colour')\n"
              "assert(R == Palette.Colour.Red)\n"
              "assert(R ~= Palette.Colour.Green)"),
        "enumerators are interned objects that compare by identity");
}

// --- suspension from a metamethod -----------------------------------------

void TestSuspendFromMetamethod() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();

  // docs/PLAN.md risk 1: a suspend inside a metamethod cannot yield, because the
  // VM has no frame to yield through. It must surface as a Lua error, not a
  // crash.
  luna::ClassBuilder<Lazy> Lazies =
      Registry.RegisterClass<Lazy>("Lazy", luna::StableTypeKey("t.Lazy"));
  Lazies.Factory("new", []() { return Lazy{}; })
      .Property("Value", [](const Lazy &Self) {
        luna::Suspend(1.0);
        return Self.Value;
      });
  Check(Lazies.Commit().IsSuccess(), "a suspending getter registers");

  std::string Error;
  Check(!F.Run("local l = Lazy.new()\nreturn l.Value", &Error),
        "reading a suspending getter fails instead of crashing");
  std::printf("  metamethod suspend -> %s\n", Error.c_str());

  // The state is still usable afterwards.
  Check(F.Run("local total = 0\n"
              "for i = 1, 5 do total = total + i end\n"
              "assert(total == 15)"),
        "the state survives the refused yield");
}

} // namespace

int main() {
  std::printf("luna tests\n");

  TestFunctions();
  TestSuspension();
  TestValues();
  TestNamespaces();
  TestClasses();
  TestOwnership();
  TestInterrupt();
  TestChunks();
  TestReflection();
  TestValidation();
  TestIndexFallback();
  TestIteration();
  TestScheduler();
  TestEnumObjects();
  TestSuspendFromMetamethod();

  std::printf("\n%d checks, %d failure%s\n", Cases, Failures,
              Failures == 1 ? "" : "s");
  return Failures == 0 ? 0 : 1;
}
