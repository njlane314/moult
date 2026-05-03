#include "moult/core/api.hpp"
#include "moult/cpp/modernization.hpp"

#include <cassert>
#include <memory>
#include <string>

using namespace moult::core;
using namespace moult::cpp_modernization;

int main() {
    SourceStore sources;
    sources.add("legacy.cpp",
                "int* f() throw() {\n"
                "  register int value = 0;\n"
                "  int* p = NULL;\n"
                "  std::auto_ptr<int> owned;\n"
                "  typedef int LegacyInt;\n"
                "  int* raw = new int;\n"
                "  delete raw;\n"
                "  const char* s = \"NULL throw() register std::auto_ptr\";\n"
                "  // NULL throw() register std::auto_ptr\n"
                "  return p;\n"
                "}\n");

    Engine engine;
    engine.set_adapter(std::make_shared<TextualCppModernizationAdapter>());
    engine.add_capsule(std::make_shared<CppModernizationCapsule>());

    RunOptions options;
    options.target = std::string(target_name);
    options.minimum_confidence = Confidence::High;

    auto result = engine.run(sources, options);
    assert(result.facts.all().size() == 7);
    assert(result.plan.findings.size() == 7);
    assert(result.plan.accepted_edit_count() == 3);
    assert(!result.plan.has_errors());

    auto applied = result.plan.edits.apply_to_memory(sources);
    const std::string& text = applied.at("legacy.cpp");
    assert(text.find("noexcept") != std::string::npos);
    assert(text.find("nullptr") != std::string::npos);
    assert(text.find("register int value") == std::string::npos);
    assert(text.find("std::auto_ptr<int> owned") != std::string::npos);
    assert(text.find("typedef int LegacyInt") != std::string::npos);
    assert(text.find("new int") != std::string::npos);
    assert(text.find("delete raw") != std::string::npos);
    assert(text.find("\"NULL throw() register std::auto_ptr\"") != std::string::npos);

    return 0;
}
