#include <luna/state.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace luna {
namespace {
bool IsIdentifier(std::string_view Text) {
  if (Text.empty())
    return false;
  if (!(Text.front() == '_' || (Text.front() >= 'a' && Text.front() <= 'z') ||
        (Text.front() >= 'A' && Text.front() <= 'Z')))
    return false;
  for (const char Character : Text) {
    const bool Ok = Character == '_' ||
                    (Character >= 'a' && Character <= 'z') ||
                    (Character >= 'A' && Character <= 'Z') ||
                    (Character >= '0' && Character <= '9');
    if (!Ok)
      return false;
  }
  return true;
}

bool IsReserved(std::string_view Text) {
  static const char *const Words[] = {
      "and", "break",    "do",     "else", "elseif", "end",   "false",
      "for", "function", "if",     "in",   "local",  "nil",   "not",
      "or",  "repeat",   "return", "then", "true",   "until", "while"};
  for (const char *Word : Words) {
    if (Text == Word)
      return true;
  }
  return false;
}

// A name that is a keyword cannot stand bare in a table type.
std::string KeyOf(std::string_view Name) {
  if (IsReserved(Name))
    return "[\"" + std::string(Name) + "\"]";
  return std::string(Name);
}

bool IsCallable(SymbolKind Kind) {
  return Kind == SymbolKind::Function || Kind == SymbolKind::Method ||
         Kind == SymbolKind::StaticMethod || Kind == SymbolKind::Factory ||
         Kind == SymbolKind::Constructor || Kind == SymbolKind::OverloadSet;
}

std::string CallSignature(const Symbol &Entry) {
  if (Entry.Kind() == SymbolKind::OverloadSet)
    return "(...any) -> any";

  std::string Signature = "(";
  if (Entry.Arity() < 0) {
    Signature += "...any";
  } else {
    for (int Index = 0; Index < Entry.Arity(); ++Index) {
      if (Index > 0)
        Signature += ", ";
      Signature += Index >= Entry.Required() ? "any?" : "any";
    }
  }
  Signature += ") -> any";
  return Signature;
}

std::string MemberType(const Symbol &Entry) {
  return IsCallable(Entry.Kind()) ? CallSignature(Entry) : "any";
}

struct Node {
  std::map<std::string, Node> Children;
  std::vector<const Symbol *> Members;
};

void Indent(std::string &Out, int Depth) {
  Out.append(static_cast<std::size_t>(Depth) * 4, ' ');
}

void AppendBody(const Node &Shape, int Depth, std::string &Out) {
  std::vector<const Symbol *> Members = Shape.Members;
  std::sort(
      Members.begin(), Members.end(),
      [](const Symbol *A, const Symbol *B) { return A->Name() < B->Name(); });

  for (const Symbol *Entry : Members) {
    // A name that also owns members is declared below as a shape instead.
    if (Shape.Children.count(Entry->Name()) != 0)
      continue;
    Indent(Out, Depth);
    Out += KeyOf(Entry->Name());
    Out += ": ";
    Out += MemberType(*Entry);
    Out += ",\n";
  }

  for (const auto &Child : Shape.Children) {
    Indent(Out, Depth);
    Out += KeyOf(Child.first);
    Out += ": {\n";
    AppendBody(Child.second, Depth + 1, Out);
    Indent(Out, Depth);
    Out += "},\n";
  }
}

void AppendDeclaration(const std::string &Name, const Node &Shape,
                       std::string &Out) {
  Out += "declare ";
  Out += Name;
  Out += ": {\n";
  AppendBody(Shape, 1, Out);
  Out += "}\n\n";
}
} // namespace

std::string GenerateDefinitions(const ReflectionSnapshot &Snapshot) {
  if (!Snapshot.IsValid())
    return std::string();

  Node Root;
  for (const Symbol &Entry : Snapshot.Symbols()) {
    const std::string &Qualified = Entry.QualifiedName();
    std::vector<std::string> Segments;
    for (std::size_t Start = 0;;) {
      const std::size_t Dot = Qualified.find('.', Start);
      Segments.push_back(Dot == std::string::npos
                             ? Qualified.substr(Start)
                             : Qualified.substr(Start, Dot - Start));
      if (Dot == std::string::npos)
        break;
      Start = Dot + 1;
    }

    bool Valid = !Segments.empty();
    for (const std::string &Segment : Segments)
      Valid = Valid && IsIdentifier(Segment);
    if (!Valid)
      continue;

    Node *Cursor = &Root;
    for (std::size_t Index = 0; Index + 1 < Segments.size(); ++Index)
      Cursor = &Cursor->Children[Segments[Index]];
    Cursor->Members.push_back(&Entry);
  }

  std::string Out;
  for (const auto &Entry : Root.Children) {
    // `declare end` is not Luau, and a global with that name cannot be written.
    if (IsReserved(Entry.first))
      continue;
    AppendDeclaration(Entry.first, Entry.second, Out);
  }

  std::vector<const Symbol *> Values = Root.Members;
  std::sort(Values.begin(), Values.end(), [](const Symbol *A, const Symbol *B) {
    return A->Name() < B->Name();
  });
  for (const Symbol *Entry : Values) {
    // A name that also owns members is already declared as a shape.
    if (Root.Children.count(Entry->Name()) != 0 || IsReserved(Entry->Name()))
      continue;
    Out += "declare ";
    Out += Entry->Name();
    Out += ": ";
    Out += MemberType(*Entry);
    Out += "\n";
  }

  while (!Out.empty() && Out.back() == '\n')
    Out.pop_back();
  return Out;
}

std::string State::Definitions() const {
  return GenerateDefinitions(Reflection());
}

} // namespace luna
