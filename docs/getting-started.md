# Getting started

Luna builds as a C++20 static library. The repository uses CMake presets and Ninja, and fetches pinned Luau and RapidCheck revisions during the first configuration.

## Requirements

You need CMake 3.25 or newer, Ninja, a C++20 compiler, and Git. The first configure also needs network access for `FetchContent`.

```bat
cmake --preset ninja-debug
cmake --build --preset ninja-debug
ctest --preset ninja-debug
```

Use `ninja-release` in all three commands for a Release build. The library target is `Luna`, with the namespaced alias `Luna::Luna`.

## A complete first program

```cpp
#include <luna/luna.hpp>

int main() {
  Luna::State State;
  if (!State.IsReady())
    return 1;

  const Luna::RegistrationResult Registration =
      State.Bindings().Register("Add", [](int A, int B) { return A + B; });
  if (!Registration.IsSuccess())
    return 1;

  const Luna::ExecutionResult Execution = State.Execute("assert(Add(20, 22) == 42)");
  return Execution.IsSuccess() ? 0 : 1;
}
```

Link the consumer only to `Luna::Luna`. Do not add Luau include directories or link Luau directly; Luna keeps that dependency behind its public boundary.

The standard Luau libraries are opened when a ready State is created, so ordinary helpers such as `assert`, `math`, `string`, and `bit32` are available to executed source.

## A slightly larger one

The same registry reaches every category. A namespace, a constant, an enum, and a class all stage into one pending plan and publish together when the plan commits.

```cpp
#include <luna/luna.hpp>

#include <string>

enum class Channel : int { Debug = 10, Info = 20 };

class Sprite {
public:
  Sprite() = default;
  Sprite(std::string Name, double Width) : NameValue(std::move(Name)), Width(Width) {}

  [[nodiscard]] std::string Name() const { return NameValue; }
  [[nodiscard]] double Area() const { return Width * Width; }

  double Width = 1.0;

private:
  std::string NameValue;
};

int main() {
  Luna::State State;
  if (!State.IsReady())
    return 1;

  Luna::BindingRegistry Registry = State.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");

  Studio.RegisterConstant("Version", "0.1.0")
      .Documentation("The demo surface.");

  Luna::EnumBuilder<Channel> Channels =
      Studio.RegisterEnum<Channel>("Channel", Luna::StableTypeKey("app.Channel"));
  Channels.Value("Debug", Channel::Debug).Value("Info", Channel::Info);

  Luna::ClassBuilder<Sprite> Sprites =
      Studio.RegisterClass<Sprite>("Sprite", Luna::StableTypeKey("app.Sprite"));
  Sprites.Constructor<std::string, double>()
      .Field("Width", &Sprite::Width)
      .Property("Name", &Sprite::Name)
      .Property("Area", Luna::PropertyPolicy::Lazy(), &Sprite::Area);

  if (!Studio.Commit().IsSuccess())
    return 1;

  const Luna::ExecutionResult Execution = State.Execute(R"(
    local S = Studio.Sprite.New("hero", 3)
    assert(S.Area == 9)
    assert(S.Name == "hero")
    assert(Studio.Channel.Info == 20)
  )");
  return Execution.IsSuccess() ? 0 : 1;
}
```

Two details matter and are explained later. A user-defined leaf type — an enum or a class — is always identified by an explicit `StableTypeKey`, never by an RTTI name (see [values and validation](values-and-validation.md)). And builders stage a plan rather than installing anything: `Commit` publishes the whole plan or none of it (see [namespaces, constants and enums](namespaces-constants-and-enums.md)).

## The worked example

`demo/imgui_color_text_edit/src/main.cpp` registers a representative surface — overloads, optional and defaulted and variadic parameters, both kinds of multiple return, nested namespaces, a scoped enum with an alias, a bitflag enum, a class hierarchy with operators, an asynchronous function, a signal with subscribed handlers, and a versioned module graph — and then lets you run scripts against it, browse the reflection, generate artifacts, and freeze the State. Build it with:

```bat
cmake --preset ninja-debug -DLUNA_BUILD_IMGUI_DEMO=ON
cmake --build --preset ninja-debug --target LunaImGuiDemo
```

It links `Luna::Luna` and nothing else from Luna, so every snippet it shows is public API.

---

[← Previous: Setup](setup.md) · [Documentation index](README.md) · [Next: State and lifetime →](state-and-lifetime.md)
