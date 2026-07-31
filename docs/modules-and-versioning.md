# Modules and versioning

A module is a named, versioned unit of registration. Luna loads each one at most once per State, resolves its dependency graph by semantic version, and publishes the whole resolved graph as one outermost transaction.

Two registry calls make up the surface. `ProvideModule` makes a definition *available* to resolution without loading it - no callback runs, nothing is installed, nothing is published. Providing the same identity and version twice is idempotent when the normalized manifests compare equal, and a conflict otherwise. `RegisterModule` loads the graph rooted at a manifest.

```cpp
Luna::BindingRegistry Registry = State.Bindings();


const Luna::RegistrationResult Older =
    Registry.ProvideModule(UnitsManifest("1.0.0"), &ConfigureUnits);
const Luna::RegistrationResult Newer =
    Registry.ProvideModule(UnitsManifest("1.2.0"), &ConfigureUnits);



const Luna::RegistrationResult Loaded =
    Registry.RegisterModule(RenderManifest(), &ConfigureRender);
```

## Manifests

`ModuleManifest::TryCreate` validates everything and returns `std::nullopt` on refusal. `Create` reports the exact `ModuleManifestStatus` instead.

```cpp
[[nodiscard]] Luna::ModuleManifest RenderManifest() {
  std::vector<Luna::ModuleDependency> Dependencies;
  Luna::ModuleDependency Units;
  Units.Identity = "studio.units";
  if (const std::optional<Luna::VersionConstraint> Constraint =
          Luna::VersionConstraint::TryParse(">=1.0.0"))
    Units.Constraints.push_back(*Constraint);
  Dependencies.push_back(std::move(Units));

  std::vector<Luna::ModuleExport> Exports;
  Luna::ModuleExport Surface;
  Surface.Kind = Luna::SymbolKind::Namespace;
  Surface.Name = "Render";
  Surface.Documentation = "The render surface.";
  Exports.push_back(std::move(Surface));

  const std::optional<Luna::SemanticVersion> Version =
      Luna::SemanticVersion::TryParse("2.1.0");

  std::optional<Luna::ModuleManifest> Created = Luna::ModuleManifest::TryCreate(
      "studio.render", Version ? *Version : Luna::SemanticVersion(),
      std::move(Dependencies), "The render module.", std::move(Exports));
  return Created ? std::move(*Created) : Luna::ModuleManifest();
}
```

An accepted manifest is normalized: dependencies are merged by identity and sorted, every constraint list is canonically ordered and deduplicated, and exports are canonically ordered. Two manifests describing the same definition therefore compare equal regardless of the order their author declared them in. `Key()` gives the canonical `Identity@Version` text used by module keys and dependency paths.

`ModuleManifestStatus` names each refusal exactly: `EmptyIdentity`, `IdentityTooLong`, `InvalidIdentity`, `InvalidVersion`, `InvalidDependencyIdentity`, `SelfDependency`, `MissingDependencyConstraint`, `InvalidDependencyConstraint`, `InvalidExportName`, `DuplicateExport`.

Exports are metadata. They declare what the module intends to publish, for reflection and generated material; they do not themselves install anything.

## Versions and constraints

`SemanticVersion` parses once into a structural record, so resolution never reparses text. Precedence follows the standard rules: the core triple compares numerically, a prerelease sorts below its release, numeric prerelease identifiers compare numerically, alphanumeric identifiers compare lexically by ASCII, a larger identifier set sorts above a shorter prefix, and build metadata never participates in precedence.

Two comparisons exist deliberately. Resolution uses precedence, so two versions differing only in build metadata are equivalent candidates. Manifest identity uses exact equality including build metadata, so a rebuilt definition is never mistaken for the loaded one.

`VersionConstraint` parses a comparator plus a version. The comparators are `Equal`, `NotEqual`, `Less`, `LessOrEqual`, `Greater`, `GreaterOrEqual`, written `==`, `!=`, `<`, `<=`, `>`, `>=`; a leading `=` or a bare version both mean `Equal`, and `ToString` always emits the canonical `==` form. `IsSatisfiedBy` evaluates with standard precedence, so a prerelease candidate is compared by prerelease rules rather than by text. `SemanticVersionStatus` and `VersionConstraintStatus` name every parse refusal.

A default-constructed version or constraint is the reserved unspecified value: it has no precedence and satisfies nothing.

## Registration callbacks

A module callback receives a transaction-attached `NamespaceBuilder`. Everything it stages joins the load's one outermost transaction, and nothing it does commits on its own.

```cpp
void ConfigureUnits(Luna::NamespaceBuilder &Builder) {
  Luna::NamespaceBuilder Units = Builder.RegisterNamespace("Units");
  Units.RegisterConstant("Metre", 1)
      .RegisterConstant("Pixel", 64)
      .RegisterFunction("ToPixels", &ToPixels)
      .Documentation("Unit constants and conversions.")
      .Documentation("ToPixels", "Converts metres into device pixels.");
}
```

Any callable satisfying `ModuleConfiguration` - invocable with `NamespaceBuilder &` - is accepted, so a function pointer, a lambda, or a functor all work. Nothing a callback throws may cross the boundary: the loader contains the exception, poisons the attempt, and restores the exact pre-load State.

`NamespaceBuilder::RegisterModule` stages a module load *inside* a namespace, so a module's surface can be nested under an existing scope. Its callbacks then run inside that plan's transaction, with builders scoped to that namespace.

## Resolution

One load resolves the whole graph before anything runs:

1. Accumulate every constraint reaching each required identity.
2. Select, for each identity, the **highest** available version satisfying all of them - except an identity already loaded in the State, which stays pinned to its loaded version. A loaded version that violates an accumulated constraint refuses the load as a conflict naming the path.
3. Run every not-yet-loaded dependency callback plus the root callback dependency-first, in canonical order, inside one transaction.
4. Publish the selected graph, its exports, VM values, types, reflection records, and dispatch targets atomically - or none of it.

Failures are canonical, not incidental. A dependency cycle, a version conflict with no satisfying candidate, a missing definition, and a callback refusal each produce one deterministic diagnostic naming the path involved. Because the whole load is one transaction, a refused graph leaves the State exactly as it was.

Load-once means a repeated `RegisterModule` of an identity already loaded is idempotent only when the normalized manifest compares equal to the loaded one, and an idempotent repeat reruns no callback and publishes no new generation. Anything else is a conflict: an unequal definition at the same version, or any different version of that identity. A refused conflict leaves the loaded graph exactly as it was.

## Provenance

Every symbol published by a module carries its origin. In reflection, `ReflectionRecord::HasModule()` and `Module()` name it, and `ModuleRecord` reports the identity, version, documentation, resolved dependencies with the versions chosen for them, declared exports, declared namespaces, and declared canonical types - all in canonical deterministic order, never in load order.

```cpp
const Luna::ReflectionRecord Record = Snapshot.Find("Render.Describe");
if (Record.HasModule()) {
  const Luna::ModuleRecord Module = Record.Module();

}
```

Declaration generation can carry that provenance into its output; see [reflection and generation](reflection-and-generation.md).

## Load-only, and what that means

There is no public entry point that unloads, replaces, or hot reloads a loaded module. Registration is additive: once a module is loaded into a State, it stays for the life of that State. If you need a different graph, build a new State.

That is a deliberate boundary rather than a missing check. The machinery those operations need is implemented and tested privately - affected-closure and blocker analysis over dependents, live userdata, rooted references, caches and retained generations; staging with reverse-order undo; and atomic publication of a new module, reflection, cache, and dispatch generation. What is absent is the decision to enable it: no State declares dynamic lifecycle support, so any such request is refused deterministically with a load-only diagnostic and changes nothing.

---

[← Previous: Classes and userdata](classes-and-userdata.md) · [Documentation index](README.md) · [Next: Reflection and generation →](reflection-and-generation.md)
