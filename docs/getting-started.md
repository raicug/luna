# Getting started

Build the repository, run its tests, then write two programs: a function, and a class with a namespace, an enum and a script that waits.

## Build and test

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

CTest runs three entries: `luna_tests`, `luna_boundary` and `luna_demo` (the demo's headless self-test). To run the benchmarks:

```powershell
.\build\LunaBench.exe
```

## A first program

```cpp
#include <luna/luna.hpp>

int main() {
  luna::State state;
  if (!state.IsReady())
    return 1;

  const luna::RegistrationResult registered = state.Bindings().Register(
      "Add", [](double a, double b) { return a + b; });
  if (!registered.IsSuccess())
    return 1;

  const luna::ExecutionResult ran = state.Execute("assert(Add(20, 22) == 42)");
  return ran.IsSuccess() ? 0 : 1;
}
```

Link the program to `luna::luna` and nothing else. The standard Luau libraries (`assert`, `math`, `string`, `table`, `bit32`, `buffer` and so on) are open in every new state.

## A larger one

A namespace, an enum and a class stage into one batch and publish together when the batch commits.

```cpp
#include <luna/luna.hpp>

#include <cstdio>
#include <string>

struct Sprite {
  double Width = 1.0;
  double Height = 1.0;
  int Writes = 0;

  double Area() const { return Width * Height; }
  std::string ToText() const {
    return "Sprite(" + std::to_string(Width) + "x" + std::to_string(Height) + ")";
  }
};

// One line, at global scope, makes a type bindable.
template <> struct luna::detail::RegisteredClassTrait<Sprite> : std::true_type {};

enum class Channel : int { Debug = 10, Info = 20 };

int main() {
  luna::State state;
  luna::BindingRegistry registry = state.Bindings();

  luna::NamespaceBuilder studio = registry.RegisterNamespace("Studio");
  studio.RegisterConstant("Version", "0.1.0");

  luna::EnumBuilder<Channel> channels = studio.RegisterEnum<Channel>(
      "Channel", luna::StableTypeKey("app.Channel"));
  channels.Value("Debug", Channel::Debug)
      .Value("Info", Channel::Info)
      .Alias("Default", "Info");
  channels.Commit();

  luna::ClassBuilder<Sprite> sprites = studio.RegisterClass<Sprite>(
      "Sprite", luna::StableTypeKey("app.Sprite"));
  sprites.Factory("new", [](double w, double h) { return Sprite{w, h}; })
      .Method("Area", &Sprite::Area)
      .Property("Width", [](const Sprite &s) { return s.Width; },
                [](Sprite &s, double v) { s.Width = v; },
                luna::PropertyPolicy::Notify(
                    [](Sprite &s, std::string_view) { ++s.Writes; }))
      .Operator(luna::ClassOperator::ToText, &Sprite::ToText);
  const luna::RegistrationResult committed = sprites.Commit();
  if (!committed.IsSuccess()) {
    std::printf("%s\n", committed.Diagnostic()->Message().c_str());
    return 1;
  }

  registry.RegisterFunction("wait", [](double seconds) { luna::Suspend(seconds); });
  registry.RegisterFunction("print", [](luna::ArgumentPack args) {
    for (std::size_t i = 0; i < args.Size(); ++i)
      std::printf("%s ", std::string(args.View(i).AsString()).c_str());
    std::printf("\n");
  });

  const luna::ExecutionResult result = state.Execute(R"(
    local s = Studio.Sprite.new(3, 4)
    s.Width = 7
    print(tostring(s) .. " area " .. s:Area() .. " channel " .. Studio.Channel.Default)
    wait(0.5)
    print("resumed")
  )");
  if (!result.IsSuccess())
    std::printf("%s\n", result.Diagnostic()->Message().c_str());
  return result.IsSuccess() ? 0 : 1;
}
```

Things to notice:

- `studio.RegisterEnum` and `studio.RegisterClass` share the namespace's pending batch. The enum's `Commit()` publishes everything staged so far, and the class's `Commit()` publishes the rest.
- `Commit()` returns a `RegistrationResult`. It never throws, and a refused commit leaves the VM untouched. See [Errors and results](errors-and-results.md).
- `wait` calls `luna::Suspend`, which yields the running script. `Execute` drives the clock itself, so the script finishes. See [Tasks and scheduling](tasks-and-scheduling.md).
- The `Notify` hook runs after a successful write, with the receiver and the property name.

## The smallest consumer

`app/src/main.cpp` registers a function, a namespace, an enum, a class, a suspending function and a table-returning function, runs a script against them, and checks the results. It is the quickest way to see the whole surface in one file.

## Where next

[State and lifetime](state-and-lifetime.md) explains what a `State` owns and how it behaves when moved or destroyed. [Registering functions](registering-functions.md) covers everything a callable can take and return.

---

[← Previous: Setup](setup.md) · [Documentation index](README.md) · [Next: State and lifetime →](state-and-lifetime.md)
