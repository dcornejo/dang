// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "yang/module_resolver.h"
#include "yang/semantic_model.h"
#include "yang/type_system.h"
#include "test_support.h"

namespace yang::semantic {
namespace {

struct Pipeline {
  VectorDiagnosticSink sink;
  InMemoryModuleRepository repository;
  std::shared_ptr<const ResolvedModule> module;
  std::optional<SemanticContext> semantics;

  explicit Pipeline(std::string source) {
    ModuleResolver modules(repository, sink);
    module = modules.Resolve(test::Source(std::move(source), sink));
    if (module) {
      SymbolResolver symbols(sink);
      semantics = symbols.Resolve(module);
    }
  }
};

const Statement* FindTopLevel(const ResolvedModule& module, std::string_view keyword,
                              std::string_view argument) {
  const SyntaxTree& tree = *module.syntax;
  const Statement& root = tree.Get(tree.roots().front());
  for (const StatementId id : root.children) {
    const Statement& child = tree.Get(id);
    if (child.keyword == keyword && child.argument == argument) return &child;
  }
  return nullptr;
}

TEST(TypeSystemTest, ResolvesTypedefChainsAndRestrictions) {
  Pipeline pipeline(R"yang(module types {
    namespace "urn:types"; prefix t;
    typedef percentage {
      type decimal64 { fraction-digits 2; range "0..100"; }
    }
    leaf load { type percentage; default "12.50"; }
    leaf label { type string { length "1..64"; pattern "[a-z]+"; } }
  })yang");
  ASSERT_TRUE(pipeline.semantics);
  TypeResolver resolver(pipeline.sink);
  auto types = resolver.Resolve(*pipeline.semantics);
  ASSERT_TRUE(types);
  const Statement* load = FindTopLevel(*pipeline.module, "leaf", "load");
  ASSERT_NE(load, nullptr);
  auto type = types->Find(*pipeline.module, load->id);
  ASSERT_TRUE(type);
  EXPECT_EQ(type->builtin, BuiltinType::kDecimal64);
  ASSERT_EQ(type->fraction_digits, 2);
  ASSERT_EQ(type->ranges.size(), 1);
  EXPECT_EQ(type->ranges[0].upper, "100");
  ASSERT_EQ(type->typedef_chain.size(), 1);
  EXPECT_EQ(type->typedef_chain[0], "percentage");
}

TEST(TypeSystemTest, ModelsEnumsBitsAndUnions) {
  Pipeline pipeline(R"yang(module types {
    namespace "urn:types"; prefix t;
    leaf color { type enumeration { enum red; enum green { value 7; } enum blue; } default green; }
    leaf flags { type bits { bit up; bit down { position 4; } } default "up down"; }
    leaf flexible { type union { type boolean; type string; } default anything; }
  })yang");
  ASSERT_TRUE(pipeline.semantics);
  TypeResolver resolver(pipeline.sink);
  auto types = resolver.Resolve(*pipeline.semantics);
  ASSERT_TRUE(types);
  auto color = types->Find(*pipeline.module, FindTopLevel(*pipeline.module, "leaf", "color")->id);
  ASSERT_TRUE(color);
  EXPECT_EQ(color->enum_values.at("red"), 0);
  EXPECT_EQ(color->enum_values.at("green"), 7);
  EXPECT_EQ(color->enum_values.at("blue"), 8);
  auto flexible = types->Find(*pipeline.module, FindTopLevel(*pipeline.module, "leaf", "flexible")->id);
  ASSERT_TRUE(flexible);
  EXPECT_EQ(flexible->union_members.size(), 2);
}

TEST(TypeSystemTest, DetectsTypedefCycles) {
  Pipeline pipeline(R"yang(module types {
    namespace "urn:types"; prefix t;
    typedef first { type second; }
    typedef second { type first; }
    leaf value { type first; }
  })yang");
  ASSERT_TRUE(pipeline.semantics);
  TypeResolver resolver(pipeline.sink);
  EXPECT_FALSE(resolver.Resolve(*pipeline.semantics));
  bool cycle = false;
  for (const auto& diagnostic : pipeline.sink.diagnostics()) {
    cycle |= diagnostic.code == DiagnosticCode::kTypeCycle;
  }
  EXPECT_TRUE(cycle);
}

TEST(TypeSystemTest, RejectsInvalidRestrictionsAndDefaults) {
  Pipeline pipeline(R"yang(module types {
    namespace "urn:types"; prefix t;
    leaf decimal { type decimal64 { fraction-digits 19; } }
    leaf choice { type union; }
    leaf enabled { type boolean; default maybe; }
  })yang");
  ASSERT_TRUE(pipeline.semantics);
  TypeResolver resolver(pipeline.sink);
  EXPECT_FALSE(resolver.Resolve(*pipeline.semantics));
  std::size_t restrictions = 0;
  std::size_t defaults = 0;
  for (const auto& diagnostic : pipeline.sink.diagnostics()) {
    restrictions += diagnostic.code == DiagnosticCode::kInvalidTypeRestriction;
    defaults += diagnostic.code == DiagnosticCode::kInvalidDefaultValue;
  }
  EXPECT_GE(restrictions, 2);
  EXPECT_EQ(defaults, 1);
}

TEST(TypeSystemTest, RequiresLeafrefPathAndIdentityrefBase) {
  Pipeline pipeline(R"yang(module types {
    namespace "urn:types"; prefix t;
    leaf target { type string; }
    leaf reference { type leafref; }
    leaf identity { type identityref; }
  })yang");
  ASSERT_TRUE(pipeline.semantics);
  TypeResolver resolver(pipeline.sink);
  EXPECT_FALSE(resolver.Resolve(*pipeline.semantics));
}

TEST(TypeSystemTest, EnforcesRestrictionNarrowingAndRestrictedDefaults) {
  Pipeline narrowing(R"yang(module types {
    namespace "urn:types"; prefix t;
    typedef small { type uint8 { range "0..100"; } }
    leaf invalid { type small { range "90..110"; } }
  })yang");
  ASSERT_TRUE(narrowing.semantics);
  TypeResolver narrowing_resolver(narrowing.sink);
  EXPECT_FALSE(narrowing_resolver.Resolve(*narrowing.semantics));

  Pipeline default_value(R"yang(module types {
    namespace "urn:types"; prefix t;
    leaf value { type uint8 { range "1..10"; } default "11"; }
  })yang");
  ASSERT_TRUE(default_value.semantics);
  TypeResolver default_resolver(default_value.sink);
  EXPECT_FALSE(default_resolver.Resolve(*default_value.semantics));
  EXPECT_EQ(default_value.sink.diagnostics().back().code,
            DiagnosticCode::kInvalidDefaultValue);
}

TEST(TypeSystemTest, ValidatesPatternsAndInvertMatchDefaults) {
  Pipeline invalid_default(R"yang(module types {
    yang-version 1.1;
    namespace "urn:types"; prefix t;
    leaf value {
      type string {
        pattern "forbidden" { modifier invert-match; }
      }
      default "forbidden";
    }
  })yang");
  ASSERT_TRUE(invalid_default.semantics);
  TypeResolver default_resolver(invalid_default.sink);
  EXPECT_FALSE(default_resolver.Resolve(*invalid_default.semantics));

  Pipeline invalid_pattern(R"yang(module types {
    namespace "urn:types"; prefix t;
    leaf value { type string { pattern "["; } }
  })yang");
  ASSERT_TRUE(invalid_pattern.semantics);
  TypeResolver pattern_resolver(invalid_pattern.sink);
  EXPECT_FALSE(pattern_resolver.Resolve(*invalid_pattern.semantics));
}

TEST(TypeSystemTest, UsesXmlSchemaPatternSemantics) {
  ResolvedType anchored;
  anchored.builtin = BuiltinType::kString;
  anchored.patterns.push_back("a+");
  EXPECT_TRUE(ValueMatchesType(anchored, "aaa"));
  EXPECT_FALSE(ValueMatchesType(anchored, "baaa"));

  ResolvedType unicode_letters;
  unicode_letters.builtin = BuiltinType::kString;
  unicode_letters.patterns.push_back(R"(\p{L}+)");
  EXPECT_TRUE(ValueMatchesType(unicode_letters, "Kahului"));
  EXPECT_TRUE(ValueMatchesType(unicode_letters, "M\xC4\x81noa"));
  EXPECT_FALSE(ValueMatchesType(unicode_letters, "route-1"));

  ResolvedType consonants;
  consonants.builtin = BuiltinType::kString;
  consonants.patterns.push_back("[a-z-[aeiou]]+");
  EXPECT_TRUE(ValueMatchesType(consonants, "rhythm"));
  EXPECT_FALSE(ValueMatchesType(consonants, "aloha"));

  ResolvedType inverted = consonants;
  inverted.pattern_inverted.push_back(true);
  EXPECT_FALSE(ValueMatchesType(inverted, "rhythm"));
  EXPECT_TRUE(ValueMatchesType(inverted, "aloha"));
}

TEST(TypeSystemTest, CachesCompiledXmlSchemaPatterns) {
  const auto first = XmlSchemaRegex::Compile(R"(\p{Nd}{1,3})");
  const auto second = XmlSchemaRegex::Compile(R"(\p{Nd}{1,3})");
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first, second);
  EXPECT_TRUE(first->Matches("123"));
  EXPECT_FALSE(first->Matches("1234"));
}

TEST(TypeSystemTest, ValidatesBinaryLexicalFormAndDecodedLength) {
  ResolvedType binary;
  binary.builtin = BuiltinType::kBinary;
  binary.lengths.push_back({"2", "2"});
  EXPECT_TRUE(ValueMatchesType(binary, "YWI="));
  EXPECT_TRUE(ValueMatchesType(binary, "YW\nI="));
  EXPECT_FALSE(ValueMatchesType(binary, "YQ=="));
  EXPECT_FALSE(ValueMatchesType(binary, "not-base64"));
}

TEST(TypeSystemTest, AcceptsYangIntegerDefaultAlternateLexicalForms) {
  ResolvedType signed_type;
  signed_type.builtin = BuiltinType::kInt16;
  EXPECT_TRUE(ValueMatchesType(signed_type, "+17"));
  EXPECT_TRUE(ValueMatchesType(signed_type, "0x11"));
  EXPECT_TRUE(ValueMatchesType(signed_type, "021"));
  EXPECT_FALSE(ValueMatchesType(signed_type, "09"));

  ResolvedType unsigned_type;
  unsigned_type.builtin = BuiltinType::kUint16;
  EXPECT_TRUE(ValueMatchesType(unsigned_type, "+0x11"));
  EXPECT_FALSE(ValueMatchesType(unsigned_type, "-1"));
}

}  // namespace
}  // namespace yang::semantic
