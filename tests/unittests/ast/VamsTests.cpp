// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//
// SVMake VAMS P0.1: Verilog-AMS (Accellera VAMS-2023) semantics.

#include "Test.h"

#include "slang/analysis/AnalysisManager.h"
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


TEST_CASE("VAMS analog blocks and contributions") {
    auto tree = vamsTree(R"(
`include "disciplines.vams"
module m(p, n);
    inout p, n;
    electrical p, n, x;
    branch (p, n) br;
    parameter real r = 1k;
    real v;
    analog begin
        v = V(p, n);
        I(p, n) <+ v / r;
        I(br) <+ potential(p, n) * 1u;
        V(x) <+ flow(br);
        if (v > 0.5) I(x) <+ 0.0;
    end
    analog initial begin
        v = 0;
    end
    analog I(x, n) <+ ddt(V(x, n)) + idt(V(x), 0) + transition(v, 0, 1n, 1n) + limexp(v);
endmodule
)");
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;

    auto& body = compilation.getRoot().lookupName<InstanceSymbol>("m").body;
    std::vector<const AnalogBlockSymbol*> blocks;
    for (auto& member : body.members()) {
        if (member.kind == SymbolKind::AnalogBlock)
            blocks.push_back(&member.as<AnalogBlockSymbol>());
    }
    REQUIRE(blocks.size() == 3);
    CHECK(!blocks[0]->isInitial);
    CHECK(blocks[1]->isInitial);

    // Walk the first block: four contributions, each to a branch access.
    struct {
        int contributions = 0, potential = 0, flow = 0, named = 0;
    } counter;
    blocks[0]->getBody().visit(makeVisitor(
        [&](auto& v, const ContributionStatement& stmt) {
            counter.contributions++;
            v.visitDefault(stmt);
        },
        [&](auto& v, const BranchAccessExpression& expr) {
            if (expr.access == BranchAccessExpression::Access::Potential)
                counter.potential++;
            else
                counter.flow++;
            if (expr.branch)
                counter.named++;
            v.visitDefault(expr);
        }));
    CHECK(counter.contributions == 4);
    CHECK(counter.potential == 3);
    CHECK(counter.flow == 4);
    CHECK(counter.named == 2);
}

TEST_CASE("VAMS access functions follow the discipline's natures") {
    auto tree = vamsTree(R"(
nature Vx units = "V"; access = Vx; abstol = 1u; endnature
nature Ix units = "A"; access = Ix; abstol = 1p; endnature
nature Va units = "V"; access = V; abstol = 1u; endnature
nature Ia units = "A"; access = I; abstol = 1p; endnature
discipline ex potential Vx; flow Ix; enddiscipline
discipline ea potential Va; flow Ia; enddiscipline
discipline \logic ; domain discrete; enddiscipline
module m;
    ex a, b;
    ea c;
    \logic d;
    real r;
    analog begin
        r = Vx(a, b) + Ix(a);
        r = V(a);
        r = V(c) + I(c);
        r = V(d);
        r = Vx(c);
    end
endmodule
module user;
    function real V(input real q); return q; endfunction
    ea c;
    real r;
    analog r = V(1.5);
endmodule
)");
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    std::vector<std::pair<DiagCode, size_t>> got;
    for (auto& d : compilation.getAllDiagnostics())
        got.push_back({d.code, compilation.getSourceManager()->getLineNumber(d.location)});
    // Lines: 17 r = V(a) (ex has Vx/Ix), 19 r = V(d) (discrete), 20 r = Vx(c) (ea has V/I).
    CHECK(got == std::vector<std::pair<DiagCode, size_t>>{{diag::AccessFunctionMismatch, 17},
                                                         {diag::BranchTerminalNoDiscipline, 19},
                                                         {diag::AccessFunctionMismatch, 20}});
}

TEST_CASE("VAMS builtin keywords start statements") {
    // V-M2: potential/flow and the other builtin keywords are possible expression starts.
    auto tree = vamsTree(R"(
`include "disciplines.vams"
module m;
    electrical a, b;
    branch (a, b) br;
    analog begin
        potential(a) <+ 1.0;
        flow(br) <+ 0.0;
    end
endmodule
)");
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;
}

TEST_CASE("VAMS contribution errors") {
    auto tree = vamsTree(R"(
`include "disciplines.vams"
module m;
    electrical a;
    real x;
    analog begin
        x <+ 1.0;
        if (x<+2) x = 1;
    end
    analog initial V(a) <+ 1.0;
    always @(x) V(a) <+ 1.0;
endmodule
)");
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    auto c = codes(compilation);
    CHECK(c == std::vector<DiagCode>{diag::ContributionTargetNotBranch,
                                     diag::ContributionOperatorInExpression,
                                     diag::ContributionInAnalogInitial,
                                     diag::ContributionOutsideAnalog});
}

TEST_CASE("VAMS math builtins evaluate in constant expressions") {
    auto tree = vamsTree(R"(
module m;
    parameter real a = log(100);
    parameter real b = ln(exp(1.0));
    parameter real c = sqrt(16) + pow(2, 10) + hypot(3, 4) + atan2(0, 1);
    parameter real d = sin(0) + cos(0) + floor(2.5) + ceil(2.5);
    parameter integer e = abs(-3) + min(4, 7) + max(4, 7);
    parameter real f = abs(-2.5) + min(1.5, 2) + ln1p(0) + expm1(0);
endmodule
)");
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;
    auto& root = compilation.getRoot();
    auto val = [&](std::string_view p) { return root.lookupName<ParameterSymbol>(p).getValue(); };
    CHECK(val("m.a").real() == 2.0);
    CHECK(val("m.b").real() == 1.0);
    CHECK(val("m.c").real() == 4.0 + 1024.0 + 5.0 + 0.0);
    CHECK(val("m.d").real() == 0.0 + 1.0 + 2.0 + 3.0);
    CHECK(val("m.e").integer() == 3 + 4 + 7);
    CHECK(val("m.f").real() == 2.5 + 1.5);

    auto bad = vamsTree("module n; parameter real p = sin(1, 2); endmodule\n");
    Compilation c2;
    c2.addSyntaxTree(bad);
    CHECK(codes(c2) == std::vector<DiagCode>{diag::TooManyArguments});
}

TEST_CASE("VAMS analog operators are analog-only") {
    auto tree = vamsTree(R"(
`include "disciplines.vams"
module m;
    electrical a;
    real x;
    parameter real p = ddt(1.0);
    always @(x) x = idt(x);
    analog initial x = transition(x);
    analog x = analysis(x);
    analog x = laplace_nd(x, {1}, {1});
endmodule
)");
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    auto c = codes(compilation);
    auto count = [&](DiagCode code) { return std::ranges::count(c, code); };
    CHECK(count(diag::AnalogOperatorOutsideAnalog) == 3);
    CHECK(count(diag::AnalysisArgNotString) == 1);
    CHECK(count(diag::VamsNotSupported) == 1);
}

TEST_CASE("VAMS $abstime and analysis()") {
    auto tree = vamsTree(R"(
`include "disciplines.vams"
module m;
    electrical a;
    real t;
    integer k;
    always @(t) t = $abstime;
    analog begin
        t = $abstime;
        k = analysis("tran", "ac");
        V(a) <+ t;
    end
endmodule
)");
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;

    auto sv = SyntaxTree::fromText("module s; real t; initial t = $abstime; endmodule\n");
    Compilation c2;
    c2.addSyntaxTree(sv);
    CHECK(codes(c2) == std::vector<DiagCode>{diag::UnknownSystemName});
}

TEST_CASE("VAMS digital-only statements in analog blocks") {
    auto tree = vamsTree(R"(
`include "disciplines.vams"
module m;
    electrical a;
    real x;
    event e;
    analog begin
        #1 x = 1;
        wait (x > 0) x = 2;
        fork x = 3; join
        x <= 4;
    end
    initial #5u x = 1;
endmodule
)");
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    auto c = codes(compilation);
    auto count = [&](DiagCode code) { return std::ranges::count(c, code); };
    CHECK(count(diag::NotAllowedInAnalog) == 4);
    CHECK(count(diag::ScaledDelay) == 1);
}

TEST_CASE("VAMS builtins as arguments and keyword builtins from SV macros") {
    auto tree = vamsTree(R"(
`include "disciplines.vams"
`define SINE sin(1.0)
module m;
    electrical a;
    real x;
    function real f(input real p, input real q); return p + q; endfunction
    analog begin
        $display(ddt(x), sin(x));
        x = f(1, sin(x)) + f(abs(x), 2);
        x = `SINE;
    end
endmodule
`end_keywords
module s;
    real y;
    initial y = `SINE;
endmodule
)");
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    CHECK(codes(compilation) == std::vector<DiagCode>{diag::UnknownSystemName});
}


TEST_CASE("VAMS analog events") {
    auto tree = vamsTree(R"(
`include "disciplines.vams"
module m;
    electrical a, gnd;
    ground gnd;
    real x;
    integer n;
    analog begin
        @(initial_step) x = 0;
        @(initial_step("tran", "dc")) x = 1;
        @(final_step) $display("done");
        @(cross(V(a) - 0.5, 1)) n = n + 1;
        @(cross(V(a), , 1e-9)) n = 0;
        @(initial_step or cross(V(a), -1)) x = 2;
        V(a) <+ x;
    end
endmodule
)");
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;

    std::vector<const AnalogEventControl*> events;
    auto& body = compilation.getRoot().lookupName<InstanceSymbol>("m").body;
    body.visit(makeVisitor([&](auto& v, const AnalogEventControl& ev) {
        events.push_back(&ev);
        v.visitDefault(ev);
    }));
    REQUIRE(events.size() == 7);
    CHECK(events[0]->eventKind == AnalogBuiltinKind::InitialStep);
    CHECK(events[0]->analyses.empty());
    CHECK(events[1]->eventKind == AnalogBuiltinKind::InitialStep);
    CHECK(std::ranges::equal(events[1]->analyses, std::vector<std::string_view>{"tran", "dc"}));
    CHECK(events[2]->eventKind == AnalogBuiltinKind::FinalStep);
    CHECK(events[3]->eventKind == AnalogBuiltinKind::Cross);
    REQUIRE(events[3]->args.size() == 2);
    CHECK(events[3]->args[0]->kind == ExpressionKind::BinaryOp);
    REQUIRE(events[4]->args.size() == 3);
    CHECK(events[4]->args[1] == nullptr);
    CHECK(events[4]->args[2] != nullptr);
    CHECK(events[5]->eventKind == AnalogBuiltinKind::InitialStep);
    CHECK(events[6]->eventKind == AnalogBuiltinKind::Cross);
}

TEST_CASE("VAMS cross() in digital code, initial_step only in analog") {
    auto tree = vamsTree(R"(
`include "disciplines.vams"
module m;
    electrical in;
    reg q = 1'b0;
    always @(cross(V(in) - 0.5, +1, 1.0e-12)) q <= ~q;
    initial @(cross(V(in))) q = 1;
    always @(initial_step) q = 0;
    always @(posedge cross(V(in))) q = 0;
    initial q = initial_step;
    analog @(cross(V(in), 2)) q = 1;
endmodule
)");
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    auto c = codes(compilation);
    CHECK(c == std::vector<DiagCode>{diag::AnalogEventInDigital, diag::AnalogEventNotInEventControl,
                                     diag::AnalogEventNotInEventControl,
                                     diag::CrossDirectionInvalid});

    auto& root = compilation.getRoot();
    auto& body = root.lookupName<InstanceSymbol>("m").body;
    int crossInAlways = 0;
    body.visit(makeVisitor([&](auto& v, const ProceduralBlockSymbol& pb) {
        if (pb.getBody().kind == StatementKind::Timed &&
            pb.getBody().as<TimedStatement>().timing.kind == TimingControlKind::AnalogEvent) {
            crossInAlways++;
        }
        v.visitDefault(pb);
    }));
    CHECK(crossInAlways == 2);
}

TEST_CASE("VAMS contributions are not allowed in event statements") {
    auto tree = vamsTree(R"(
`include "disciplines.vams"
module m;
    electrical a;
    reg clk;
    real x;
    analog begin
        @(cross(V(a))) V(a) <+ 1.0;
        @(initial_step) begin
            if (x > 0) I(a) <+ 1.0;
        end
        @(posedge clk) x = V(a);
        V(a) <+ x;
    end
    analog @(above(V(a))) x = 1;
endmodule
)");
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    auto c = codes(compilation);
    CHECK(c == std::vector<DiagCode>{diag::ContributionInEventStatement,
                                     diag::ContributionInEventStatement, diag::VamsNotSupported});
}

TEST_CASE("VAMS digital events in analog blocks are legal") {
    // V-H4: VAMS-2023 5.10.5, A.6.5 analog_event_expression.
    auto tree = vamsTree(R"(
`include "disciplines.vams"
module m;
    electrical a;
    reg clk;
    real x;
    analog begin
        @(posedge clk) x = V(a);
        @(clk or initial_step) x = 0;
        V(a) <+ x;
    end
endmodule
)");
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;
}

TEST_CASE("VAMS P0.5 fixtures compile clean") {
    // V-H6: byte-for-byte copies of svmake tests/rtl/vams/proto/*.vams, mapped to VAMS-2023.
    auto dir = findTestDir() + "vams/proto/";
    for (auto name : {"p1_rc_step.vams", "p2_diode_rect.vams", "p3_cmp_dff_rc.vams",
                      "p4_relax_osc.vams", "p5_steps_smoke.vams"}) {
        INFO(name);
        PreprocessorOptions ppo;
        ppo.additionalIncludePaths.push_back(findTestDir() + "vams");
        ppo.keywordMapping.push_back({".../*.vams", KeywordVersion::vams_2023});
        Bag bag;
        bag.set(ppo);
        auto tree = SyntaxTree::fromFile(dir + name, getSourceManager(), bag);
        REQUIRE(tree);
        Compilation compilation(bag);
        compilation.addSyntaxTree(*tree);
        NO_COMPILATION_ERRORS;

        // The analysis pass (drivers, multi-driver, unused) must accept the analog design.
        compilation.freeze();
        analysis::AnalysisManager analysis;
        analysis.analyze(compilation);
        auto adiags = filterWarnings(analysis.getDiagnostics());
        if (!adiags.empty())
            FAIL_CHECK(report(adiags));
    }
}

TEST_CASE("VAMS AST walk and serialization") {
    auto tree = vamsTree(R"(
`include "disciplines.vams"
module res(p, n);
    inout p, n;
    electrical p, n;
    parameter real r = 1k;
    analog I(p, n) <+ V(p, n) / r;
endmodule
)");
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;

    auto& body = compilation.getRoot().lookupName<InstanceSymbol>("res").body;
    const AnalogBlockSymbol* block = nullptr;
    for (auto& member : body.members()) {
        if (member.kind == SymbolKind::AnalogBlock)
            block = &member.as<AnalogBlockSymbol>();
    }
    REQUIRE(block);

    JsonWriter writer;
    ASTSerializer serializer(compilation, writer);
    serializer.setIncludeAddresses(false);
    serializer.serialize(*block);
    // L11: the golden covers the VAMS subtree only (the analog block of `res`).
    CHECK(writer.view() ==
          R"({"name":"","kind":"AnalogBlock","isInitial":false,"body":{"kind":"Contribution",)"
          R"("lhs":{"kind":"BranchAccess","type":"real","access":"flow","nature":"Current",)"
          R"("discipline":"electrical","positive":{"kind":"NamedValue","type":"logic","symbol":"p"},)"
          R"("negative":{"kind":"NamedValue","type":"logic","symbol":"n"}},"rhs":{"kind":"BinaryOp",)"
          R"("type":"real","op":"Divide","left":{"kind":"BranchAccess","type":"real",)"
          R"("access":"potential","nature":"Voltage","discipline":"electrical","positive":)"
          R"({"kind":"NamedValue","type":"logic","symbol":"p"},"negative":{"kind":"NamedValue",)"
          R"("type":"logic","symbol":"n"}},"right":{"kind":"NamedValue","type":"real","symbol":"r",)"
          R"("constant":"1000"}}}})");
}

TEST_CASE("VAMS t_vams_kwd_bad: every VAMS-2.3 keyword line is a syntax error") {
    auto path = findTestDir() + "vams/t_vams_kwd_bad.v";
    auto tree = SyntaxTree::fromFile(path, getSourceManager());
    REQUIRE(tree);
    Compilation compilation;
    compilation.addSyntaxTree(*tree);
    auto& sm = getSourceManager();

    // Expected: for each `int <word>;` line, a parse error at the word's column when the word
    // is a VAMS-2.3 keyword, otherwise (assert) an undeclared type `int` at column 3.
    auto text = sm.getSourceText((*tree)->root().getFirstToken().location().buffer());
    std::map<size_t, std::pair<size_t, bool>> expected; // line -> (column, isKeyword)
    size_t lineNo = 1, pos = 0;
    while (pos < text.size()) {
        auto end = text.find('\n', pos);
        auto line = text.substr(pos, end == std::string_view::npos ? text.size() - pos : end - pos);
        if (line.starts_with("  int ") && line.ends_with(";")) {
            auto word = line.substr(6, line.size() - 7);
            // Every word in Verilator's list is a VAMS-2.3 keyword except `assert` (an SV
            // keyword the list includes); decided here independently of the keyword tables.
            bool kw = word != "assert";
            expected[lineNo] = {kw ? 7 : 3, kw};
        }
        lineNo++;
        if (end == std::string_view::npos)
            break;
        pos = end + 1;
    }
    REQUIRE(expected.size() == 65);

    // Parse diagnostics per line (the tree's), and semantic ones (the compilation's).
    std::map<size_t, std::vector<std::pair<size_t, DiagCode>>> parse, all;
    for (auto& d : (*tree)->diagnostics())
        parse[sm.getLineNumber(d.location)].push_back({sm.getColumnNumber(d.location), d.code});
    for (auto& d : compilation.getAllDiagnostics()) {
        if (d.isError())
            all[sm.getLineNumber(d.location)].push_back({sm.getColumnNumber(d.location), d.code});
    }
    for (auto& [line, exp] : expected) {
        INFO("line " << line);
        auto& [col, kw] = exp;
        if (kw) {
            // A reserved word in a declarator position: a syntax error on that line.
            CHECK(parse.contains(line));
        }
        else {
            // `int assert;` is syntactically fine in VAMS (neither word is reserved); it fails
            // only because `int` is not a type in Verilog-AMS.
            CHECK(!parse.contains(line));
            REQUIRE(all.contains(line));
            CHECK(all[line][0] == std::pair<size_t, DiagCode>{3, diag::UndeclaredIdentifier});
        }
    }
}
