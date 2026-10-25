#include <luna/luna.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>
#include <stdexcept>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

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

template <>
struct luna::detail::RegisteredClassTrait<Point> : std::true_type {};

struct Bag {
  double Known = 1.0;
  double Dynamic = 0.0;
};

struct Lazy {
  double Value = 1.0;
};

struct Root {
  double Value = 1.0;
  [[nodiscard]] double Doubled() const { return Value * 2.0; }
};

struct Child : Root {
  double Extra = 2.0;
};

struct Side {
  double Pad = 7.0;
};

struct Mixed : Side, Root {};

template <> struct luna::detail::RegisteredClassTrait<Bag> : std::true_type {};
template <> struct luna::detail::RegisteredClassTrait<Lazy> : std::true_type {};
template <> struct luna::detail::RegisteredClassTrait<Root> : std::true_type {};
template <>
struct luna::detail::RegisteredClassTrait<Child> : std::true_type {};
template <> struct luna::detail::RegisteredClassTrait<Side> : std::true_type {};
template <>
struct luna::detail::RegisteredClassTrait<Mixed> : std::true_type {};

// Owns something that cannot be copied.
struct Moved {
  std::unique_ptr<int> Owned;
  double V = 0.0;
  Moved() = default;
  explicit Moved(double In) : Owned(std::make_unique<int>(1)), V(In) {}
  Moved(Moved &&) = default;
  Moved &operator=(Moved &&) = default;
};

struct Holder {
  double W = 1.0;
};

template <>
struct luna::detail::RegisteredClassTrait<Moved> : std::true_type {};
template <>
struct luna::detail::RegisteredClassTrait<Holder> : std::true_type {};

// Opts in through the public name.
struct Plain {
  double V = 4.0;
};
template <> struct luna::RegisteredClassTrait<Plain> : std::true_type {};

// Never registered with any State.
struct Stranger {
  int Value = 3;
};

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

void TestFunctions() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();
  Registry.Register("Add", [](double A, double B) { return A + B; });
  Registry.Register("Nil", []() { return 42; });
  Registry.Register("Nothing", []() {});
  Registry.Register(
      "Pair", [](double A) { return std::pair<double, double>{A, A * 2}; });
  Registry.Register("Greeting",
                    [](std::string Name) { return std::string("hi ") + Name; });
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

void TestValues() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();
  std::vector<std::string> Seen;
  Registry.Register("collect", [&Seen](luna::ArgumentPack Args) {
    Seen.clear();
    for (std::size_t Index = 0; Index < Args.Size(); ++Index)
      Seen.emplace_back(
          Args.View(Index).Category() == luna::ValueCategory::Number ? "n"
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
  Levels.Value("Low", Level::Low)
      .Value("High", Level::High)
      .Alias("Top", "High");
  Check(Levels.Commit().IsSuccess(), "an enum commits");
  Check(Outer.Commit().IsSuccess(), "a namespace commits");
  Check(F.Run("assert(Outer.Ping() == 7)"), "namespaced function");
  Check(F.Run("assert(Outer.Answer == 42)"), "namespaced constant");
  Check(F.Run("assert(Outer.Inner.Pong() == 8)"), "nested namespace");
  Check(F.Run("assert(Outer.Level.Low == 1 and Outer.Level.Top == 2)"),
        "enum values and aliases");
}

void TestClasses() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();
  luna::ClassBuilder<Point> Points =
      Registry.RegisterClass<Point>("Point", luna::StableTypeKey("t.Point"));

  Points.Factory("new", [](double X, double Y) { return Point{X, Y, 0}; })
      .Method("Sum", &Point::Sum)
      .Method("Scaled", &Point::Scaled)
      .Property(
          "X", [](const Point &Self) { return Self.X; },
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

void TestOwnership() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();
  luna::ClassBuilder<Point> Points =
      Registry.RegisterClass<Point>("Point", luna::StableTypeKey("t.Point"));
  Points.Factory("new", [](double X, double Y) { return Point{X, Y, 0}; })
      .Property("X", [](const Point &Self) { return Self.X; });
  Check(Points.Commit().IsSuccess(), "the class commits before publication");
  luna::LifetimeHandle Lifetime;
  Point Borrowed{1.0, 2.0, 0};
  Check(Registry
            .RegisterValue(
                "root", [&Borrowed]() { return &Borrowed; },
                luna::OwnershipPolicy::Borrowed(Lifetime))
            .IsSuccess(),
        "a borrowed instance publishes");

  Check(F.Run("assert(root.X == 1)"), "a borrowed instance is readable");
  Lifetime.Invalidate();
  std::string Error;
  Check(!F.Run("return root.X", &Error), "a dead lifetime refuses access");
  Check(Error.find("no longer available") != std::string::npos,
        "the refusal explains the lifetime");

  luna::ClassBuilder<Root> Roots =
      Registry.RegisterClass<Root>("Root", luna::StableTypeKey("t.Root"));
  Roots.Factory("new", [] { return new Root(); });
  Roots.Field("Value", &Root::Value);
  Check(Roots.Commit().IsSuccess(), "the base class commits");
  luna::ClassBuilder<Child> Children =
      Registry.RegisterClass<Child>("Child", luna::StableTypeKey("t.Child"));
  Children.Factory("new", [] { return new Child(); });
  Children.BaseOf<Root>("Root");
  Check(Children.Commit().IsSuccess(), "the derived class commits");

  luna::LifetimeHandle ChildLifetime;
  Check(Registry
            .RegisterValue(
                "derived", []() -> Child * { return new Child(); },
                luna::OwnershipPolicy::Borrowed(ChildLifetime))
            .IsSuccess(),
        "a borrowed instance of a derived class publishes");
  ChildLifetime.Invalidate();
  Registry.RegisterFunction("TakeRoot",
                            [](Root &Value) { return Value.Value; });
  Check(!F.Run("return TakeRoot(derived)", &Error),
        "a dead lifetime refuses access through a base parameter");
  Check(Error.find("no longer available") != std::string::npos,
        "the base-parameter refusal names the lifetime");

  Registry.RegisterFunction("MakePoint", [] {
    auto Shared = std::make_shared<Point>();
    Shared->X = 7.0;
    return luna::OwnedValue::Instance(Shared);
  });
  Check(F.Run("local p = MakePoint()\n"
              "local junk = {}\n"
              "for i = 1, 20000 do junk[i] = i end\n"
              "junk = nil\n"
              "assert(p.X == 7)"),
        "an OwnedValue instance keeps the object alive for the VM");
}

void TestAliases() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();
  luna::ClassBuilder<Point> Points =
      Registry.RegisterClass<Point>("Point", luna::StableTypeKey("t.Point"));
  Points.Factory("new", [] { return new Point(); });
  Points.Property(
      "X", [](const Point &Self) { return Self.X; },
      [](Point &Self, double Value) { Self.X = Value; });
  Check(Points.Commit().IsSuccess(), "the first name commits");

  luna::ClassBuilder<Point> Again =
      Registry.RegisterClass<Point>("Marker", luna::StableTypeKey("t.Marker"));
  Again.Method("Mark", [](Point &Self) { return Self.X * 2.0; });
  Check(Again.Commit().IsSuccess(), "a second name for the same type commits");
  Check(F.Run("local p = Marker.new()\n"
              "p.X = 4\n"
              "assert(p:Mark() == 8)\n"
              "assert(p:IsA('Marker'))\n"
              "assert(p:IsA('Point'))\n"
              "assert(typeof(p) == 'Point')"),
        "an alias shares members and answers IsA under both names");
  Check(F.Run("local p = Point.new()\nassert(p:IsA('Marker'))"),
        "an instance of the first name also matches the alias");
}

void TestExtension() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();
  luna::ClassBuilder<Bag> Bags =
      Registry.RegisterClass<Bag>("Bag", luna::StableTypeKey("t.Bag"));
  Bags.Factory("new", [] { return new Bag(); });
  Bags.Field("Known", &Bag::Known);
  Bags.Operator(luna::ClassOperator::ToText,
                [](const Bag &) { return std::string("Bag!"); });
  Bags.Operator(luna::ClassOperator::Add, [](const Bag &A, const Bag &B) {
    return Bag{A.Known + B.Known, 0.0};
  });
  Bags.Operator(luna::ClassOperator::Index, [](const Bag &Self,
                                               luna::OwnedValue Key) {
    return Key.IsString() && Key.AsString() == "dyn" ? Self.Known * 10.0 : 0.0;
  });
  Check(Bags.Commit().IsSuccess(), "a class with operators commits");

  luna::ClassBuilder<Bag> More =
      Registry.RegisterClass<Bag>("Bag", luna::StableTypeKey("t.Bag"));
  More.Method("Doubled", [](Bag &Self) { return Self.Known * 2.0; });
  Check(More.Commit().IsSuccess(), "the same class commits again");

  Check(F.Run("local a = Bag.new()\n"
              "a.Known = 3\n"
              "assert(a:Doubled() == 6)\n"
              "assert(tostring(a) == 'Bag!')\n"
              "assert((a + a).Known == 6)\n"
              "assert(a.dyn == 30)"),
        "a later commit extends a class without dropping its operators");
}

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

  const luna::Chunk Broken = F.State.Load("add(", "broken");
  Check(!Broken.IsValid(), "a chunk that does not compile is not valid");
  const luna::ExecutionResult Never = Broken.Call();
  Check(!Never.IsSuccess(), "running a chunk that did not compile fails");
  Check(Never.Diagnostic() &&
            Never.Diagnostic()->Category() == luna::ErrorCategory::Compile,
        "the failure is the compile error, not a missing chunk");

  const luna::Chunk Named = F.State.Load("error('boom')", "named");
  Check(Named.Name() == "named", "a chunk keeps the name it was given");
  const luna::ExecutionResult NamedRun = Named.Call();
  Check(NamedRun.Diagnostic() && NamedRun.Diagnostic()->Chunk() == "named" &&
            NamedRun.Diagnostic()->Line() == 1,
        "errors in a named chunk report that name");
}

void TestStateLifetime() {
  luna::Delegate<void(double)> Handler;
  luna::Chunk Loaded;
  {
    luna::State Scoped;
    Scoped.Bindings().RegisterFunction(
        "hold",
        [&Handler](luna::Delegate<void(double)> Given) { Handler = Given; });
    Check(Scoped.Execute("hold(function(Value) end)").IsSuccess(),
          "a script hands a function to the host");
    Check(Handler.IsBound(), "the captured delegate is bound");
    Loaded = Scoped.Load("return 1");
    Check(Loaded.IsValid(), "a chunk is loaded from the scoped state");
  }

  Check(!Handler.IsBound(), "a delegate dies with its state");
  Check(!Handler.Invoke(1.0).IsSuccess(),
        "invoking a delegate after its state is gone fails cleanly");
  Check(!Loaded.Call().IsSuccess(),
        "calling a chunk after its state is gone fails cleanly");
}

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

void TestValidation() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();
  luna::NamespaceBuilder Scope = Registry.RegisterNamespace("Dup");
  Scope.RegisterFunction("Twice", []() { return 1; });
  Scope.RegisterFunction("Twice", []() { return 2; });
  const luna::RegistrationResult Committed = Scope.Commit();
  Check(!Committed.IsSuccess(), "a duplicate name in one commit is refused");
  Check(Committed.Diagnostic() != nullptr, "the refusal carries a diagnostic");
  Check(F.Run("assert(Dup == nil)"), "a rejected commit leaves the VM alone");

  enum class Shade : int { Dark = 1 };
  luna::EnumBuilder<Shade> Shades =
      Registry.RegisterEnum<Shade>("Shade", luna::StableTypeKey("t.Shade"));
  Shades.Value("Dark", Shade::Dark).Alias("Night", "Missing");
  const luna::RegistrationResult BadAlias = Shades.Commit();
  Check(!BadAlias.IsSuccess(), "an alias to a missing value is refused");
  Check(BadAlias.Diagnostic() &&
            BadAlias.Diagnostic()->Message().find("not one of its values") !=
                std::string::npos,
        "the refusal names the missing alias target");

  const luna::RegistrationResult Empty =
      Registry.RegisterValue("Nothing", [] { return luna::OwnedValue(); });
  Check(!Empty.IsSuccess(), "a value factory that produces nothing is refused");
  Check(Empty.Diagnostic() && Empty.Diagnostic()->Message().find(
                                  "produced no value") != std::string::npos,
        "the refusal explains the empty publication");

  const luna::RegistrationResult Unregistered =
      Registry.RegisterValue("Unregistered", []() -> Point * {
        static Point Sample;
        return &Sample;
      });
  Check(!Unregistered.IsSuccess(),
        "a value of an unregistered class is refused");
  Check(Unregistered.Diagnostic() && Unregistered.Diagnostic()->Message().find(
                                         "not registered") != std::string::npos,
        "the refusal says the class was never registered");

  Registry.RegisterFunction("NullPoint", []() -> Point * { return nullptr; });
  Check(F.Run("assert(NullPoint() == nil)"),
        "a null pointer return is nil, not a silent arity change");
}

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

  Run("an Index operator answers a numeric key",
      "local b = Bag.new()\nassert(b[1] == 1 and b[2] == 0)");
}

void TestIteration() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();
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
  Check(Points.Commit().IsSuccess(),
        "a class with an iterate operator commits");
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

void TestScheduler() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();
  std::vector<std::string> Log;
  Registry.Register(
      "log", [&Log](std::string What) { Log.push_back(std::move(What)); });
  Registry.Register("wait", [](double Seconds) { luna::Suspend(Seconds); });
  const std::uint64_t Task =
      F.State.Spawn("log('a')\nwait(1)\nlog('b')\nwait(1)\nlog('c')");

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

void TestSuspendFromMetamethod() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();
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
  Check(F.Run("local total = 0\n"
              "for i = 1, 5 do total = total + i end\n"
              "assert(total == 15)"),
        "the state survives the refused yield");
}

void TestContainers() {
  Fixture F;
  luna::BindingRegistry Registry = F.State.Bindings();
  Registry.Register("Vec", []() { return std::vector<double>{1.0, 2.0, 3.0}; });
  Registry.Register("Nested", []() {
    return std::vector<std::vector<double>>{{1.0, 2.0}, {3.0}};
  });
  Registry.Register("Map", []() {
    return std::map<std::string, double>{{"a", 1.0}, {"b", 2.0}};
  });
  Registry.Register("MapOfVec", []() {
    return std::map<std::string, std::vector<double>>{{"xs", {1.0, 2.0}}};
  });
  Registry.Register("VecOfPair", []() {
    return std::vector<std::pair<double, double>>{{1.0, 2.0}};
  });
  Registry.Register("Sum", [](std::vector<double> Values) {
    double Total = 0.0;
    for (double V : Values)
      Total += V;
    return Total;
  });
  Registry.Register("Keyed", [](std::map<std::string, double> Table) {
    return Table["a"] + Table["b"];
  });
  Registry.Register(
      "PairIn", [](std::pair<double, double> P) { return P.first + P.second; });
  Registry.Register("TupIn", [](std::tuple<double, double, double> T) {
    return std::get<0>(T) + std::get<1>(T) + std::get<2>(T);
  });
  Registry.Register("OptVec", [](std::optional<std::vector<double>> V) {
    return V ? static_cast<double>(V->size()) : 0.0;
  });
  Check(F.Run("local v = Vec() assert(#v == 3 and v[3] == 3)"),
        "a vector return is an array table");
  Check(F.Run("local n = Nested() assert(#n[1] == 2 and n[2][1] == 3)"),
        "a nested vector is a nested table");
  Check(F.Run("local m = Map() assert(m.a == 1 and m.b == 2)"),
        "a map return is a keyed table");
  Check(F.Run("local m = MapOfVec() assert(#m.xs == 2)"),
        "a map of vectors nests");
  Check(F.Run("local v = VecOfPair() assert(v[1][1] == 1 and v[1][2] == 2)"),
        "a pair nested in a vector becomes a table");
  Check(F.Run("assert(Sum({1,2,3}) == 6)"), "a vector parameter is read");
  Check(F.Run("assert(Keyed({a=2, b=5}) == 7)"), "a map parameter is read");
  Check(F.Run("assert(PairIn({3,4}) == 7)"), "a pair parameter is read");
  Check(F.Run("assert(TupIn({1,2,3}) == 6)"), "a tuple parameter is read");
  Check(F.Run("assert(OptVec({1,2}) == 2 and OptVec() == 0)"),
        "an optional container distinguishes nil from absent");
  Check(!F.Run("return Sum({1, 'x'})"), "a bad element fails conversion");
  Check(!F.Run("return TupIn({1, 2})"), "a short tuple table fails");
  Check(!F.Run("return Keyed('nope')"), "a non-table map argument fails");
  Check(!F.Run("return Sum({1, 'x', 3})"),
        "a bad element fails the whole read");

  Registry.Register("OptVecOut", [] {
    return std::vector<std::optional<double>>{1.0, std::nullopt, 3.0};
  });
  Registry.Register("OptVecIn", [](std::vector<std::optional<double>> Values) {
    return static_cast<double>(Values.size()) +
           (Values.size() > 1 && !Values[1] ? 100.0 : 0.0);
  });
  Check(F.Run("local v = OptVecOut()\n"
              "assert(v[1] == 1 and v[2] == nil and v[3] == 3)"),
        "an absent optional element is a nil slot");
  Check(F.Run("assert(OptVecIn({1, nil, 3}) == 103)"),
        "an optional element is read back with its hole");

  luna::ClassBuilder<Point> Points =
      Registry.RegisterClass<Point>("Point", luna::StableTypeKey("t.Point"));
  Points.Factory("new", [] { return new Point(); });
  Points.Field("Tag", &Point::Tag);
  Check(Points.Commit().IsSuccess(), "a class for container elements commits");
  Registry.Register("TupClass", [](std::tuple<Point, double> Entry) {
    return std::get<0>(Entry).Tag + std::get<1>(Entry);
  });
  Registry.Register("VecClass", [](std::vector<Point> Items) {
    return static_cast<double>(Items.size());
  });
  Check(
      F.Run("local p = Point.new()\np.Tag = 5\nassert(TupClass({p, 2}) == 7)"),
      "a class element in a tuple is read");
  Check(F.Run("local p = Point.new()\nassert(VecClass({p, p}) == 2)"),
        "a class element in a vector is read");
  std::string Error;
  Check(!F.Run("return TupClass({1, 2})", &Error),
        "a non-instance in a class slot fails");
  Check(Error.find("expected Point") != std::string::npos,
        "the refusal names the element class");
}

void TestLimits() {
  {
    luna::StateOptions Options;
    Options.InstructionBudget = 200000;
    luna::State Limited(Options);
    const luna::ExecutionResult Result =
        Limited.Execute("local n = 0 while true do n = n + 1 end");
    Check(!Result.IsSuccess(), "a runaway loop is stopped");
    Check(Result.IsInterrupted(), "the budget reports an interruption");
    Check(Result.Diagnostic() && Result.Diagnostic()->Message().find(
                                     "budget") != std::string::npos,
          "the diagnostic names the budget");

    Check(Limited.Execute("assert(1 + 1 == 2)").IsSuccess(),
          "the state is usable after the budget tripped");
    Check(Limited
              .Execute("local s = 0 for i = 1, 1000 do s = s + i end "
                       "assert(s == 500500)")
              .IsSuccess(),
          "a loop within the budget still runs");
  }

  {
    luna::StateOptions Options;
    Options.InstructionBudget = 200000;
    luna::State Limited(Options);
    luna::NamespaceBuilder Task = Limited.Bindings().RegisterNamespace("task");
    Task.RegisterFunction("wait",
                          [](double Seconds) { luna::Suspend(Seconds); });
    Task.Commit();
    Check(Limited.Execute("for i = 1, 50 do task.wait(0.01) end").IsSuccess(),
          "a yielding chunk is re-armed each slice");
  }

  {
    luna::StateOptions Options;
    Options.MemoryLimitBytes = 4ull * 1024 * 1024;
    luna::State Limited(Options);
    Check(Limited.IsReady(), "a memory-limited state starts");
    Check(Limited.MemoryUsed() > 0, "memory use is always reported");
    Check(!Limited.Execute("local t = {} for i = 1, 1e7 do t[i] = i end")
               .IsSuccess(),
          "an allocation past the limit fails");
    Check(Limited.Execute("assert(true)").IsSuccess(),
          "the state survives the memory limit");
  }

  {
    luna::StateOptions Options;
    Options.StripUnsafeLibs = true;
    luna::State Stripped(Options);
    Check(Stripped
              .Execute("assert(io == nil and debug == nil and "
                       "package == nil)")
              .IsSuccess(),
          "the unsafe libraries are removed");
    Check(Stripped
              .Execute("assert(type(os.clock) == 'function' and "
                       "type(os.time) == 'function')")
              .IsSuccess(),
          "the clock stays, because os holds nothing unsafe");
    Check(Stripped.Execute("assert(type(string.format) == 'function')")
              .IsSuccess(),
          "the pure libraries remain");
  }

  {
    luna::State Sandboxed;
    Check(Sandboxed.Bindings()
              .Register("Seven", []() { return 7.0; })
              .IsSuccess(),
          "a binding registers before sandboxing");
    Sandboxed.Sandbox();
    Check(Sandboxed.Execute("assert(Seven() == 7)").IsSuccess(),
          "a binding is reachable through a sandboxed global");
    Check(!Sandboxed.Execute("_G.Injected = 1").IsSuccess(),
          "a new global is refused once sandboxed");
  }
}

void TestSignals() {
  Fixture F;
  luna::Signal<void(double)> Changed;
  Changed.Attach(F.State);
  double Total = 0.0;
  luna::Connection First = Changed.Connect([&Total](double V) { Total += V; });
  Changed.Connect([&Total](double V) { Total += V * 10.0; });
  luna::FireReport Report = Changed.Fire(2.0);
  Check(Report.Invoked == 2, "every handler fires");
  Check(Report.IsClean(), "a clean fire reports no failures");
  Check(Total == 22.0, "each handler received the argument");
  Check(Changed.HandlerCount() == 2, "the handler count is reported");
  First.Disconnect();
  Check(!First.Connected(), "a disconnected handle says so");
  Changed.Fire(1.0);
  Check(Total == 32.0, "a disconnected handler does not fire again");
  Changed.Clear();
  double LastSeen = 0.0;
  Changed.Connect([](double) { throw std::runtime_error("handler failed"); });
  Changed.Connect([&LastSeen](double V) { LastSeen = V; });
  const luna::FireReport Broken = Changed.Fire(5.0);
  Check(Broken.Invoked == 2, "a throwing handler does not stop the others");
  Check(Broken.Failed == 1, "the failure is counted");
  Check(!Broken.FirstError.empty(), "the failure carries a message");
  Check(LastSeen == 5.0, "the handler after the throwing one still ran");
  Changed.Clear();
  int Hits = 0;
  luna::Connection Late;
  Changed.Connect([&Hits](double) { ++Hits; });
  Late = Changed.Connect([&Late, &Hits](double) {
    ++Hits;
    Late.Disconnect();
  });
  Changed.Fire(0.0);
  Check(Hits == 2, "a handler may disconnect itself while firing");
  Check(Changed.HandlerCount() == 1, "the self-disconnect took effect");
  luna::Signal<void(double)> Scripted;
  Scripted.Attach(F.State);
  luna::RegisterSignal(F.State.Bindings(), "Scripted",
                       luna::StableTypeKey("t.Signal"), Scripted);

  Check(F.Run("fired = 0\n"
              "Scripted:Connect(function(Value) fired = fired + Value end)\n"
              "assert(Scripted:GetHandlerCount() == 1)"),
        "a script connects a handler");
  const luna::FireReport ScriptReport = Scripted.Fire(4.0);
  Check(ScriptReport.Invoked == 1, "a script handler fires");
  Check(F.Run("assert(fired == 4)"), "the script handler saw the argument");
  Scripted.Clear();
  const std::uint64_t Handle = F.State.Spawn("resumed = false\n"
                                             "Scripted:Wait()\n"
                                             "resumed = true");
  Check(Handle != 0, "spawning a waiting chunk returns a handle");
  Check(F.Run("assert(resumed == false)"), "the chunk is parked, not finished");
  Scripted.Fire(1.0);
  F.State.Tick(0.0);
  Check(F.Run("assert(resumed == true)"),
        "firing the signal resumes the waiter");

  luna::Signal<void(std::string)> Text;
  std::string Seen;
  Text.Connect([&Seen](std::string Value) { Seen += "[" + Value + "]"; });
  Text.Connect([&Seen](std::string Value) { Seen += "[" + Value + "]"; });
  Text.Fire("abc");
  Check(Seen == "[abc][abc]",
        "a by-value argument reaches every handler intact");
}
// Overloads resolve by arity then by parameter type, instead of replacing.
void TestOverloads() {
  Fixture F;

  F.State.Bindings().RegisterFunction("ByArity",
                                      [](double Value) { return Value + 1.0; });
  F.State.Bindings().RegisterFunction("ByArity",
                                      [](double A, double B) { return A + B; });

  Check(F.Run("assert(ByArity(1) == 2)"), "a one-argument overload resolves");
  Check(F.Run("assert(ByArity(1, 2) == 3)"),
        "a two-argument overload resolves");

  F.State.Bindings().RegisterFunction(
      "ByType", [](double Value) { return std::string("number"); });
  F.State.Bindings().RegisterFunction(
      "ByType", [](bool Value) { return std::string("boolean"); });

  Check(F.Run("assert(ByType(1) == 'number')"), "a number picks the number");
  Check(F.Run("assert(ByType(true) == 'boolean')"),
        "a boolean picks the boolean");

  F.State.Bindings().RegisterFunction("NoArgs", [] { return 1.0; });
  F.State.Bindings().RegisterFunction("NoArgs", [](double A) { return A; });
  F.State.Bindings().RegisterFunction("NoArgs",
                                      [](double A, double B) { return A * B; });

  Check(F.Run("assert(NoArgs() == 1)"), "a zero-argument overload resolves");
  Check(F.Run("assert(NoArgs(4) == 4)"), "the middle overload resolves");
  Check(F.Run("assert(NoArgs(2, 3) == 6)"), "the last overload resolves");

  F.State.Bindings().RegisterFunction("OptionalTail",
                                      [](double A) { return A; });
  F.State.Bindings().RegisterFunction(
      "OptionalTail",
      [](double A, std::optional<double> B) { return A + B.value_or(10.0); });

  Check(F.Run("assert(OptionalTail(1) == 1)"),
        "the exact arity beats an optional tail");
  Check(F.Run("assert(OptionalTail(1, 2) == 3)"),
        "the optional tail takes the second argument");

  std::string Error;
  Check(!F.Run("ByType(1, 2, 3)", &Error), "no overload match is an error");
  Check(Error.find("no overload of 'ByType'") != std::string::npos,
        "the error names the symbol and the arity");

  luna::ClassBuilder<Point> Points = F.State.Bindings().RegisterClass<Point>(
      "Point", luna::StableTypeKey("t.Point"));
  Points.Factory("new", [] { return new Point(); });
  Points.Field("Tag", &Point::Tag);
  Points.Method("Scale", [](Point &Self) { return Self.Tag; });
  Points.Method("Scale", [](Point &Self, double By) { return Self.Tag * By; });
  Points.Commit();

  Check(F.Run("local p = Point.new()\n"
              "p.Tag = 3\n"
              "assert(p:Scale() == 3)\n"
              "assert(p:Scale(2) == 6)"),
        "a method overload set resolves by arity");

  std::string MethodError;
  Check(!F.Run("local p = Point.new()\nreturn p:Scale('x')", &MethodError),
        "no method overload match is an error");
  Check(MethodError.find("accepts 1 argument") != std::string::npos,
        "a method's receiver is not counted as an argument");

  F.State.Bindings().RegisterFunction(
      "Leak", [](std::map<std::string, double> M) {
        return 1.0 + static_cast<double>(M.size());
      });
  F.State.Bindings().RegisterFunction("Leak", [](luna::ArgumentPack Pack) {
    return static_cast<double>(Pack.Size());
  });
  Check(F.Run("assert(Leak({a='x'}) == 1)"),
        "a rejected table match leaves nothing behind on the stack");

  int ByAritySymbols = 0;
  for (const luna::Symbol &Entry : F.State.Symbols()) {
    if (Entry.QualifiedName() == "ByArity")
      ++ByAritySymbols;
  }
  Check(ByAritySymbols == 1, "an overloaded name appears once in the symbols");

  Check(F.State.Reflection().IsValid(), "reflection survives overloads");
}

void TestDiagnostics() {
  Fixture F;

  const luna::ExecutionResult Failed =
      F.State.Execute("local Value = nil\nreturn Value.Field");
  Check(!Failed.IsSuccess(), "a runtime error fails");
  const luna::ErrorDiagnostic *Report = Failed.Diagnostic();
  Check(Report != nullptr, "the failure carries a diagnostic");
  Check(Report->Category() == luna::ErrorCategory::Runtime,
        "the category is runtime");
  Check(Report->Line() == 2, "the failing line is reported");
  Check(!Report->Chunk().empty(), "the chunk is reported");
  Check(Report->HasLocation(), "the diagnostic knows it has a location");
  Check(Report->Where() == Report->Message(),
        "the location prefix is not repeated");

  const luna::ExecutionResult CompileError = F.State.Execute("return 1 +");
  Check(!CompileError.IsSuccess(), "a compile error fails");
  Check(CompileError.Diagnostic()->Category() == luna::ErrorCategory::Compile,
        "the category is compile");

  F.State.Bindings().RegisterFunction("NeedsNumber",
                                      [](double Value) { return Value; });
  const luna::ExecutionResult ArgumentError =
      F.State.Execute("NeedsNumber('a')");
  Check(!ArgumentError.IsSuccess(), "a bad argument fails");
  Check(ArgumentError.Diagnostic()->Line() == 1,
        "the call site line is reported");
  Check(!ArgumentError.Diagnostic()->Traceback().empty(),
        "a binding error carries a traceback");
  Check(ArgumentError.Diagnostic()->Traceback().find("luna:1") !=
            std::string::npos,
        "the traceback names the chunk and line");
}

void TestDefinitions() {
  Fixture F;

  F.State.Bindings().RegisterFunction("Plain",
                                      [](double Value) { return Value; });
  F.State.Bindings().RegisterFunction(
      "WithOptionals",
      [](double A, std::optional<double> B) { return A + B.value_or(0.0); });
  F.State.Bindings().RegisterFunction("Overloaded", [] { return 1.0; });
  F.State.Bindings().RegisterFunction("Overloaded", [](double A) { return A; });

  luna::ClassBuilder<Point> Points = F.State.Bindings().RegisterClass<Point>(
      "Point", luna::StableTypeKey("t.Point"));
  Points.Factory("new", [] { return new Point(); });
  Points.Field("Tag", &Point::Tag);
  Points.Method("Sum", &Point::Sum);
  Points.Commit();

  luna::NamespaceBuilder Tools =
      F.State.Bindings().RegisterNamespace("Tools.Text");
  Tools.RegisterFunction(
      "Trim", [](std::string_view Text) { return std::string(Text); });
  Tools.Commit();

  const std::string Body = F.State.Definitions();
  Check(!Body.empty(), "definitions are generated");
  Check(Body.find("declare Plain: (any) -> any") != std::string::npos,
        "a function declares its arity");
  Check(Body.find("declare WithOptionals: (any, any?) -> any") !=
            std::string::npos,
        "an optional tail is marked optional");
  Check(Body.find("declare Overloaded: (...any) -> any") != std::string::npos,
        "an overload set is declared variadic");
  Check(Body.find("declare Point: {") != std::string::npos,
        "a class becomes a shape");
  Check(Body.find("    Tag: any,") != std::string::npos, "a field is declared");
  Check(Body.find("    Sum: () -> any,") != std::string::npos,
        "a method excludes the receiver from its arity");
  Check(Body.find("declare Tools: {\n    Text: {\n        Trim: (any) -> "
                  "any,\n    },\n}") != std::string::npos,
        "a namespace nests inside its parent");
  Check(Body.find("declare Tools: any") == std::string::npos,
        "a name with members is not declared as a value");

  // `declare` is definition-file syntax, so the shape is checked by hand.
  int Depth = 0;
  bool Balanced = true;
  for (const char Character : Body) {
    if (Character == '{')
      ++Depth;
    else if (Character == '}')
      Balanced = Balanced && --Depth >= 0;
  }
  Check(Balanced && Depth == 0, "every brace in the body is closed");

  Check(F.State.Definitions() == Body, "generating twice is stable");
}

// A derived class inherits methods, properties and fields, and answers IsA.
void TestBaseClasses() {
  Fixture F;

  luna::ClassBuilder<Root> Roots = F.State.Bindings().RegisterClass<Root>(
      "Root", luna::StableTypeKey("t.Root"));
  Roots.Factory("new", [] { return new Root(); });
  Roots.Field("Value", &Root::Value);
  Roots.Method("Doubled", &Root::Doubled);
  Roots.Operator(luna::ClassOperator::Add, [](const Root &A, const Root &B) {
    return Root{A.Value + B.Value};
  });
  Roots.Commit();

  luna::ClassBuilder<Child> Children = F.State.Bindings().RegisterClass<Child>(
      "Child", luna::StableTypeKey("t.Child"));
  Children.Factory("new", [] { return new Child(); });
  Children.Field("Extra", &Child::Extra);
  Children.BaseOf<Root>("Root");
  Children.Commit();

  Check(F.Run("local c = Child.new()\n"
              "c.Value = 5\n"
              "assert(c.Value == 5)\n"
              "assert(c:Doubled() == 10)\n"
              "assert(c.Extra == 2)\n"
              "assert(c:IsA('Child'))\n"
              "assert(c:IsA('Root'))\n"
              "assert(not c:IsA('Bag'))\n"
              "local r = Root.new()\n"
              "assert(r:IsA('Root'))\n"
              "assert(not r:IsA('Child'))"),
        "a child inherits fields and methods and knows its base");

  Check(F.Run("local c = Child.new()\nc.Value = 3\nc.Extra = 4\n"
              "assert(c.Value == 3 and c.Extra == 4)"),
        "an inherited field and an own field coexist");

  Check(F.Run("local a = Child.new()\n"
              "a.Value = 3\n"
              "local b = Child.new()\n"
              "local c = a + b\n"
              "assert(c.Value == 4)"),
        "an inherited operator applies to the derived class");

  const std::string ChildDefinition = F.State.Definitions();
  const std::size_t ChildBlock = ChildDefinition.find("declare Child:");
  Check(ChildBlock != std::string::npos &&
            ChildDefinition.find("Doubled", ChildBlock) <
                ChildDefinition.find("declare", ChildBlock + 1) &&
            ChildDefinition.find("Extra", ChildBlock) <
                ChildDefinition.find("declare", ChildBlock + 1),
        "a derived definition lists inherited and own members");

  luna::ClassBuilder<Child> Override = F.State.Bindings().RegisterClass<Child>(
      "Child", luna::StableTypeKey("t.Child"));
  Override.Method("Doubled",
                  [](const Child &Self) { return Self.Value * 3.0; });
  Override.BaseOf<Root>("Root");
  Override.Commit();

  Check(F.Run("local c = Child.new()\nc.Value = 2\nassert(c:Doubled() == 6)"),
        "a derived member overrides an inherited one");

  // A base that does not sit at offset zero needs the stored offset applied.
  luna::ClassBuilder<Side> Sides = F.State.Bindings().RegisterClass<Side>(
      "Side", luna::StableTypeKey("t.Side"));
  Sides.Factory("new", [] { return new Side(); });
  Sides.Field("Pad", &Side::Pad);
  Sides.Commit();

  luna::ClassBuilder<Mixed> Mixeds = F.State.Bindings().RegisterClass<Mixed>(
      "Mixed", luna::StableTypeKey("t.Mixed"));
  Mixeds.Factory("new", [] { return new Mixed(); });
  Mixeds.BaseOf<Root>("Root");
  Mixeds.Commit();

  F.State.Bindings().RegisterFunction(
      "TakeRoot", [](Root &Instance) { return Instance.Value; });
  Check(F.Run("local m = Mixed.new()\nm.Value = 9\nassert(m:Doubled() == 18)\n"
              "assert(TakeRoot(m) == 9)"),
        "a non-zero base offset is applied when marshalling");

  luna::ClassBuilder<Bag> Orphans = F.State.Bindings().RegisterClass<Bag>(
      "Orphan", luna::StableTypeKey("t.Orphan"));
  Orphans.BaseOf<Bag>("Missing");
  const luna::RegistrationResult Missing = Orphans.Commit();
  Check(!Missing.IsSuccess(), "an unregistered base is rejected");
  Check(Missing.Diagnostic()->Message().find("not registered before it") !=
            std::string::npos,
        "the rejection names the missing base");

  luna::ClassBuilder<Mixed> Mismatched =
      F.State.Bindings().RegisterClass<Mixed>(
          "Mismatch", luna::StableTypeKey("t.Mismatch"));
  Mismatched.BaseOf<Side>("Root");
  const luna::RegistrationResult Wrong = Mismatched.Commit();
  Check(!Wrong.IsSuccess(), "a base of the wrong type is rejected");
  Check(Wrong.Diagnostic()->Message().find("different type") !=
            std::string::npos,
        "the rejection explains the mismatch");
}

// a handler is held past the task that registered it, and the garbage
// collector runs in between. It used to be invoked on the freed coroutine.
void TestHandlerOutlivesTask() {
  Fixture F;
  luna::Delegate<void(double)> Held;
  double Seen = 0.0;
  F.State.Bindings().Register(
      "hold",
      [&Held](luna::Delegate<void(double)> Handler) { Held = Handler; });
  F.State.Bindings().Register("note", [&Seen](double Value) { Seen += Value; });
  F.State.Bindings().Register("wait",
                              [](double Seconds) { luna::Suspend(Seconds); });

  Check(F.Run("hold(function(v) note(v) end)"),
        "a script hands over a handler");
  for (int Round = 0; Round < 8; ++Round)
    F.Run("local t = {} for i = 1, 5000 do t[i] = {} end collectgarbage()");
  const luna::DelegateCallResult Late = Held.Invoke(2.0);
  Check(Late.IsSuccess() && Seen == 2.0,
        "a handler still runs after its task ended and the collector ran");

  // The registering task is parked, not finished: the handler must not run on
  // top of its suspended coroutine.
  (void)F.State.Spawn(
      "hold(function(v) note(v * 10) end) wait(100) note(1000)");
  for (int Round = 0; Round < 3; ++Round)
    F.Run("collectgarbage()");
  const luna::DelegateCallResult Parked = Held.Invoke(1.0);
  Check(Parked.IsSuccess() && Seen == 12.0,
        "a handler runs while the task that registered it is suspended");
  F.State.Tick(101.0);
  Check(Seen == 1012.0 && F.State.PendingTasks() == 0,
        "the suspended task still resumes and finishes intact");

  Check(F.Run("hold(function() error('boom') end)"),
        "an erroring handler registers");
  const luna::DelegateCallResult Failed = Held.Invoke(0.0);
  Check(!Failed.IsSuccess() &&
            Failed.Diagnostic().find("boom") != std::string::npos,
        "a handler error comes back as a diagnostic");
}

// a callable over a class that is not registered must be refused, and a
// class-typed position must never accept an instance of another class.
void TestUnregisteredClassRefused() {
  Fixture F;
  const luna::RegistrationResult Early = F.State.Bindings().RegisterFunction(
      "early", [](Point &Self) { return Self.X; });
  Check(!Early.IsSuccess(), "a function over an unregistered class is refused");
  Check(Early.Diagnostic() && Early.Diagnostic()->Message().find(
                                  "not registered") != std::string::npos,
        "the refusal says the class is not registered");

  const luna::RegistrationResult Nested = F.State.Bindings().RegisterFunction(
      "early_list", [](std::vector<Point> Items) { return Items.size(); });
  Check(!Nested.IsSuccess(), "a class inside a container is refused too");

  luna::ClassBuilder<Bag> Bags = F.State.Bindings().RegisterClass<Bag>(
      "Bag", luna::StableTypeKey("t.rp.Bag"));
  Bags.Factory("new", [] { return Bag{}; });
  Check(Bags.Commit().IsSuccess(), "another class registers on its own");

  luna::NamespaceBuilder Space = F.State.Bindings().RegisterNamespace("ns");
  Space.RegisterFunction("get", [](Point &Self) { return Self.X; });
  luna::ClassBuilder<Point> Points =
      Space.RegisterClass<Point>("Point", luna::StableTypeKey("t.rp.Point"));
  Points.Factory("new", [] { return Point{}; });
  Check(Points.Commit().IsSuccess(),
        "a function and its class register together in one commit");

  Check(F.Run("assert(ns.get(ns.Point.new()) == 0)"),
        "the right class is accepted");
  std::string Message;
  Check(!F.Run("ns.get(Bag.new())", &Message),
        "an instance of another class is refused");
  Check(Message.find("expected") != std::string::npos,
        "the refusal names the class that was expected");

  Check(F.State.Bindings()
            .RegisterFunction("later", [](Point &Self) { return Self.Y; })
            .IsSuccess(),
        "a function registered after its class is accepted");
}

// starting another task from a native callable must not disturb the one
// that is running.
void TestNestedSchedulerEntry() {
  Fixture F;
  int Marked = 0;
  F.State.Bindings().Register("wait",
                              [](double Seconds) { luna::Suspend(Seconds); });
  F.State.Bindings().Register("mark", [&Marked] { Marked += 1; });
  F.State.Bindings().Register("inner", [](luna::ArgumentPack Args) {
    luna::State *Host = luna::StateFromVM(Args.GetVM());
    (void)Host->Spawn(std::string(Args.View(0).AsString()));
  });
  F.State.Bindings().Register("inner_run", [](luna::ArgumentPack Args) {
    luna::State *Host = luna::StateFromVM(Args.GetVM());
    (void)Host->Execute(std::string(Args.View(0).AsString()));
  });

  const std::uint64_t Handle =
      F.State.Spawn("for i = 1, 40 do inner('local a = 1') end\n"
                    "inner_run('local b = 2')\n"
                    "inner('wait(10)')\n"
                    "wait(1)\n"
                    "mark()");
  Check(Handle != 0, "the outer task starts");
  Check(Marked == 0, "wait() after nested spawns still suspends the task");
  F.State.Tick(0.5);
  Check(Marked == 0, "the task is still waiting half way");
  F.State.Tick(0.6);
  Check(Marked == 1, "the task resumes once its time has passed");
  Check(F.State.PendingTasks() == 1, "the nested sleeper is left running");
}

// a chunk that waits carries on from Tick instead of being dropped.
void TestChunkYields() {
  Fixture F;
  int Marked = 0;
  F.State.Bindings().Register("wait",
                              [](double Seconds) { luna::Suspend(Seconds); });
  F.State.Bindings().Register("mark", [&Marked] { Marked += 1; });

  const luna::Chunk Loaded = F.State.Load("wait(0.1) mark()", "waits");
  const luna::ExecutionResult First = Loaded.Call();
  Check(First.IsSuccess(), "a chunk that waits starts");
  Check(Marked == 0 && F.State.PendingTasks() == 1,
        "the chunk is parked, not finished and not dropped");
  F.State.Tick(0.2);
  Check(Marked == 1, "the rest of the chunk runs after the wait");
  Check(F.State.PendingTasks() == 0, "the finished chunk leaves the task list");

  Check(Loaded.Call().IsSuccess(), "the chunk can be called again");
  F.State.Tick(0.2);
  Check(Marked == 2, "each call carries on independently");

  const luna::Chunk Faulty = F.State.Load("error('after')", "faulty");
  Check(!Faulty.Call().IsSuccess(), "an error in a chunk is still reported");
}

// RequestInterrupt stops a loop with no budget configured.
void TestInterruptWithoutBudget() {
  Fixture F;
  auto Running = std::async(std::launch::async, [&F] {
    return F.State.Execute("local n = 0 while true do n = n + 1 end");
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  F.State.RequestInterrupt("stop it");
  if (Running.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
    Check(false, "RequestInterrupt stops a loop with no budget");
    std::printf("\n%d checks, hung\n", Cases);
    std::fflush(stdout);
    std::_Exit(1);
  }

  const luna::ExecutionResult Result = Running.get();
  Check(Result.IsInterrupted(), "the stopped loop reports an interruption");
  Check(Result.Diagnostic() &&
            Result.Diagnostic()->Message().find("stop it") != std::string::npos,
        "the diagnostic carries the reason the host gave");
  F.State.ClearInterrupt();
  Check(F.Run("local n = 0 for i = 1, 10 do n = n + i end assert(n == 55)"),
        "the state runs again after the interrupt is cleared");

  // A loop that only calls out has call safepoints rather than back edges.
  F.State.Bindings().Register("ping", [] {});
  auto Calling = std::async(std::launch::async, [&F] {
    return F.State.Execute("while true do ping() end");
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  F.State.RequestInterrupt("again");
  if (Calling.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
    Check(false, "RequestInterrupt stops a loop that only calls");
    std::fflush(stdout);
    std::_Exit(1);
  }
  Check(Calling.get().IsInterrupted(), "a second interrupt stops the next run");
}

// registration reports a failure instead of throwing, whatever the cause.
void TestRegistrationNeverThrows() {
  {
    luna::State Sandboxed;
    Sandboxed.Sandbox();
    bool Threw = false;
    luna::RegistrationResult Result;
    try {
      Result = Sandboxed.Bindings().RegisterFunction("late", [] {});
    } catch (...) {
      Threw = true;
    }
    Check(!Threw, "registering after Sandbox() does not throw");
    Check(!Result.IsSuccess(), "registering after Sandbox() is refused");
    Check(Result.Diagnostic() && Result.Diagnostic()->Message().find(
                                     "Sandbox") != std::string::npos,
          "the refusal points at Sandbox()");
    Check(Sandboxed.Execute("assert(late == nil)").IsSuccess(),
          "nothing was published, and the state still runs");
  }

  {
    luna::State Baseline;
    luna::StateOptions Options;
    Options.MemoryLimitBytes = Baseline.MemoryUsed() + 150 * 1024;
    luna::State Tight(Options);
    luna::NamespaceBuilder Big = Tight.Bindings().RegisterNamespace("Big");
    for (int Index = 0; Index < 3000; ++Index)
      Big.RegisterFunction("f" + std::to_string(Index),
                           [Index]() { return Index; });
    bool Threw = false;
    luna::RegistrationResult Result;
    try {
      Result = Big.Commit();
    } catch (...) {
      Threw = true;
    }
    Check(!Threw, "running out of memory while registering does not throw");
    Check(!Result.IsSuccess(), "a registration that cannot fit is refused");
    Check(Tight.Execute("assert(Big == nil)").IsSuccess(),
          "a registration that ran out of memory left nothing behind");
  }
}

// a map is read from a copy of each key.
void TestMapNumericKeys() {
  Fixture F;
  std::map<std::string, double> Seen;
  F.State.Bindings().Register(
      "take",
      [&Seen](std::map<std::string, double> In) { Seen = std::move(In); });
  std::string Error;
  Check(F.Run("take({[1] = 2, [2] = 3, name = 4})", &Error),
        "a table with number keys reads as a string-keyed map: " + Error);
  Check(Seen.size() == 3 && Seen["1"] == 2.0 && Seen["2"] == 3.0 &&
            Seen["name"] == 4.0,
        "every entry arrived, number keys as text");
}

// choosing an overload neither coerces nor alters the arguments.
void TestOverloadsAreStrict() {
  Fixture F;
  std::string Which;
  double Exact = 0.0;
  F.State.Bindings().Register("pick",
                              [&Which](std::string) { Which = "string"; });
  F.State.Bindings().Register("pick", [&Which](double) { Which = "number"; });
  Check(F.Run("pick(5)"), "a number reaches the overload that takes a number");
  Check(Which == "number",
        "a number is not coerced into the earlier string overload");
  Check(F.Run("pick('5')"),
        "a string reaches the overload that takes a string");
  Check(Which == "string",
        "a numeric string is not coerced into the number overload");
  Check(!F.Run("pick(true)"), "an argument no overload takes is refused");

  F.State.Bindings().Register(
      "both", [&Which](std::string, double) { Which = "text,number"; });
  F.State.Bindings().Register("both", [&Which, &Exact](double A, std::string) {
    Which = "number,text";
    Exact = A;
  });
  Check(F.Run("both(0.1 + 0.2, 'x')"),
        "a call that the second overload takes runs");
  Check(Which == "number,text", "the first overload turned it down");
  Check(Exact == 0.1 + 0.2,
        "turning an overload down left the argument exactly as it was");
}

// finished tasks are reclaimed even when the host never calls Tick.
void TestFinishedTasksAreReclaimed() {
  Fixture F;
  for (int Round = 0; Round < 3000; ++Round)
    (void)F.State.Execute("local x = 1");
  Check(F.State.PendingTasks() == 0, "nothing is left running");
  Check(F.State.TaskSlots() < 64,
        "the scheduler does not keep an entry for every finished script");

  (void)F.State.Spawn("local x = 1");
  for (int Round = 0; Round < 200; ++Round)
    (void)F.State.Spawn("local x = 1");
  Check(F.State.TaskSlots() < 64, "spawned tasks are reclaimed too");
  Check(F.Run("assert(true)"), "the state runs after reclaiming");
}

// a failure in a task nobody is waiting on reaches the host.
void TestTaskFailuresAreReported() {
  std::vector<luna::TaskFailure> Seen;
  luna::StateOptions Options;
  Options.OnTaskError = [&Seen](const luna::TaskFailure &Failure) {
    Seen.push_back(Failure);
  };
  luna::State State(Options);
  State.Bindings().Register("wait",
                            [](double Seconds) { luna::Suspend(Seconds); });

  const std::uint64_t Handle = State.Spawn("error('boom')");
  Check(Seen.size() == 1 && Seen[0].Task == Handle,
        "a spawned task that fails on its first run is reported");
  Check(Seen.size() == 1 &&
            Seen[0].Error.Message().find("boom") != std::string::npos,
        "the report carries the script's message");

  Check(State.Spawn("this is not valid") == 0,
        "source that does not compile makes no task");
  Check(Seen.size() == 2 && Seen[1].Task == 0 &&
            Seen[1].Error.Category() == luna::ErrorCategory::Compile,
        "the compile error is reported with task 0");

  (void)State.Spawn("wait(1) error('late')");
  State.Tick(0.5);
  Check(Seen.size() == 2,
        "nothing is reported while the task is still waiting");
  State.Tick(0.6);
  Check(Seen.size() == 3 &&
            Seen[2].Error.Message().find("late") != std::string::npos,
        "a task that fails after Tick resumed it is reported");

  (void)State.Spawn("wait(1) error('pumped')");
  Check(State.Execute("wait(2)").IsSuccess(),
        "Execute finishes its own script");
  Check(Seen.size() == 4 &&
            Seen[3].Error.Message().find("pumped") != std::string::npos,
        "a task that Execute pumped on the way is reported");

  luna::StateOptions Throwing;
  Throwing.OnTaskError = [](const luna::TaskFailure &) {
    throw std::runtime_error("handler failed");
  };
  luna::State Careless(Throwing);
  bool Threw = false;
  try {
    (void)Careless.Spawn("error('x')");
  } catch (...) {
    Threw = true;
  }
  Check(!Threw, "a handler that throws does not break the scheduler");
}

// a reference return aliases the host's object, and a value return moves.
void TestReturnsReferencesAndMoves() {
  Fixture F;
  static Holder Global;
  Global.W = 1.0;
  luna::ClassBuilder<Holder> Holders = F.State.Bindings().RegisterClass<Holder>(
      "Holder", luna::StableTypeKey("t.Holder"));
  Holders.Factory("get", []() -> Holder & { return Global; })
      .Property(
          "W", [](const Holder &Self) { return Self.W; },
          [](Holder &Self, double Value) { Self.W = Value; });
  Check(Holders.Commit().IsSuccess(),
        "a class with a reference-returning factory registers");
  Check(F.Run("local h = Holder.get() h.W = 42"),
        "the script writes through the reference");
  Check(Global.W == 42.0, "the write reached the host's object, not a copy");

  luna::ClassBuilder<Moved> Movers = F.State.Bindings().RegisterClass<Moved>(
      "Moved", luna::StableTypeKey("t.Moved"));
  Movers.Factory("new", [](double V) { return Moved(V); })
      .Property("V", [](const Moved &Self) { return Self.V; });
  Check(Movers.Commit().IsSuccess(), "a class that cannot be copied registers");
  Check(F.Run("assert(Moved.new(7).V == 7)"),
        "a class that cannot be copied can be returned by value");
}

// a commit that fails after it started publishing leaves nothing behind.
void TestFailedCommitLeavesNothing() {
  Fixture F;
  const std::size_t Before = F.State.Symbols().Size();

  {
    luna::NamespaceBuilder Space = F.State.Bindings().RegisterNamespace("rb");
    Space.RegisterFunction("keep", []() { return 1; });
    Space.RegisterValue("broken", []() -> Point * { return nullptr; });
    luna::ClassBuilder<Point> Points =
        Space.RegisterClass<Point>("Point", luna::StableTypeKey("t.rb.Point"));
    Points.Factory("new", [] { return Point{}; })
        .Method("Ghost", [](const Point &) { return 1; });
    const luna::RegistrationResult Failed = Points.Commit();
    Check(!Failed.IsSuccess(), "a commit whose value factory fails is refused");
    Check(F.State.Symbols().Size() == Before, "the symbol list is as it was");
    Check(F.Run("assert(rb == nil)"),
          "the namespace it began creating is gone");
  }

  {
    luna::NamespaceBuilder Space = F.State.Bindings().RegisterNamespace("rb");
    Space.RegisterFunction("keep", []() { return 1; });
    luna::ClassBuilder<Point> Points =
        Space.RegisterClass<Point>("Point", luna::StableTypeKey("t.rb.Point"));
    Points.Factory("new", [] { return Point{}; })
        .Method("Real", [](const Point &) { return 2; });
    Check(Points.Commit().IsSuccess(),
          "the same names register cleanly afterwards");
    Check(F.Run("local p = rb.Point.new()\n"
                "assert(rb.keep() == 1 and p:Real() == 2)\n"
                "assert(p.Ghost == nil)"),
          "nothing from the failed attempt came back");
  }

  // Extending a class that already exists must not keep the failed additions.
  {
    luna::NamespaceBuilder Space = F.State.Bindings().RegisterNamespace("rb");
    Space.RegisterValue("broken", []() -> Point * { return nullptr; });
    luna::ClassBuilder<Point> Points =
        Space.RegisterClass<Point>("Point", luna::StableTypeKey("t.rb.Point"));
    Points.Method("Extra", [](const Point &) { return 3; })
        .Operator(luna::ClassOperator::Length,
                  [](const Point &) { return 9.0; });
    Check(!Points.Commit().IsSuccess(), "a failing extension is refused");
    Check(F.Run("local p = rb.Point.new()\n"
                "assert(p.Extra == nil and p:Real() == 2)\n"
                "assert(pcall(function() return #p end) == false)"),
          "the class is as it was before the failed extension");
  }

  // A namespace cannot be built over something that is not a table.
  Check(F.Run("blocked = 5"), "a global is set");
  {
    luna::NamespaceBuilder Space =
        F.State.Bindings().RegisterNamespace("blocked");
    Space.RegisterFunction("inner", []() { return 1; });
    Space.RegisterFunction("sibling", []() { return 2; });
    const luna::RegistrationResult Refused = Space.Commit();
    Check(!Refused.IsSuccess(),
          "a namespace over a non-table global is refused");
    Check(Refused.Diagnostic() && Refused.Diagnostic()->Message().find(
                                      "not a table") != std::string::npos,
          "the refusal says what is in the way");
    Check(F.Run("assert(blocked == 5)"), "the global it ran into is untouched");
  }

  // One name cannot be two kinds of member.
  luna::ClassBuilder<Holder> Clash = F.State.Bindings().RegisterClass<Holder>(
      "Clash", luna::StableTypeKey("t.Clash"));
  Clash.Method("X", [](const Holder &) { return 1; })
      .Property("X", [](const Holder &Self) { return Self.W; });
  const luna::RegistrationResult Mixed = Clash.Commit();
  Check(!Mixed.IsSuccess(),
        "a method and a property with one name are refused");
  Check(Mixed.Diagnostic() &&
            Mixed.Diagnostic()->Message().find(
                "both a method and a property") != std::string::npos,
        "the refusal names the clash");
}

// one name is one C++ type.
void TestNameBelongsToOneType() {
  Fixture F;
  luna::ClassBuilder<Point> First = F.State.Bindings().RegisterClass<Point>(
      "Shared", luna::StableTypeKey("t.s.A"));
  First.Factory("new", [] { return Point{}; });
  Check(First.Commit().IsSuccess(), "the first class registers");

  luna::ClassBuilder<Bag> Other = F.State.Bindings().RegisterClass<Bag>(
      "Shared", luna::StableTypeKey("t.s.B"));
  Other.Factory("new", [] { return Bag{}; });
  const luna::RegistrationResult Merged = Other.Commit();
  Check(!Merged.IsSuccess(),
        "a different type under a registered name is refused");
  Check(Merged.Diagnostic() && Merged.Diagnostic()->Message().find(
                                   "different type") != std::string::npos,
        "the refusal says the name is taken by another type");

  luna::ClassBuilder<Point> Same = F.State.Bindings().RegisterClass<Point>(
      "Shared", luna::StableTypeKey("t.s.A"));
  Same.Method("More", [](const Point &) { return 4; });
  Check(Same.Commit().IsSuccess(), "the same type may extend its own class");
  Check(F.Run("assert(Shared.new():More() == 4)"), "the extension is there");
}

// a chunk that is dropped gives its function back to the state.
void TestChunksAreReleased() {
  Fixture F;
  (void)F.Run("collectgarbage()");
  const std::size_t Base = F.State.MemoryUsed();
  for (int Index = 0; Index < 3000; ++Index) {
    luna::Chunk Loaded = F.State.Load(
        "local a = " + std::to_string(Index) + " return a", "dropped");
    (void)Loaded;
  }
  (void)F.Run("collectgarbage()");
  (void)F.Run("collectgarbage()");
  Check(F.State.MemoryUsed() < Base + 128 * 1024,
        "3000 dropped chunks do not stay in the state");
}

// what a script can reach of the bindings, it cannot rewrite.
void TestBindingsAreLocked() {
  Fixture F;
  static Holder Subject;
  Subject.W = 1.0;
  luna::LifetimeHandle Life;

  luna::ClassBuilder<Holder> Holders = F.State.Bindings().RegisterClass<Holder>(
      "Holder", luna::StableTypeKey("t.lock.Holder"));
  Holders.Factory("pick", []() -> Holder * { return &Subject; })
      .BorrowedFrom(Life)
      .Property(
          "W", [](const Holder &Self) { return Self.W; },
          [](Holder &Self, double Value) { Self.W = Value; });
  Check(Holders.Commit().IsSuccess(), "a class registers");

  Check(
      F.Run(
          "local h = Holder.pick() assert(type(getmetatable(h)) == 'string')"),
      "getmetatable hands back a lock message, not the metatable");
  Check(!F.Run("Holder.pick = nil"), "a script cannot replace a static member");
  Check(!F.Run("Holder.extra = 1"), "a script cannot add to a class table");

  luna::ClassBuilder<Holder> More = F.State.Bindings().RegisterClass<Holder>(
      "Holder", luna::StableTypeKey("t.lock.Holder"));
  More.Method("Twice", [](const Holder &Self) { return Self.W * 2.0; });
  Check(More.Commit().IsSuccess(), "the host can still extend a locked class");
  Check(F.Run("assert(Holder.pick():Twice() == 2)"), "the extension works");
  Check(!F.Run("Holder.pick = nil"),
        "the class is locked again after the commit");

  enum class Hue : int { Red = 1, Green = 2 };
  luna::EnumBuilder<Hue> Hues = F.State.Bindings().RegisterEnum<Hue>(
      "Hue", luna::StableTypeKey("t.lock.Hue"));
  Hues.Value("Red", Hue::Red).Value("Green", Hue::Green).AsObjects();
  Check(Hues.Commit().IsSuccess(), "an enum registers");
  Check(!F.Run("Hue.Red = 5"), "a script cannot replace an enumerator");
  Check(!F.Run("Hue.Red.Value = 9"), "a script cannot edit an enumerator");
  Check(F.Run("assert(Hue.Red.Value == 1)"), "the enumerator is unchanged");

  // Pointers a bound function returns are tied to the class's handle.
  Check(F.Run("kept = Holder.pick() assert(kept.W == 1)"),
        "an instance is returned");
  Life.Invalidate();
  std::string Message;
  Check(!F.Run("return kept.W", &Message),
        "an instance returned by pointer stops working with its handle");
  Check(Message.find("no longer available") != std::string::npos,
        "the failure says the instance is gone");
}

// A request to wait that fails must not linger, and waiting where nothing can
// yield is refused.
void TestSuspensionEdges() {
  Fixture F;
  int Done = 0;
  F.State.Bindings().Register("bad", [] {
    luna::Suspend(5.0);
    throw std::runtime_error("refused");
  });
  F.State.Bindings().Register("ok", [] {});
  F.State.Bindings().Register("done", [&Done] { ++Done; });
  F.State.Bindings().Register("wait",
                              [](double Seconds) { luna::Suspend(Seconds); });
  (void)F.State.Spawn("pcall(bad) ok() done()");
  Check(Done == 1, "a call that asked to wait and then failed does not make "
                   "the next one wait");

  luna::Delegate<void()> Held;
  F.State.Bindings().Register("hold",
                              [&Held](luna::Delegate<void()> H) { Held = H; });
  Check(F.Run("hold(function() wait(1) end)"),
        "a handler that waits registers");
  const luna::DelegateCallResult Waited = Held.Invoke();
  Check(!Waited.IsSuccess() &&
            Waited.Diagnostic().find("cannot wait") != std::string::npos,
        "wait() in a handler the host calls is refused, not silently skipped");
}

void TestSignalWaitValues() {
  Fixture F;
  luna::Signal<void(double, std::string)> Both;
  Both.Attach(F.State);
  luna::RegisterSignal(F.State.Bindings(), "Both",
                       luna::StableTypeKey("t.sw.Both"), Both);

  (void)F.State.Spawn("local n, t = Both:Wait() gotNum = n gotText = t");
  Both.Fire(7.0, "hi");
  F.State.Tick(0.0);
  Check(F.Run("assert(gotNum == 7 and gotText == 'hi')"),
        "Wait returns the values the signal was fired with");

  const luna::ExecutionResult Parked = F.State.Execute("Both:Wait()");
  Check(Parked.IsSuccess() && Parked.IsPending(),
        "Execute says its script is parked, not finished");
  Check(!F.State.Execute("local x = 1").IsPending(),
        "a finished script is not pending");
  Both.Fire(1.0, "x");
  F.State.Tick(0.0);
}

void TestStrictMembers() {
  luna::StateOptions Strict;
  Strict.StrictMembers = true;
  luna::State Careful(Strict);
  luna::State Lenient;
  for (luna::State *Target : {&Careful, &Lenient}) {
    luna::ClassBuilder<Holder> Holders =
        Target->Bindings().RegisterClass<Holder>(
            "Holder", luna::StableTypeKey("t.sm.Holder"));
    Holders.Factory("new", [] { return Holder{}; })
        .Property("W", [](const Holder &Self) { return Self.W; });
    Check(Holders.Commit().IsSuccess(), "a class registers");
  }
  Check(Lenient.Execute("assert(Holder.new().Wdith == nil)").IsSuccess(),
        "an unknown member reads as nil by default");
  std::string Message;
  const luna::ExecutionResult Result =
      Careful.Execute("return Holder.new().Wdith");
  Check(!Result.IsSuccess(),
        "an unknown member is an error when members are strict");
  Check(Result.Diagnostic() &&
            Result.Diagnostic()->Message().find("not a valid member of") !=
                std::string::npos,
        "the error names the member and the class");
  Check(Careful.Execute("assert(Holder.new().W == 1)").IsSuccess(),
        "a real member still reads");
}

void TestValuesRoundTrip() {
  Fixture F;
  luna::OwnedValue Seen;
  F.State.Bindings().Register(
      "inspect", [&Seen](luna::ArgumentPack Args) { Seen = Args.At(0); });
  Check(F.Run("inspect({10, nil, 30, extra = 'e', [7] = 'seven'})"),
        "a table is passed");
  const luna::OwnedTable *Table = Seen.AsTable();
  Check(Table && Table->Items.size() == 3 &&
            Table->Items[0].AsNumber() == 10.0 && Table->Items[1].IsNil() &&
            Table->Items[2].AsNumber() == 30.0,
        "a sequence keeps its positions, a hole included");
  bool Extra = false;
  bool Seven = false;
  if (Table) {
    for (const luna::OwnedTable::Entry &Field : Table->Fields) {
      Extra = Extra || (Field.Key == "extra" && Field.Value.AsString() == "e");
      Seven = Seven || (Field.Key == "7" && Field.Value.AsString() == "seven");
    }
  }
  Check(Extra && Seven,
        "a named field and a number outside the sequence both survive");

  luna::ClassBuilder<Holder> Holders = F.State.Bindings().RegisterClass<Holder>(
      "Holder", luna::StableTypeKey("t.rt.Holder"));
  Holders.Factory("make", [] { return std::make_shared<Holder>(); });
  Check(Holders.Commit().IsSuccess(),
        "a class with a shared factory registers");
  Check(F.Run("inspect(Holder.make())"), "an instance is passed");
  Check(Seen.IsInstance() && Seen.AsInstance()->Object != nullptr,
        "a shared instance comes back as an instance, not nil");

  const luna::Chunk Loaded = F.State.Load("return ...", "args");
  luna::ValuePack Arguments;
  Arguments.Append(luna::OwnedValue::Instance<Stranger>(Stranger{}));
  bool Threw = false;
  luna::ExecutionResult Result;
  try {
    Result = Loaded.Call(Arguments);
  } catch (...) {
    Threw = true;
  }
  Check(!Threw, "an argument that cannot be passed does not throw");
  Check(!Result.IsSuccess() && Result.Diagnostic() &&
            Result.Diagnostic()->Message().find("not registered") !=
                std::string::npos,
        "it is refused, saying the class is not registered");
}

void TestNumbersAtTheEdge() {
  Fixture F;
  F.State.Bindings().Register("Signed", [](std::int64_t) { return 1.0; });
  F.State.Bindings().Register("Unsigned", [](std::uint64_t) { return 1.0; });
  Check(F.Run("Signed(2^53)"), "2^53 is a 64-bit integer");
  Check(F.Run("Signed(-(2^63))"), "the lowest 64-bit integer is accepted");
  Check(!F.Run("Signed(2^63)"), "2^63 is one past the highest and is refused");
  Check(!F.Run("Signed(0/0)"), "nan is refused");
  Check(F.Run("Unsigned(2^63)"), "2^63 fits an unsigned 64-bit integer");
  Check(!F.Run("Unsigned(2^64)"), "2^64 does not");
  Check(!F.Run("Unsigned(-1)"), "a negative number does not");
}

void TestStateMovesAndGoes() {
  luna::State A;
  luna::BindingRegistry Registry = A.Bindings();
  luna::Signal<void()> Sig;
  Sig.Attach(A);
  luna::RegisterSignal(Registry, "Moved", luna::StableTypeKey("t.mv.Sig"), Sig);

  luna::State B = std::move(A);
  Check(Registry.RegisterFunction("afterMove", [] { return 5.0; }).IsSuccess(),
        "a registry taken before a move still registers");
  Check(B.Execute("assert(afterMove() == 5)").IsSuccess(),
        "the moved state has it");
  Check(Registry.Owner() == &B,
        "the registry follows the state it was taken from");
  (void)B.Spawn("Moved:Wait() resumedAfterMove = true");
  Sig.Fire();
  B.Tick(0.0);
  Check(B.Execute("assert(resumedAfterMove == true)").IsSuccess(),
        "a signal attached before the move still wakes its waiters");

  luna::BindingRegistry Stale;
  luna::Signal<void()> Orphan;
  {
    luna::State Temporary;
    Stale = Temporary.Bindings();
    Orphan.Attach(Temporary);
  }
  const luna::RegistrationResult Gone = Stale.RegisterFunction("x", [] {});
  Check(!Gone.IsSuccess(), "a registry whose state is gone refuses");
  Check(Stale.Owner() == nullptr, "and no longer names an owner");
  Orphan.Fire();
  Check(Orphan.AttachedState() == nullptr,
        "a signal whose state is gone fires safely");
}

void TestSymbolsAreSafeToRead() {
  Fixture F;
  std::atomic<bool> Stop{false};
  std::atomic<std::size_t> Largest{0};
  std::thread Reader([&] {
    while (!Stop.load()) {
      const std::size_t Seen = F.State.Symbols().Size();
      std::size_t Known = Largest.load();
      while (Seen > Known && !Largest.compare_exchange_weak(Known, Seen)) {
      }
    }
  });
  luna::NamespaceBuilder Many = F.State.Bindings().RegisterNamespace("Many");
  for (int Index = 0; Index < 600; ++Index)
    Many.RegisterFunction("f" + std::to_string(Index),
                          [Index] { return Index; });
  Check(Many.Commit().IsSuccess(),
        "many functions register while another thread reads");
  Stop.store(true);
  Reader.join();
  Check(F.State.Symbols().Size() >= 600, "every symbol is listed");
}

void TestDiagnosticsAndDefinitions() {
  Fixture F;
  const luna::ExecutionResult Failed =
      F.State.Execute("local function f()\n  error('inside')\nend\nf()");
  Check(!Failed.IsSuccess() && Failed.Diagnostic() &&
            Failed.Diagnostic()->Traceback().find("luna:2") !=
                std::string::npos,
        "a script's own error carries a traceback naming the line it was "
        "raised on");

  luna::NamespaceBuilder Words = F.State.Bindings().RegisterNamespace("Words");
  Words.RegisterFunction("end", [](double Value) { return Value; });
  Check(Words.Commit().IsSuccess(),
        "a function named like a keyword registers");
  F.State.Bindings().RegisterFunction("repeat",
                                      [](double Value) { return Value; });
  const std::string Body = F.State.Definitions();
  Check(Body.find("[\"end\"]: (any) -> any") != std::string::npos,
        "a keyword member is quoted in a table type");
  Check(Body.find("declare repeat") == std::string::npos,
        "a global named like a keyword is not declared");
}

// The task entry points report running out of memory instead of throwing.
void TestOutOfMemoryAtTaskCreation() {
  luna::State Baseline;
  luna::StateOptions Options;
  Options.MemoryLimitBytes = Baseline.MemoryUsed() + 96 * 1024;
  luna::State Tight(Options);
  (void)Tight.Execute("keep = {} for i = 1, 1e6 do keep[i] = {} end");

  bool Threw = false;
  int Refused = 0;
  try {
    for (int Round = 0; Round < 30; ++Round) {
      if (!Tight.Execute("local x = 1").IsSuccess())
        ++Refused;
      if (Tight.Spawn("local x = 1") == 0)
        ++Refused;
      luna::Chunk Loaded = Tight.Load("local x = 1", "tight");
      if (!Loaded.IsValid())
        ++Refused;
      else if (!Loaded.Call().IsSuccess())
        ++Refused;
    }
  } catch (...) {
    Threw = true;
  }
  Check(!Threw, "Execute, Spawn and Load do not throw when memory runs out");
  Check(Refused > 0, "running out of memory is reported to the caller");
}

void TestTaskLibrary() {
  std::vector<luna::TaskFailure> Failures;
  luna::StateOptions Options;
  Options.OnTaskError = [&Failures](const luna::TaskFailure &Failure) {
    Failures.push_back(Failure);
  };
  luna::State State(Options);
  luna::BindingRegistry Registry = State.Bindings();
  Check(luna::RegisterTaskLibrary(Registry).IsSuccess(),
        "the task library registers");

  // A coroutine the script made keeps waiting after its caller moved on.
  (void)State.Spawn("local co = coroutine.wrap(function()\n"
                    "  task.wait(1)\n"
                    "  inner = true\n"
                    "end)\n"
                    "co()\n"
                    "outer = true");
  Check(State.Execute("assert(outer == true and inner == nil)").IsSuccess(),
        "the caller carries on past a coroutine that waits");
  State.Tick(1.1);
  Check(State.Execute("assert(inner == true)").IsSuccess(),
        "the scheduler resumes a coroutine the script made");

  (void)State.Spawn("elapsed = task.wait(0.5)");
  State.Tick(0.6);
  Check(State.Execute("assert(elapsed >= 0.5 and elapsed < 0.7)").IsSuccess(),
        "task.wait returns the seconds that passed");

  Check(State
            .Execute("task.spawn(function(a, b) sum = a + b end, 2, 3)\n"
                     "assert(sum == 5)")
            .IsSuccess(),
        "task.spawn runs now and passes its arguments");
  Check(State.Execute("task.spawn(function() task.wait(1) late = true end)")
                .IsSuccess() &&
            State.PendingTasks() == 1,
        "a spawned function can wait");
  State.Tick(1.1);
  Check(State.Execute("assert(late == true)").IsSuccess(),
        "it finishes when its time comes");

  Check(State
            .Execute("task.defer(function() deferred = true end)\n"
                     "assert(deferred == nil)")
            .IsSuccess(),
        "task.defer does not run at once");
  State.Tick(0.0);
  Check(State.Execute("assert(deferred == true)").IsSuccess(),
        "it runs on the next tick");

  Check(
      State
          .Execute("task.delay(2, function() delayed = true end)\n"
                   "local h = task.delay(1, function() cancelled = true end)\n"
                   "task.cancel(h)")
          .IsSuccess(),
      "task.delay and task.cancel run");
  State.Tick(1.5);
  Check(
      State.Execute("assert(delayed == nil and cancelled == nil)").IsSuccess(),
      "nothing fires early, and a cancelled task never fires");
  State.Tick(1.0);
  Check(
      State.Execute("assert(delayed == true and cancelled == nil)").IsSuccess(),
      "the delay fires at its time");

  Failures.clear();
  Check(State.Execute("task.spawn(function() error('boom') end)").IsSuccess(),
        "a spawned function that fails does not fail its spawner");
  Check(Failures.size() == 1 &&
            Failures[0].Error.Message().find("boom") != std::string::npos,
        "its failure goes to OnTaskError");
  Check(!State.Execute("task.spawn(5)").IsSuccess(),
        "task.spawn refuses something that is not a function");
  Check(State.Execute("wait(0.1) aliased = true").IsSuccess(),
        "the global wait is there too");

  luna::Delegate<void(double)> Held;
  Registry.Register("hold", [&Held](luna::Delegate<void(double)> Handler) {
    Held = Handler;
  });
  Check(State.Execute("hold(function(v) task.wait(1) heldGot = v end)")
            .IsSuccess(),
        "a handler that waits registers");
  Check(Held.Spawn(7.0) != 0, "a delegate can start its function as a task");
  State.Tick(1.1);
  Check(State.Execute("assert(heldGot == 7)").IsSuccess(),
        "that task waited and finished, with its argument");
  (void)Held.SpawnAfter(2.0, 8.0);
  State.Tick(1.0);
  Check(State.Execute("assert(heldGot == 7)").IsSuccess(),
        "a delayed spawn waits");
  State.Tick(1.1);
  Check(State.Execute("assert(heldGot == 7)").IsSuccess(),
        "and then runs its function, which waits one more second");
  State.Tick(1.1);
  Check(State.Execute("assert(heldGot == 8)").IsSuccess(), "which finishes");
}

void TestWakeFromAnotherThread() {
  Fixture F;
  static int Key = 0;
  F.State.Bindings().Register("park", [] { luna::WaitForSignal(&Key); });
  (void)F.State.Spawn("got = park()");
  Check(F.State.HasWaiters(&Key), "a script is parked");
  Check(F.Run("assert(got == nil)"), "it is still parked");

  std::thread Worker([&F] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    F.State.PostWake(&Key, [](void *VM) {
      lua_pushnumber(static_cast<lua_State *>(VM), 5.0);
      return 1;
    });
  });
  Worker.join();
  F.State.Tick(0.0);
  Check(F.Run("assert(got == 5)"),
        "a wake posted from another thread resumes the script with its values");
}

void TestChunkReturnedToScript() {
  Fixture F;
  luna::State *Host = &F.State;
  Check(luna::RegisterTaskLibrary(F.State.Bindings()).IsSuccess(),
        "the task library registers");
  F.State.Bindings().Register("load", [Host](std::string Source) {
    luna::Chunk Loaded = Host->Load(Source, "loaded");
    if (!Loaded.IsValid())
      throw std::runtime_error(Loaded.Diagnostic()->Message());
    return Loaded;
  });

  Check(F.Run("local f = load('return 1 + 1') assert(type(f) == 'function')\n"
              "assert(f() == 2)"),
        "a returned chunk is a function the script can call");
  Check(F.Run("local f = load('local a, b = ... return a * b') assert(f(6, 7) "
              "== 42)"),
        "it takes arguments and returns values");
  std::string Message;
  Check(!F.Run("load('x = = 1')", &Message),
        "a chunk that does not compile throws");
  Check(Message.find("loaded") != std::string::npos,
        "the error names the chunk and carries the compile error");

  (void)F.State.Spawn("load('task.wait(0.5) loadedDone = true')()");
  Check(F.Run("assert(loadedDone == nil)"),
        "the loaded function waits in the caller's task");
  F.State.Tick(0.6);
  Check(F.Run("assert(loadedDone == true)"), "and finishes after the wait");

  luna::Chunk Broken = F.State.Load("x = = 1", "broken");
  Check(!Broken.IsValid() && Broken.Diagnostic() &&
            Broken.Diagnostic()->Category() == luna::ErrorCategory::Compile,
        "a chunk that did not compile reports its diagnostic");
  Check(F.State.Load("return 1", "fine").Diagnostic() == nullptr,
        "a chunk that compiled has none");
}

void TestEnumeratorTypes() {
  Fixture F;
  enum class Hue : int { Red = 1, Green = 2 };
  luna::EnumBuilder<Hue> Hues = F.State.Bindings().RegisterEnum<Hue>(
      "Hue", luna::StableTypeKey("t.et.Hue"));
  Hues.Value("Red", Hue::Red).Value("Green", Hue::Green).AsObjects();
  Check(Hues.Commit().IsSuccess(), "an enum registers");
  Check(F.Run("assert(typeof(Hue.Red) == 'EnumItem')"),
        "an enumerator reports EnumItem");
  Check(F.Run("assert(tostring(Hue.Green) == 'Hue.Green')"),
        "an enumerator prints as its enum and name");
  Check(F.Run("assert(Hue.Red == Hue.Red and Hue.Red ~= Hue.Green)"),
        "enumerators still compare by identity");
  Check(F.Run("assert(type(getmetatable(Hue.Red)) == 'string')"),
        "their metatable is locked");
  Check(!F.Run("Hue.Red.Value = 3"), "and they are still read-only");
}

void TestPublicClassTrait() {
  Fixture F;
  luna::ClassBuilder<Plain> Plains = F.State.Bindings().RegisterClass<Plain>(
      "Plain", luna::StableTypeKey("t.pt.Plain"));
  Plains.Factory("new", [] { return Plain{}; })
      .Property("V", [](const Plain &Self) { return Self.V; });
  Check(Plains.Commit().IsSuccess(),
        "a class that opted in through luna::RegisteredClassTrait registers");
  F.State.Bindings().Register("take", [](const Plain &Self) { return Self.V; });
  Check(F.Run("assert(take(Plain.new()) == 4 and Plain.new().V == 4)"),
        "it works as a parameter and a return");
}

void TestScriptSignals() {
  std::vector<luna::TaskFailure> Failures;
  luna::StateOptions Options;
  Options.OnTaskError = [&Failures](const luna::TaskFailure &Failure) {
    Failures.push_back(Failure);
  };
  luna::State State(Options);
  luna::BindingRegistry Registry = State.Bindings();
  Check(luna::RegisterTaskLibrary(Registry).IsSuccess(),
        "the task library registers");
  luna::ScriptSignalNames Names;
  Names.SignalType = "Event";
  Check(luna::RegisterScriptSignals(Registry, Names).IsSuccess(),
        "the script signal classes register");

  luna::ScriptSignal Changed("Changed");
  Check(Registry.RegisterValue("Changed", [Changed] { return Changed; })
            .IsSuccess(),
        "a signal is handed to scripts by value");

  Check(State
            .Execute("got = {}\n"
                     "conn = Changed:Connect(function(a, b)\n"
                     "  got[#got + 1] = a .. b\n"
                     "end)\n"
                     "assert(conn.Connected == true)\n"
                     "assert(typeof(conn) == 'Connection')\n"
                     "assert(typeof(Changed) == 'Event')\n"
                     "assert(Changed.ConnectionCount == 1 and Changed.Name == "
                     "'Changed')")
            .IsSuccess(),
        "a script connects and gets a connection object");
  Changed.Fire("x", "y");
  Check(State.Execute("assert(got[1] == 'xy')").IsSuccess(),
        "a handler receives what the host fired");
  Check(State.Execute("conn:Disconnect() assert(conn.Connected == false)")
            .IsSuccess(),
        "a connection disconnects");
  Changed.Fire("p", "q");
  Check(State.Execute("assert(#got == 1 and Changed.ConnectionCount == 0)")
            .IsSuccess(),
        "a disconnected handler does not run again");

  Check(State
            .Execute("once = 0\n"
                     "Changed:Once(function() once = once + 1 end)")
            .IsSuccess(),
        "a script connects with Once");
  Changed.Fire();
  Changed.Fire();
  Check(State.Execute("assert(once == 1)").IsSuccess(),
        "a Once handler runs once");

  (void)State.Spawn("local a, b = Changed:Wait() waited = a + b");
  Changed.Fire(3.0, 4.0);
  State.Tick(0.0);
  Check(State.Execute("assert(waited == 7)").IsSuccess(),
        "Wait returns the values fired, with no Attach call");

  Check(State
            .Execute("Changed:Connect(function(v)\n"
                     "  task.wait(1)\n"
                     "  late = v\n"
                     "end)")
            .IsSuccess(),
        "a handler that waits connects");
  Changed.Fire(9.0);
  Check(State.Execute("assert(late == nil)").IsSuccess(), "it is parked");
  State.Tick(1.1);
  Check(State.Execute("assert(late == 9)").IsSuccess(),
        "a signal handler runs as a task, so it can wait");

  int HostCalls = 0;
  const luna::ScriptConnection Hosted =
      Changed.Connect([&HostCalls](const luna::ValuePack &Payload) {
        HostCalls += static_cast<int>(Payload.Size());
      });
  Check(State.Execute("Changed:Fire(1, 2, 3)").IsSuccess() && HostCalls == 3,
        "a script can fire a signal, and a host handler hears it");
  Hosted.Disconnect();
  Check(!Hosted.Connected(), "a host connection disconnects");

  Check(
      State
          .Execute(
              "local s = Signal.new('mine')\n"
              "local n = 0\n"
              "s:Connect(function() n = n + 1 end)\n"
              "s:Fire() s:Fire()\n"
              "assert(n == 2 and s.Name == 'mine' and s.ConnectionCount == 1)\n"
              "s:DisconnectAll()\n"
              "assert(s.ConnectionCount == 0)\n"
              "assert(tostring(s) == 'Event mine')")
          .IsSuccess(),
      "a script makes its own signal");
  Check(State.Execute("assert(Changed == Changed and Changed ~= Signal.new())")
            .IsSuccess(),
        "signals compare by identity");

  Failures.clear();
  Check(State
            .Execute("Signal.new():Connect(function() error('bad') end)\n"
                     "local s = Signal.new() s:Connect(function() error('bad') "
                     "end) s:Fire()")
            .IsSuccess(),
        "a handler that fails does not fail the fire");
  Check(Failures.size() == 1 &&
            Failures[0].Error.Message().find("bad") != std::string::npos,
        "its failure goes to OnTaskError");

  luna::Signal<void(double)> Typed;
  Typed.Attach(State);
  luna::RegisterSignal(Registry, "Typed", luna::StableTypeKey("t.ss.Typed"),
                       Typed);
  Check(
      State.Execute("Typed:Connect(function(v) task.wait(1) typedLate = v end)")
          .IsSuccess(),
      "a typed signal connects a handler that waits");
  Typed.Fire(2.0);
  State.Tick(1.1);
  Check(State.Execute("assert(typedLate == 2)").IsSuccess(),
        "a typed signal's script handler runs as a task too");
}

void TestInspectingValues() {
  Fixture F;
  luna::OwnedValue Seen;
  F.State.Bindings().Register(
      "inspect", [&Seen](luna::ArgumentPack Args) { Seen = Args.At(0); });
  luna::ClassBuilder<Holder> Holders = F.State.Bindings().RegisterClass<Holder>(
      "Holder", luna::StableTypeKey("t.iv.Holder"));
  Holders.Factory("new", [] { return Holder{}; })
      .Operator(luna::ClassOperator::ToText,
                [](const Holder &) { return std::string("holder!"); });
  Check(Holders.Commit().IsSuccess(), "a class registers");

  Check(F.Run("inspect({1, 'two', true, nested = {x = 1}, f = print})"),
        "a table is passed");
  Check(Seen.Kind() == luna::ValueCategory::Table && Seen.Size() == 3 &&
            Seen.FieldCount() == 2,
        "a table reports its sequence and its fields");
  Check(Seen.Element(0).ToNumber() == 1.0 && Seen.Element(1).TextBytes() == "two" &&
            Seen.Element(2).ToBoolean() == true,
        "its elements read back");
  Check(Seen.Field("nested").Field("x").ToNumber() == 1.0,
        "a field of a field reads back");
  Check(Seen.Field("f").Kind() == luna::ValueCategory::Function,
        "a function is reported as one");
  Check(Seen.Field("missing").Kind() == luna::ValueCategory::Nil &&
            Seen.Element(99).Kind() == luna::ValueCategory::Nil,
        "a missing entry reads as nil");
  Check(Seen.Element(0).ToText() == "1" && !Seen.Field("nested").ToText(),
        "scalars have text and tables do not");
  bool NamedNested = false;
  for (std::size_t Index = 0; Index < Seen.FieldCount(); ++Index)
    NamedNested = NamedNested || Seen.FieldName(Index) == "nested";
  Check(NamedNested, "field names are listed");

  Check(F.Run("inspect(Holder.new())"), "an instance is passed");
  Check(Seen.Kind() == luna::ValueCategory::Userdata && Seen.IsUserdata() &&
            Seen.UserdataClassName() == "Holder" &&
            Seen.UserdataText() == "holder!" && Seen.UserdataIsLive() &&
            Seen.UserdataTarget() != nullptr,
        "an instance describes its class, its text and its object");
}
} // namespace

int main() {
  // Unbuffered, so the last line printed survives a crash.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("luna tests\n");
  TestFunctions();
  TestSuspension();
  TestValues();
  TestNamespaces();
  TestClasses();
  TestAliases();
  TestExtension();
  TestOwnership();
  TestInterrupt();
  TestChunks();
  TestStateLifetime();
  TestReflection();
  TestValidation();
  TestIndexFallback();
  TestIteration();
  TestScheduler();
  TestEnumObjects();
  TestSuspendFromMetamethod();
  TestContainers();
  TestLimits();
  TestSignals();
  TestOverloads();
  TestDiagnostics();
  TestDefinitions();
  TestBaseClasses();
  TestHandlerOutlivesTask();
  TestUnregisteredClassRefused();
  TestNestedSchedulerEntry();
  TestChunkYields();
  TestInterruptWithoutBudget();
  TestRegistrationNeverThrows();
  TestMapNumericKeys();
  TestOverloadsAreStrict();
  TestFinishedTasksAreReclaimed();
  TestTaskFailuresAreReported();
  TestReturnsReferencesAndMoves();
  TestFailedCommitLeavesNothing();
  TestNameBelongsToOneType();
  TestChunksAreReleased();
  TestBindingsAreLocked();
  TestSuspensionEdges();
  TestSignalWaitValues();
  TestStrictMembers();
  TestValuesRoundTrip();
  TestNumbersAtTheEdge();
  TestStateMovesAndGoes();
  TestSymbolsAreSafeToRead();
  TestDiagnosticsAndDefinitions();
  TestOutOfMemoryAtTaskCreation();
  TestTaskLibrary();
  TestWakeFromAnotherThread();
  TestChunkReturnedToScript();
  TestEnumeratorTypes();
  TestPublicClassTrait();
  TestScriptSignals();
  TestInspectingValues();
  std::printf("\n%d checks, %d failure%s\n", Cases, Failures,
              Failures == 1 ? "" : "s");
  return Failures == 0 ? 0 : 1;
}
