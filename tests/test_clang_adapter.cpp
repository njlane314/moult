#include "moult/clang/adapter.hpp"
#include "moult/cpp/modernization.hpp"
#include "moult/core/api.hpp"

#include <cassert>
#include <memory>
#include <string>

using namespace moult::clang_adapter;
using namespace moult::core;
using namespace moult::cpp_modernization;

static std::size_t count_opportunity(const FactStore& facts, std::string_view object) {
    return facts.where([&](const Fact& fact) {
        return fact.kind == "cpp.modernization.opportunity" && fact.object == object;
    }).size();
}

int main() {
    SourceStore sources;
    sources.add("legacy.cpp",
                "#define NULL 0\n"
                "namespace std { template <class T> class auto_ptr {}; }\n"
                "int* legacy_pointer() throw() {\n"
                "    register int value = 0;\n"
                "    int* pointer = NULL;\n"
                "    typedef int LegacyInt;\n"
                "    LegacyInt casted = (int)3.0;\n"
                "    int* raw = new int;\n"
                "    delete raw;\n"
                "    return pointer;\n"
                "}\n"
                "void ownership() {\n"
                "    std::auto_ptr<int> owned;\n"
                "}\n");

    Engine engine;
    engine.set_adapter(std::make_shared<ClangCppModernizationAdapter>());
    engine.add_capsule(std::make_shared<CppModernizationCapsule>());

    RunOptions options;
    options.target = std::string(target_name);
    options.minimum_confidence = Confidence::High;

    auto result = engine.run(sources, options);
    assert(count_opportunity(result.facts, "use-nullptr") == 1);
    assert(count_opportunity(result.facts, "use-noexcept") == 1);
    assert(count_opportunity(result.facts, "remove-register") == 1);
    assert(count_opportunity(result.facts, "replace-auto-ptr") == 1);
    assert(count_opportunity(result.facts, "prefer-using-alias") == 1);
    assert(count_opportunity(result.facts, "review-c-style-cast") == 1);
    assert(count_opportunity(result.facts, "review-raw-new") == 1);
    assert(count_opportunity(result.facts, "review-raw-delete") == 1);
    assert(!result.facts.by_kind("cxx.translation_unit").empty());
    assert(!result.facts.by_kind("cxx.function").empty());
    assert(!result.facts.by_kind("cxx.declaration").empty());
    assert(result.plan.accepted_edit_count() == 3);

    const auto applied = result.plan.edits.apply_to_memory(sources);
    const std::string& text = applied.at("legacy.cpp");
    assert(text.find("noexcept") != std::string::npos);
    assert(text.find("nullptr") != std::string::npos);
    assert(text.find("register int value") == std::string::npos);
    assert(text.find("std::auto_ptr<int> owned") != std::string::npos);
    assert(text.find("typedef int LegacyInt") != std::string::npos);
    assert(text.find("(int)3.0") != std::string::npos);
    assert(text.find("new int") != std::string::npos);
    assert(text.find("delete raw") != std::string::npos);

    return 0;
}
