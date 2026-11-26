// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//
// SVMake VAMS P0.1: Verilog-AMS (Accellera VAMS-2023) semantics.

#include "Test.h"

#include "slang/ast/ASTSerializer.h"
#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/symbols/AnalogSymbols.h"
#include "slang/diagnostics/VamsDiags.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/Json.h"
#include "slang/text/SourceManager.h"

namespace {

const char* const VamsPrefix = "`begin_keywords \"VAMS-2023\"\n";

Bag vamsOptions(std::vector<std::string> defines = {}) {
    PreprocessorOptions ppo;
    ppo.additionalIncludePaths.push_back(findTestDir() + "vams");
    ppo.predefines = std::move(defines);
    Bag bag;
    bag.set(ppo);
    return bag;
}

std::shared_ptr<SyntaxTree> vamsTree(const std::string& text, const Bag& options = vamsOptions(),
                                     std::string_view name = "source") {
    return SyntaxTree::fromText(VamsPrefix + text, options, name);
}

std::vector<DiagCode> codes(Compilation& compilation) {
    std::vector<DiagCode> result;
    for (auto& d : compilation.getAllDiagnostics())
        result.push_back(d.code);
    return result;
}

const NetSymbol& net(Compilation& compilation, std::string_view path) {
    return compilation.getRoot().lookupName<NetSymbol>(path);
}

} // namespace

TEST_CASE("VAMS natures and disciplines") {
    auto tree = vamsTree(R"(
nature Base
    units = "u"; access = Bx; abstol = 1e-3;
    idt_nature = Base; my_attr = 7;
endnature
nature Flowy units = "f"; access = Fy; abstol = 1; endnature
nature Derived : Base;
    abstol = 1u;
endnature
discipline bd
    potential Derived;
    flow Flowy;
    potential.abstol = 1n;
enddiscipline
nature FromDisc : bd.potential
endnature
discipline \logic ;
    domain discrete;
enddiscipline
discipline empty
enddiscipline
discipline sigflow; potential Base; domain continuous; enddiscipline
module m; endmodule
)");
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;

    auto base = compilation.getNature("Base");
    auto derived = compilation.getNature("Derived");
    auto fromDisc = compilation.getNature("FromDisc");
    REQUIRE(base);
    REQUIRE(derived);
    REQUIRE(fromDisc);
    CHECK(base->getParent() == nullptr);
    CHECK(base->getAccessName() == "Bx");
    CHECK(base->getUnits() == "u");
    CHECK(base->getAbstol() == 1e-3);
    CHECK(base->getIdtNature() == base);
    CHECK(base->getDdtNature() == base);
    REQUIRE(base->getUserAttributes().size() == 1);
    CHECK(base->getUserAttributes()[0].name == "my_attr");

    CHECK(derived->getParent() == base);
    CHECK(&derived->getBase() == base);
    CHECK(derived->getAccessName() == "Bx");
    CHECK(derived->getUnits() == "u");
    CHECK(derived->getAbstol() == 1e-6);
    CHECK(fromDisc->getParent() == derived);
    CHECK(fromDisc->getAbstol() == 1e-6);

    auto bd = compilation.getDiscipline("bd");
    REQUIRE(bd);
    CHECK(bd->getPotential() == derived);
    CHECK(bd->getFlow() == compilation.getNature("Flowy"));
    CHECK(bd->getDomain() == DisciplineSymbol::Domain::Unspecified);
    CHECK(bd->isContinuous());

    auto logic = compilation.getDiscipline("logic");
    REQUIRE(logic);
    CHECK(logic->getDomain() == DisciplineSymbol::Domain::Discrete);
    CHECK(logic->isNatureless());
    CHECK(!logic->isContinuous());

    auto empty = compilation.getDiscipline("empty");
    REQUIRE(empty);
    CHECK(empty->isDomainless());

    // Not in any scope's name map (V-H3).
    CHECK(compilation.getNatures().size() == 4);
    CHECK(compilation.getDisciplines().size() == 4);
    CHECK(compilation.isAccessName("Bx"));
    CHECK(!compilation.isAccessName("V"));
}

TEST_CASE("VAMS nature and discipline errors") {
    auto tree = vamsTree(R"(
nature NoAttrs
endnature
nature Good units = "V"; access = V; abstol = 1; endnature
nature Child : Good
    access = W; units = "x";
endnature
nature BadAccess units = "V"; access = "V"; abstol = 1; endnature
nature BadUnits units = 3; access = U; abstol = 1; endnature
nature BadParent : Nope endnature
nature BadDdt units = "V"; access = D; abstol = 1; ddt_nature = Nope; endnature
discipline d1
    potential Missing;
    flow Good;
enddiscipline
discipline d2
    potential Good;
    flow Good;
enddiscipline
discipline d3
    potential Good;
    potential Good;
    domain continuous;
    domain discrete;
enddiscipline
module m; endmodule
)");
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    auto c = codes(compilation);
    auto count = [&](DiagCode code) { return std::ranges::count(c, code); };
    CHECK(count(diag::NatureMissingAttribute) == 3);
    CHECK(count(diag::NatureAttributeChanged) == 2);
    CHECK(count(diag::NatureAttributeNotName) == 1);
    CHECK(count(diag::NatureUnitsNotString) == 1);
    CHECK(count(diag::UnknownNature) == 3);
    CHECK(count(diag::DisciplineAccessConflict) == 1);
    CHECK(count(diag::DisciplineDuplicateBinding) == 2);
}

TEST_CASE("VAMS Annex D standard definitions elaborate clean") {
    auto text = R"(
`include "disciplines.vams"
`include "constants.vams"
module m;
    localparam real u0 = `P_U0;
    localparam real q = `P_Q;
    localparam real pi = `M_PI;
    electrical a;
endmodule
)";
    auto check = [&](std::vector<std::string> defines, double u0, double q) {
        auto tree = vamsTree(text, vamsOptions(std::move(defines)));
        Compilation compilation;
        compilation.addSyntaxTree(tree);
        NO_COMPILATION_ERRORS;

        auto& root = compilation.getRoot();
        CHECK(root.lookupName<ParameterSymbol>("m.u0").getValue().real() == u0);
        CHECK(root.lookupName<ParameterSymbol>("m.q").getValue().real() == q);

        auto electrical = compilation.getDiscipline("electrical");
        REQUIRE(electrical);
        REQUIRE(electrical->getPotential());
        REQUIRE(electrical->getFlow());
        CHECK(electrical->getPotential()->name == "Voltage");
        CHECK(electrical->getPotential()->getAccessName() == "V");
        CHECK(electrical->getPotential()->getAbstol() == 1e-6);
        CHECK(electrical->getFlow()->name == "Current");
        CHECK(electrical->getFlow()->getAccessName() == "I");
        CHECK(electrical->getFlow()->getIdtNature() == compilation.getNature("Charge"));
        CHECK(compilation.getDiscipline("logic")->getDomain() ==
              DisciplineSymbol::Domain::Discrete);
        CHECK(compilation.getDiscipline("voltage")->getFlow() == nullptr);
        CHECK(net(compilation, "m.a").discipline == electrical);
    };

    check({}, 4.0e-7 * 3.14159265358979323846, 1.602176462e-19);
    check({"PHYSICAL_CONSTANTS_NIST2018"}, 1.25663706212e-6, 1.602176634e-19);
}

TEST_CASE("VAMS discipline nets and ports") {
    auto tree = vamsTree(R"(
`include "disciplines.vams"
module res(p, n);
    inout p, n;
    electrical p, n;
endmodule
module top;
    electrical in, out;
    electrical [3:0] bus;
    res r1(in, out);
endmodule
)");
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;

    auto electrical = compilation.getDiscipline("electrical");
    CHECK(net(compilation, "top.in").discipline == electrical);
    CHECK(net(compilation, "top.r1.p").discipline == electrical);
    CHECK(net(compilation, "top.bus").discipline == electrical);
    CHECK(net(compilation, "top.bus").getType().getBitWidth() == 4);
    CHECK(net(compilation, "top.in").getType().getBitWidth() == 1);

    auto& top = compilation.getRoot().lookupName<InstanceSymbol>("top");
    CHECK(top.getDefinition().isVams);
}

TEST_CASE("VAMS disciplines are invisible to SystemVerilog") {
    // V-H3: natures and disciplines are in no scope; an SV module of the same unit may use
    // their names, and an SV `electrical a;` is not a discipline net.
    auto tree = SyntaxTree::fromText(std::string(VamsPrefix) + R"(
`include "disciplines.vams"
module v; electrical a; endmodule
`end_keywords
module s;
    logic Voltage, electrical, V;
endmodule
module s2;
    electrical b;
endmodule
)",
                                     vamsOptions());
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    auto c = codes(compilation);
    REQUIRE(c.size() == 1);
    CHECK(c[0] == diag::UndeclaredIdentifier);

    auto& root = compilation.getRoot();
    CHECK(root.lookupName<InstanceSymbol>("v").getDefinition().isVams);
    CHECK(!root.lookupName<InstanceSymbol>("s").getDefinition().isVams);
    CHECK(!root.lookupName<InstanceSymbol>("s2").getDefinition().isVams);
}

TEST_CASE("VAMS ground declarations") {
    auto tree = vamsTree(R"(
`include "disciplines.vams"
module m;
    electrical gnd, a;
    ground gnd;
    ground electrical g2;
    wire w;
    ground w;
    ground undeclared;
    ground logic dg;
endmodule
)");
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    auto c = codes(compilation);
    CHECK(c == std::vector<DiagCode>{diag::GroundNotContinuous, diag::UndeclaredIdentifier,
                                     diag::GroundNotContinuous});

    CHECK(net(compilation, "m.gnd").isGround);
    CHECK(!net(compilation, "m.a").isGround);
    CHECK(net(compilation, "m.g2").isGround);
    CHECK(net(compilation, "m.g2").discipline == compilation.getDiscipline("electrical"));
}

TEST_CASE("VAMS branches") {
    auto tree = vamsTree(R"(
`include "disciplines.vams"
module m;
    electrical a, b;
    electrical [1:0] v;
    branch (a, b) ab;
    branch (a) ag;
    branch (v[1], b) vb;
endmodule
)");
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;

    auto& root = compilation.getRoot();
    auto& ab = root.lookupName<BranchSymbol>("m.ab");
    REQUIRE(ab.getPositive().net);
    CHECK(ab.getPositive().net->name == "a");
    REQUIRE(ab.getNegative());
    REQUIRE(ab.getNegative()->net);
    CHECK(ab.getNegative()->net->name == "b");
    CHECK(ab.getDiscipline() == compilation.getDiscipline("electrical"));

    auto& ag = root.lookupName<BranchSymbol>("m.ag");
    REQUIRE(ag.getPositive().net);
    CHECK(ag.getPositive().net->name == "a");
    CHECK(ag.getNegative() == nullptr);

    auto& vb = root.lookupName<BranchSymbol>("m.vb");
    REQUIRE(vb.getPositive().net);
    CHECK(vb.getPositive().net->name == "v");
    CHECK(vb.getPositive().index == 1);
}

TEST_CASE("VAMS branch errors") {
    auto tree = vamsTree(R"(
`include "disciplines.vams"
module m;
    electrical a;
    electrical [1:0] v;
    magnetic mg;
    logic \logic_var ;
    wire w;
    real r;
    branch (a, a) same;
    branch (w) nodisc;
    branch (r) notnet;
    branch (a, mg) mismatch;
    branch (v) vec;
    branch (<a>) portb;
endmodule
)");
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    auto c = codes(compilation);
    std::ranges::sort(c, [](DiagCode x, DiagCode y) { return x.getCode() < y.getCode(); });
    std::vector<DiagCode> expected{diag::VamsNotSupported,          diag::VamsNotSupported,
                                   diag::BranchTerminalNotNet,      diag::BranchTerminalNoDiscipline,
                                   diag::BranchDisciplineMismatch, diag::BranchSameTerminal};
    std::ranges::sort(expected, [](DiagCode x, DiagCode y) { return x.getCode() < y.getCode(); });
    CHECK(c == expected);
}

TEST_CASE("VAMS discipline compatibility (3.11.1)") {
    auto tree = vamsTree(R"(
nature V1 units = "V"; access = V1; abstol = 1; endnature
nature V1d : V1 endnature
nature V2 units = "V"; access = V2; abstol = 1; endnature
nature A1 units = "A"; access = A1; abstol = 1; endnature
nature A2 units = "Amp"; access = A2; abstol = 1; endnature
discipline e1 potential V1; flow A1; enddiscipline
discipline e1d potential V1d; flow A1; enddiscipline
discipline e2 potential V2; flow A1; enddiscipline
discipline e3 potential V1; flow A2; enddiscipline
discipline sig potential V1; enddiscipline
discipline cont domain continuous; enddiscipline
discipline disc domain discrete; enddiscipline
discipline none enddiscipline
module m; endmodule
)");
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;

    auto d = [&](std::string_view n) -> const DisciplineSymbol& {
        return *compilation.getDiscipline(n);
    };
    CHECK(d("e1").isCompatibleWith(d("e1")));  // self
    CHECK(d("e1").isCompatibleWith(d("e1d"))); // derived nature
    CHECK(d("e1").isCompatibleWith(d("e2")));  // same units
    CHECK(!d("e1").isCompatibleWith(d("e3"))); // flow natures incompatible
    CHECK(d("e1").isCompatibleWith(d("sig"))); // non-existent flow binding
    CHECK(d("e1").isCompatibleWith(d("cont"))); // natureless, same domain
    CHECK(!d("e1").isCompatibleWith(d("disc"))); // different domains
    CHECK(d("e1").isCompatibleWith(d("none")));  // domainless
    CHECK(d("disc").isCompatibleWith(d("none")));
}

TEST_CASE("VAMS natures are deduplicated across compilation units") {
    // V-C1: each file has its own preprocessor, so the include guard does not span them.
    auto unit = R"(
`include "disciplines.vams"
module m_@@; electrical a; endmodule
)";
    auto make = [&](std::string_view suffix, const SourceLibrary* lib = nullptr) {
        std::string text = std::string(VamsPrefix) + unit;
        text.replace(text.find("@@"), 2, suffix);
        return SyntaxTree::fromText(text, getSourceManager(), "unit", "", vamsOptions(), lib);
    };

    {
        Compilation compilation;
        compilation.addSyntaxTree(make("a"));
        compilation.addSyntaxTree(make("b"));
        NO_COMPILATION_ERRORS;
        CHECK(compilation.getDisciplines().size() == 11);
        CHECK(net(compilation, "m_a.a").discipline == net(compilation, "m_b.a").discipline);
    }
    {
        SourceLibrary libA("libA", 1), libB("libB", 2);
        Compilation compilation;
        compilation.addSyntaxTree(make("a", &libA));
        compilation.addSyntaxTree(make("b", &libB));
        NO_COMPILATION_ERRORS;
    }
    {
        Compilation compilation;
        compilation.addSyntaxTree(vamsTree(
            "nature N units = \"V\"; access = N1; abstol = 1; endnature\nmodule a; endmodule\n"));
        compilation.addSyntaxTree(vamsTree(
            "nature N units = \"V\"; access = N2; abstol = 1; endnature\nmodule b; endmodule\n"));
        auto c = codes(compilation);
        REQUIRE(c.size() == 1);
        CHECK(c[0] == diag::NatureRedefinition);
    }
}

