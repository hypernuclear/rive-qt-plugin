// rive-asset-gen — build-time generator for typed access to .riv asset
// metadata.
//
// For a given .riv file, emits:
//   1. A C++ header defining `inline constexpr std::string_view`
//      constants under a user-supplied namespace, organized by
//      Artboards / StateMachines / TextRuns / ViewModels.
//   2. (Optional) A QML singleton that exposes the same names as
//      `readonly property string` chains for QML import.
//
// Run at build time via the qt_add_rive_assets() CMake helper. The
// emitted constants make typo-bound lookups (e.g. sm.getNumber("speed"))
// into compile-checked symbols (sm.getNumber(MyAsset::Inputs::Speed))
// without changing the runtime API surface.
//
// Loading uses rive::NoOpFactory (utils/no_op_factory) so the tool runs
// without a real graphics device — pure metadata walk.
//
// CLI:
//   rive-asset-gen --input PATH.riv
//                  --output-cpp PATH.h
//                  --namespace MyApp::Assets::AssetName
//                  [--output-qml PATH.qml]
//                  [--qml-singleton SingletonName]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rive/artboard.hpp>
#include <rive/file.hpp>
#include <rive/text/text_value_run.hpp>
#include <rive/viewmodel/viewmodel.hpp>
#include <rive/viewmodel/viewmodel_instance.hpp>
#include <rive/viewmodel/viewmodel_property.hpp>
#include <rive/viewmodel/viewmodel_property_artboard.hpp>
#include <rive/viewmodel/viewmodel_property_asset_image.hpp>
#include <rive/viewmodel/viewmodel_property_boolean.hpp>
#include <rive/viewmodel/viewmodel_property_color.hpp>
#include <rive/viewmodel/viewmodel_property_enum.hpp>
#include <rive/viewmodel/viewmodel_property_list.hpp>
#include <rive/viewmodel/viewmodel_property_number.hpp>
#include <rive/viewmodel/viewmodel_property_string.hpp>
#include <rive/viewmodel/viewmodel_property_trigger.hpp>
#include <rive/viewmodel/viewmodel_property_viewmodel.hpp>

#include <utils/no_op_factory.hpp>

namespace {

struct Args
{
    std::string input;
    std::string outputCpp;
    std::string outputQml; // optional
    std::string ns;        // C++ namespace, e.g. MyApp::Assets::Foo
    std::string qmlSingleton; // QML singleton element name (default: last
                              // component of ns or basename)
};

void printUsage(const char* argv0)
{
    std::fprintf(stderr,
                 "Usage: %s --input PATH.riv --output-cpp PATH.h "
                 "--namespace NS [--output-qml PATH.qml] "
                 "[--qml-singleton NAME]\n",
                 argv0);
}

bool parseArgs(int argc, char** argv, Args& out)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&](const char* flag) -> const char* {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "missing value for %s\n", flag);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--input")
        {
            const char* v = next("--input");
            if (!v) return false;
            out.input = v;
        }
        else if (a == "--output-cpp")
        {
            const char* v = next("--output-cpp");
            if (!v) return false;
            out.outputCpp = v;
        }
        else if (a == "--output-qml")
        {
            const char* v = next("--output-qml");
            if (!v) return false;
            out.outputQml = v;
        }
        else if (a == "--namespace")
        {
            const char* v = next("--namespace");
            if (!v) return false;
            out.ns = v;
        }
        else if (a == "--qml-singleton")
        {
            const char* v = next("--qml-singleton");
            if (!v) return false;
            out.qmlSingleton = v;
        }
        else
        {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return false;
        }
    }
    if (out.input.empty() || out.outputCpp.empty() || out.ns.empty())
    {
        printUsage(argv[0]);
        return false;
    }
    return true;
}

// Sanitize an arbitrary string into a valid C++/QML identifier.
// - Replace non-[A-Za-z0-9_] with '_'.
// - Collapse runs of '_'.
// - Prepend '_' if first char is a digit.
// - If empty, return "_".
std::string sanitize(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (char c : in)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (ok)
        {
            if (!out.empty() && c == '_' && out.back() == '_')
                continue;
            out.push_back(c);
        }
        else if (out.empty() || out.back() != '_')
        {
            out.push_back('_');
        }
    }
    while (!out.empty() && out.back() == '_')
        out.pop_back();
    if (out.empty())
        out = "_";
    if (out.front() >= '0' && out.front() <= '9')
        out.insert(out.begin(), '_');
    return out;
}

// Disambiguate identifiers that collide after sanitization. Tracks names
// already used in the current scope and appends `_2`, `_3`, … as needed.
struct IdentScope
{
    std::unordered_set<std::string> used;

    std::string make(const std::string& original)
    {
        const std::string base = sanitize(original);
        if (used.insert(base).second)
            return base;
        for (int n = 2;; ++n)
        {
            std::string candidate = base + "_" + std::to_string(n);
            if (used.insert(candidate).second)
                return candidate;
        }
    }
};

std::vector<uint8_t> readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        return {};
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (!in.read(reinterpret_cast<char*>(data.data()), size))
        return {};
    return data;
}

// String-literal-quote with escapes for backslash and double-quote.
std::string cppQuote(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s)
    {
        if (c == '\\' || c == '"')
            out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

// Recursively descend through an Artboard's component tree, collecting
// distinct text run names. We use a set to dedupe — multiple text runs
// can share a name (Rive's editor allows it), and the runtime resolves
// by name + path. The codegen surface only needs the unique names.
void collectTextRunNames(rive::ContainerComponent* container,
                         std::set<std::string>& out)
{
    if (!container)
        return;
    for (rive::Component* child : container->children())
    {
        if (!child)
            continue;
        if (child->is<rive::TextValueRun>())
        {
            const std::string name = child->name();
            if (!name.empty())
                out.insert(name);
        }
        if (child->is<rive::ContainerComponent>())
        {
            collectTextRunNames(child->as<rive::ContainerComponent>(), out);
        }
    }
}

// One row of name → constant per output target.
struct NameEntry
{
    std::string ident;    // sanitized identifier
    std::string original; // original Rive name (string-literal value)
};

struct PropertyBuckets
{
    std::vector<NameEntry> properties; // primitive-valued (number, bool, string, color, enum)
    std::vector<NameEntry> triggers;
    std::vector<NameEntry> nestedViewModels;
    std::vector<NameEntry> lists;
    std::vector<NameEntry> images;
    std::vector<NameEntry> artboards;
};

PropertyBuckets bucketViewModelProperties(rive::ViewModel* vm)
{
    PropertyBuckets out;
    if (!vm)
        return out;
    IdentScope props, triggers, nested, lists, images, artboards;
    for (rive::ViewModelProperty* prop : vm->properties())
    {
        if (!prop)
            continue;
        const std::string name = prop->name();
        if (name.empty())
            continue;
        if (prop->is<rive::ViewModelPropertyTrigger>())
            out.triggers.push_back({triggers.make(name), name});
        else if (prop->is<rive::ViewModelPropertyViewModel>())
            out.nestedViewModels.push_back({nested.make(name), name});
        else if (prop->is<rive::ViewModelPropertyList>())
            out.lists.push_back({lists.make(name), name});
        else if (prop->is<rive::ViewModelPropertyAssetImage>())
            out.images.push_back({images.make(name), name});
        else if (prop->is<rive::ViewModelPropertyArtboard>())
            out.artboards.push_back({artboards.make(name), name});
        else
            // Number, boolean, string, color, enum (custom + system).
            out.properties.push_back({props.make(name), name});
    }
    return out;
}

// Split "MyApp::Assets::Foo" into ["MyApp", "Assets", "Foo"].
std::vector<std::string> splitNs(const std::string& ns)
{
    std::vector<std::string> out;
    std::string current;
    for (size_t i = 0; i < ns.size(); ++i)
    {
        if (ns[i] == ':' && i + 1 < ns.size() && ns[i + 1] == ':')
        {
            if (!current.empty())
                out.push_back(current);
            current.clear();
            ++i;
        }
        else
        {
            current.push_back(ns[i]);
        }
    }
    if (!current.empty())
        out.push_back(current);
    return out;
}

// Walk + emit ----------------------------------------------------------------

struct ArtboardSummary
{
    std::string ident;            // sanitized
    std::string name;             // original
    std::vector<NameEntry> stateMachines;
    std::vector<NameEntry> textRuns;
};

struct ViewModelSummary
{
    std::string ident; // sanitized
    std::string name;  // original
    std::vector<NameEntry> instances;
    PropertyBuckets buckets;
};

struct FileSummary
{
    std::vector<ArtboardSummary> artboards;
    std::vector<ViewModelSummary> viewModels;
};

FileSummary walkFile(rive::File* file)
{
    FileSummary out;
    if (!file)
        return out;

    IdentScope artboardIdents;
    for (size_t i = 0; i < file->artboardCount(); ++i)
    {
        rive::Artboard* ab = file->artboard(i);
        if (!ab)
            continue;
        const std::string name = ab->name();
        if (name.empty())
            continue;

        ArtboardSummary as;
        as.name = name;
        as.ident = artboardIdents.make(name);

        IdentScope smIdents;
        for (size_t s = 0; s < ab->stateMachineCount(); ++s)
        {
            const std::string smName = ab->stateMachineNameAt(s);
            if (smName.empty())
                continue;
            as.stateMachines.push_back({smIdents.make(smName), smName});
        }

        std::set<std::string> textRunNames;
        collectTextRunNames(ab, textRunNames);
        IdentScope trIdents;
        for (const std::string& trName : textRunNames)
            as.textRuns.push_back({trIdents.make(trName), trName});

        out.artboards.push_back(std::move(as));
    }

    IdentScope vmIdents;
    for (size_t i = 0; i < file->viewModelCount(); ++i)
    {
        rive::ViewModel* vm = file->viewModel(i);
        if (!vm)
            continue;
        const std::string name = vm->name();
        if (name.empty())
            continue;

        ViewModelSummary vs;
        vs.name = name;
        vs.ident = vmIdents.make(name);

        IdentScope instIdents;
        for (size_t j = 0; j < vm->instanceCount(); ++j)
        {
            rive::ViewModelInstance* inst = vm->instance(j);
            if (!inst)
                continue;
            const std::string instName = inst->name();
            if (instName.empty())
                continue;
            vs.instances.push_back({instIdents.make(instName), instName});
        }

        vs.buckets = bucketViewModelProperties(vm);
        out.viewModels.push_back(std::move(vs));
    }

    return out;
}

// C++ emission ---------------------------------------------------------------

void emitCppList(std::ostream& os, const std::string& indent,
                 const std::vector<NameEntry>& entries)
{
    for (const NameEntry& e : entries)
    {
        os << indent << "inline constexpr std::string_view " << e.ident
           << " = " << cppQuote(e.original) << ";\n";
    }
}

void emitCppPropertyBuckets(std::ostream& os, const std::string& indent,
                            const PropertyBuckets& b)
{
    auto section = [&](const char* label, const std::vector<NameEntry>& v) {
        if (v.empty())
            return;
        os << indent << "namespace " << label << " {\n";
        emitCppList(os, indent + "    ", v);
        os << indent << "} // namespace " << label << "\n";
    };
    section("Properties", b.properties);
    section("Triggers", b.triggers);
    section("ViewModels", b.nestedViewModels);
    section("Lists", b.lists);
    section("Images", b.images);
    section("Artboards", b.artboards);
}

void emitCpp(std::ostream& os, const Args& args, const FileSummary& sum)
{
    os << "// AUTO-GENERATED — DO NOT EDIT\n"
       << "// rive-asset-gen output for: " << args.input << "\n"
       << "//\n"
       << "// Constants for typed access to artboard / state-machine / view-\n"
       << "// model names. Use these in place of string literals so a typo\n"
       << "// becomes a compile error instead of a silent no-op at runtime.\n"
       << "\n"
       << "#pragma once\n"
       << "\n"
       << "#include <string_view>\n"
       << "\n";

    const auto nsParts = splitNs(args.ns);
    for (const std::string& part : nsParts)
        os << "namespace " << part << " {\n";
    os << "\n";

    if (!sum.artboards.empty())
    {
        // Top-level Artboards namespace lists every artboard's name —
        // useful for `rive.artboard = MyAsset::Artboards::Form`.
        os << "namespace Artboards {\n";
        for (const ArtboardSummary& as : sum.artboards)
        {
            os << "    inline constexpr std::string_view " << as.ident
               << " = " << cppQuote(as.name) << ";\n";
        }
        os << "} // namespace Artboards\n\n";

        // Per-artboard subspaces hold the things scoped to that
        // artboard (state machines, text runs).
        for (const ArtboardSummary& as : sum.artboards)
        {
            if (as.stateMachines.empty() && as.textRuns.empty())
                continue;
            os << "namespace " << as.ident << " {\n";
            if (!as.stateMachines.empty())
            {
                os << "    namespace StateMachines {\n";
                emitCppList(os, "        ", as.stateMachines);
                os << "    } // namespace StateMachines\n";
            }
            if (!as.textRuns.empty())
            {
                os << "    namespace TextRuns {\n";
                emitCppList(os, "        ", as.textRuns);
                os << "    } // namespace TextRuns\n";
            }
            os << "} // namespace " << as.ident << "\n\n";
        }
    }

    if (!sum.viewModels.empty())
    {
        os << "namespace ViewModels {\n";
        for (const ViewModelSummary& vs : sum.viewModels)
        {
            os << "    namespace " << vs.ident << " {\n";
            os << "        inline constexpr std::string_view _Name = "
               << cppQuote(vs.name) << ";\n";
            if (!vs.instances.empty())
            {
                os << "        namespace Instances {\n";
                emitCppList(os, "            ", vs.instances);
                os << "        } // namespace Instances\n";
            }
            emitCppPropertyBuckets(os, "        ", vs.buckets);
            os << "    } // namespace " << vs.ident << "\n";
        }
        os << "} // namespace ViewModels\n\n";
    }

    for (auto it = nsParts.rbegin(); it != nsParts.rend(); ++it)
        os << "} // namespace " << *it << "\n";
}

// QML emission ---------------------------------------------------------------
//
// The QML side mirrors the C++ structure but uses nested QtObject
// `readonly property string` chains. Singleton sits at the top.
//
// QML rule: property names must start with a lowercase letter (or
// underscore) — uppercase first chars are a parse error. Data names
// like "PersonViewModel" become "personViewModel" on the QML side
// only; the C++ side keeps PascalCase namespaces.

std::string qmlIdent(const std::string& cppIdent)
{
    if (cppIdent.empty())
        return cppIdent;
    if (cppIdent.front() >= 'A' && cppIdent.front() <= 'Z')
    {
        std::string out = cppIdent;
        out.front() = static_cast<char>(out.front() - 'A' + 'a');
        return out;
    }
    return cppIdent;
}

void emitQmlList(std::ostream& os, const std::string& indent,
                 const std::vector<NameEntry>& entries)
{
    for (const NameEntry& e : entries)
    {
        os << indent << "readonly property string " << qmlIdent(e.ident)
           << ": " << cppQuote(e.original) << "\n";
    }
}

void emitQmlPropertyBuckets(std::ostream& os, const std::string& indent,
                            const PropertyBuckets& b)
{
    auto section = [&](const char* label, const std::vector<NameEntry>& v) {
        if (v.empty())
            return;
        os << indent << "readonly property QtObject " << qmlIdent(label)
           << ": QtObject {\n";
        emitQmlList(os, indent + "    ", v);
        os << indent << "}\n";
    };
    section("Properties", b.properties);
    section("Triggers", b.triggers);
    section("ViewModels", b.nestedViewModels);
    section("Lists", b.lists);
    section("Images", b.images);
    section("Artboards", b.artboards);
}

void emitQml(std::ostream& os, const Args& args, const FileSummary& sum)
{
    os << "// AUTO-GENERATED — DO NOT EDIT\n"
       << "// rive-asset-gen output for: " << args.input << "\n"
       << "\n"
       << "pragma Singleton\n"
       << "import QtQml\n"
       << "\n"
       << "QtObject {\n";

    if (!sum.artboards.empty())
    {
        os << "    readonly property QtObject artboards: QtObject {\n";
        for (const ArtboardSummary& as : sum.artboards)
        {
            os << "        readonly property string " << qmlIdent(as.ident)
               << ": " << cppQuote(as.name) << "\n";
        }
        os << "    }\n";

        for (const ArtboardSummary& as : sum.artboards)
        {
            if (as.stateMachines.empty() && as.textRuns.empty())
                continue;
            os << "    readonly property QtObject " << qmlIdent(as.ident)
               << ": QtObject {\n";
            if (!as.stateMachines.empty())
            {
                os << "        readonly property QtObject stateMachines: "
                      "QtObject {\n";
                emitQmlList(os, "            ", as.stateMachines);
                os << "        }\n";
            }
            if (!as.textRuns.empty())
            {
                os << "        readonly property QtObject textRuns: "
                      "QtObject {\n";
                emitQmlList(os, "            ", as.textRuns);
                os << "        }\n";
            }
            os << "    }\n";
        }
    }

    if (!sum.viewModels.empty())
    {
        os << "    readonly property QtObject viewModels: QtObject {\n";
        for (const ViewModelSummary& vs : sum.viewModels)
        {
            os << "        readonly property QtObject " << qmlIdent(vs.ident)
               << ": QtObject {\n";
            os << "            readonly property string _name: "
               << cppQuote(vs.name) << "\n";
            if (!vs.instances.empty())
            {
                os << "            readonly property QtObject instances: "
                      "QtObject {\n";
                emitQmlList(os, "                ", vs.instances);
                os << "            }\n";
            }
            emitQmlPropertyBuckets(os, "            ", vs.buckets);
            os << "        }\n";
        }
        os << "    }\n";
    }

    os << "}\n";
}

} // namespace

int main(int argc, char** argv)
{
    Args args;
    if (!parseArgs(argc, argv, args))
        return 2;

    std::vector<uint8_t> data = readFile(args.input);
    if (data.empty())
    {
        std::fprintf(stderr, "rive-asset-gen: failed to read %s\n",
                     args.input.c_str());
        return 1;
    }

    rive::NoOpFactory factory;
    rive::ImportResult result = rive::ImportResult::malformed;
    rive::rcp<rive::File> file = rive::File::import(
        rive::Span<const uint8_t>(data.data(), data.size()), &factory, &result);
    if (!file || result != rive::ImportResult::success)
    {
        std::fprintf(stderr,
                     "rive-asset-gen: import failed (status=%d) for %s\n",
                     static_cast<int>(result), args.input.c_str());
        return 1;
    }

    const FileSummary summary = walkFile(file.get());

    {
        std::ofstream cpp(args.outputCpp);
        if (!cpp)
        {
            std::fprintf(stderr,
                         "rive-asset-gen: cannot write %s\n",
                         args.outputCpp.c_str());
            return 1;
        }
        emitCpp(cpp, args, summary);
    }

    if (!args.outputQml.empty())
    {
        std::ofstream qml(args.outputQml);
        if (!qml)
        {
            std::fprintf(stderr,
                         "rive-asset-gen: cannot write %s\n",
                         args.outputQml.c_str());
            return 1;
        }
        emitQml(qml, args, summary);
    }

    return 0;
}
