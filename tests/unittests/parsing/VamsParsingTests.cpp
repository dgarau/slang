// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//
// SVMake VAMS P0.1: Verilog-AMS (Accellera VAMS-2023) lexing and preprocessing.

#include "Test.h"

#include "slang/ast/Compilation.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/parsing/Lexer.h"
#include "slang/parsing/LexerFacts.h"
#include "slang/parsing/Parser.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"

using LF = LexerFacts;

namespace {

// Annex B, Table B.1 of VAMS-2023, transcribed independently of LexerFacts.cpp (215 words).
const char* const AnnexB[] = {
    "above", "abs", "absdelay", "absdelta", "abstol", "ac_stim", "access", "acos", "acosh",
    "aliasparam", "always", "analog", "analysis", "and", "asin", "asinh", "assign", "atan",
    "atan2", "atanh", "automatic", "begin", "branch", "break", "buf", "bufif0", "bufif1", "case",
    "casex", "casez", "ceil", "cell", "cmos", "config", "connect", "connectmodule",
    "connectrules", "continue", "continuous", "cos", "cosh", "cross", "ddt", "ddt_nature", "ddx",
    "deassign", "default", "defparam", "design", "disable", "discipline", "discrete", "domain",
    "driver_update", "edge", "else", "end", "endcase", "endconfig", "endconnectrules",
    "enddiscipline", "endfunction", "endgenerate", "endmodule", "endnature", "endparamset",
    "endprimitive", "endspecify", "endtable", "endtask", "event", "exclude", "exp", "expm1",
    "final_step", "flicker_noise", "floor", "flow", "for", "force", "forever", "fork", "from",
    "function", "generate", "genvar", "ground", "highz0", "highz1", "hypot", "idt", "idt_nature",
    "idtmod", "if", "ifnone", "incdir", "include", "inf", "initial", "initial_step", "inout",
    "input", "instance", "integer", "join", "laplace_nd", "laplace_np", "laplace_zd",
    "laplace_zp", "large", "last_crossing", "liblist", "library", "limexp", "ln", "ln1p",
    "localparam", "log", "macromodule", "max", "medium", "merged", "min", "module", "nand",
    "nature", "negedge", "nmos", "noise_table", "noise_table_log", "nor", "noshowcancelled",
    "not", "notif0", "notif1", "or", "output", "parameter", "paramset", "pmos", "posedge",
    "potential", "pow", "primitive", "pull0", "pull1", "pulldown", "pullup",
    "pulsestyle_ondetect", "pulsestyle_onevent", "rcmos", "real", "realtime", "reg", "release",
    "repeat", "resolveto", "return", "rnmos", "rpmos", "rtran", "rtranif0", "rtranif1",
    "scalared", "showcancelled", "signed", "sin", "sinh", "slew", "small", "specify",
    "specparam", "split", "sqrt", "string", "strong0", "strong1", "supply0", "supply1", "table",
    "tan", "tanh", "task", "time", "timer", "tran", "tranif0", "tranif1", "transition", "tri",
    "tri0", "tri1", "triand", "trior", "trireg", "units", "unsigned", "use", "uwire", "vectored",
    "wait", "wand", "weak0", "weak1", "while", "white_noise", "wire", "wor", "wreal", "xnor",
    "xor", "zi_nd", "zi_np", "zi_zd", "zi_zp",
};

// Annex G: keywords added after VAMS 2.3 (items 4803, 7780, 830); net_resolution was removed in 2.4.
const char* const Post23[] = {"absdelta", "expm1", "ln1p", "break", "continue", "return"};

std::vector<Token> lexAll(std::string_view text, const Bag& options = {}) {
    diagnostics.clear();
    Preprocessor preprocessor(getSourceManager(), alloc, diagnostics, options);
    preprocessor.pushSource(text);
    std::vector<Token> result;
    while (true) {
        Token t = preprocessor.next();
        if (t.kind == TokenKind::EndOfFile)
            break;
        result.push_back(t);
    }
    return result;
}

std::vector<TokenKind> kinds(const std::vector<Token>& tokens) {
    std::vector<TokenKind> result;
    for (auto& t : tokens)
        result.push_back(t.kind);
    return result;
}

std::string vams(std::string_view text) {
    return "`begin_keywords \"VAMS-2023\"\n" + std::string(text);
}

// Compiles `text` and returns the value of the parameter `path` (e.g. "m.a").
ConstantValue paramValue(Compilation& compilation, std::string_view path) {
    auto& param = compilation.getRoot().lookupName<ParameterSymbol>(path);
    return param.getValue();
}

} // namespace

TEST_CASE("VAMS keyword tables follow Annex B") {
    auto vt = LF::getKeywordTable(KeywordVersion::vams_2023);
    auto v23 = LF::getKeywordTable(KeywordVersion::vams_2_3);
    auto sv = LF::getKeywordTable(KeywordVersion::v1800_2023);
    REQUIRE(vt);
    REQUIRE(v23);

    CHECK(std::size(AnnexB) == 215);
    CHECK(vt->size() == 215);
    CHECK(v23->size() == 210);

    for (auto word : AnnexB) {
        INFO(word);
        auto it = vt->find(word);
        REQUIRE(it != vt->end());
        auto kind = it->second;
        CHECK(LF::isKeyword(kind));
        CHECK(LF::getTokenKindText(kind) == word);

        // Lexed as that keyword in VAMS source...
        auto tokens = lexAll(vams(word));
        REQUIRE(tokens.size() == 1);
        CHECK(tokens[0].kind == kind);

        // ...and as an identifier in SystemVerilog unless SV reserves it too.
        auto svTokens = lexAll(word, optionsFor(LanguageVersion::v1800_2023));
        REQUIRE(svTokens.size() == 1);
        if (sv->find(word) == sv->end())
            CHECK(svTokens[0].kind == TokenKind::Identifier);
        else
            CHECK(svTokens[0].kind == kind);

        bool post23 = std::ranges::find(Post23, std::string_view(word)) != std::end(Post23);
        CHECK((v23->find(word) != v23->end()) == !post23);
    }

    CHECK(vt->find("net_resolution") == vt->end());
    REQUIRE(v23->find("net_resolution") != v23->end());
    CHECK(LF::isKeyword(v23->find("net_resolution")->second));

    CHECK(LF::getKeywordVersion("VAMS-2023") == KeywordVersion::vams_2023);
    CHECK(LF::getKeywordVersion("VAMS-2.3") == KeywordVersion::vams_2_3);
    CHECK(LF::isVamsKeywordVersion(KeywordVersion::vams_2_3));
    CHECK(!LF::isVamsKeywordVersion(KeywordVersion::v1364_2005));

    // The extra words are reserved in VAMS-2.3 only where Annex G says so.
    auto t23 = lexAll("`begin_keywords \"VAMS-2.3\"\nabsdelta net_resolution ddt");
    REQUIRE(t23.size() == 3);
    CHECK(t23[0].kind == TokenKind::Identifier);
    CHECK(t23[1].kind == TokenKind::NetResolutionKeyword);
    CHECK(t23[2].kind == TokenKind::DdtKeyword);
    CHECK(lexAll(vams("net_resolution"))[0].kind == TokenKind::Identifier);
    CHECK(lexAll(vams("<+"))[0].kind == TokenKind::LessThanPlus);
    CHECK(LF::getTokenKindText(TokenKind::LessThanPlus) == "<+");
}

TEST_CASE("VAMS keywords stay identifiers in SystemVerilog") {
    auto tree = SyntaxTree::fromText(R"(
`define M(from, flow) (from + flow)
`define sin 3
module m;
    logic sin, abs, from, flow, inf, domain, ground, branch, analog, ddt, potential;
    int ln = `M(1, 2) + `sin;
    function int access(int units); return units; endfunction
    initial ln = access(ln);
endmodule
)");
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;

    // `cross` is an SV keyword (covergroups); in an expression it is an SV syntax error, and
    // must stay exactly that error (the VAMS cross() event function must not leak into SV).
    auto tree2 = SyntaxTree::fromText(R"(
module n;
    logic x, a;
    initial x = cross(a);
endmodule
)");
    auto& diags = tree2->diagnostics();
    REQUIRE(!diags.empty());
    CHECK(diags[0].code == diag::ExpectedExpression);
}

TEST_CASE("VAMS keyword names for macros and macro formals") {
    auto tokens = lexAll(vams(R"(
`define abs(x) (x)
`define M(from) from+1
`abs(3) `M(2)
)"));
    CHECK_DIAGNOSTICS_EMPTY;
    auto k = kinds(tokens);
    CHECK(k == std::vector<TokenKind>{TokenKind::OpenParenthesis, TokenKind::IntegerLiteral,
                                      TokenKind::CloseParenthesis, TokenKind::IntegerLiteral,
                                      TokenKind::Plus, TokenKind::IntegerLiteral});
}

TEST_CASE("VAMS contribution token forms only in VAMS source") {
    auto v = lexAll(vams("a<+b"));
    CHECK(kinds(v) == std::vector<TokenKind>{TokenKind::Identifier, TokenKind::LessThanPlus,
                                             TokenKind::Identifier});

    for (auto lv : {LanguageVersion::v1800_2023, LanguageVersion::v1364_2005}) {
        auto s = lexAll("a<+b", optionsFor(lv));
        CHECK(kinds(s) == std::vector<TokenKind>{TokenKind::Identifier, TokenKind::LessThan,
                                                 TokenKind::Plus, TokenKind::Identifier});
    }
    auto t1364 = lexAll("`begin_keywords \"1364-2005\"\na<+b");
    CHECK(t1364[1].kind == TokenKind::LessThan);

    auto tree = SyntaxTree::fromText("module m; localparam p = 2<+3; endmodule");
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;
    CHECK(paramValue(compilation, "m.p").integer() == 1);

    // Macro bodies keep the tokens of the mode they were defined in, except where slang's
    // implicit concatenation of adjacent body tokens re-lexes them (`1k`): that re-lex runs
    // in the mode of the expansion (V-M1), so `1k` defined in SV expands to 1e3 in VAMS.
    auto svDef = lexAll("`define LP a<+b ddt 1k\n`begin_keywords \"VAMS-2023\"\n`LP");
    CHECK(kinds(svDef) ==
          std::vector<TokenKind>{TokenKind::Identifier, TokenKind::LessThan, TokenKind::Plus,
                                 TokenKind::Identifier, TokenKind::Identifier,
                                 TokenKind::RealLiteral});
    auto vDef = lexAll(vams("`define LP a<+b ddt 1k\n`end_keywords\n`LP"));
    CHECK(kinds(vDef) == std::vector<TokenKind>{TokenKind::Identifier, TokenKind::LessThanPlus,
                                                TokenKind::Identifier, TokenKind::DdtKeyword,
                                                TokenKind::RealLiteral});
}

TEST_CASE("VAMS scale factors") {
    auto value = [](std::string_view text) {
        auto tokens = lexAll(vams(text));
        REQUIRE(tokens.size() == 1);
        CHECK(tokens[0].kind == TokenKind::RealLiteral);
        CHECK(tokens[0].rawText() == text);
        return tokens[0].realValue();
    };

    CHECK(value("10n") == 10e-9);
    CHECK(value("1k") == 1e3);
    CHECK(value("7K") == 7e3);
    CHECK(value("2.5u") == 2.5e-6);
    CHECK(value("1M") == 1e6);
    CHECK(value("1m") == 1e-3);
    CHECK(value("3T") == 3e12);
    CHECK(value("5G") == 5e9);
    CHECK(value("4p") == 4e-12);
    CHECK(value("6f") == 6e-15);
    CHECK(value("9a") == 9e-18);
    CHECK(value("1.5k") == 1.5e3);
    CHECK(value("1_000k") == 1e6);
    CHECK(value("1.3u") == 1.3e-6);

    // Not a scale factor: identifier character follows, or after an exponent.
    CHECK(kinds(lexAll(vams("1kx"))) ==
          std::vector<TokenKind>{TokenKind::IntegerLiteral, TokenKind::Identifier});
    CHECK(kinds(lexAll(vams("1e3k"))) ==
          std::vector<TokenKind>{TokenKind::RealLiteral, TokenKind::Identifier});

    // Time literals win over scale factors, in VAMS source too.
    for (auto text : {"1ns", "1ms", "1fs", "1us", "1ps", "1s"}) {
        auto t = lexAll(vams(text));
        REQUIRE(t.size() == 1);
        CHECK(t[0].kind == TokenKind::TimeLiteral);
    }
    lexAll(vams("`timescale 1ns/1ps\n"));
    CHECK_DIAGNOSTICS_EMPTY;

    // SystemVerilog is unchanged.
    auto sv = optionsFor(LanguageVersion::v1800_2023);
    CHECK(kinds(lexAll("1n", sv)) ==
          std::vector<TokenKind>{TokenKind::IntegerLiteral, TokenKind::Identifier});
    CHECK(kinds(lexAll("2.5u", sv)) ==
          std::vector<TokenKind>{TokenKind::RealLiteral, TokenKind::Identifier});
    CHECK(kinds(lexAll("1k 7K 1m", sv)) ==
          std::vector<TokenKind>{TokenKind::IntegerLiteral, TokenKind::Identifier,
                                 TokenKind::IntegerLiteral, TokenKind::Identifier,
                                 TokenKind::IntegerLiteral, TokenKind::Identifier});
    CHECK(kinds(lexAll("1ns 1s", sv)) ==
          std::vector<TokenKind>{TokenKind::TimeLiteral, TokenKind::TimeLiteral});
    CHECK(kinds(lexAll("1step", sv)) == std::vector<TokenKind>{TokenKind::OneStep});

    // Hex digits that look like scale factors still form based literals.
    auto tree = SyntaxTree::fromText(vams(R"(
module m;
    localparam a = 8'h3a;
    localparam b = 8'h1f;
    localparam c = 'hfa;
    localparam d = 'h3ab;
    localparam e = 'h1f_0;
    localparam f = 2.5u;
    localparam g = 1k + 1;
endmodule
)"));
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;
    CHECK(paramValue(compilation, "m.a").integer() == 0x3a);
    CHECK(paramValue(compilation, "m.b").integer() == 0x1f);
    CHECK(paramValue(compilation, "m.c").integer() == 0xfa);
    CHECK(paramValue(compilation, "m.d").integer() == 0x3ab);
    CHECK(paramValue(compilation, "m.e").integer() == 0x1f0);
    CHECK(paramValue(compilation, "m.f").real() == 2.5e-6);
    CHECK(paramValue(compilation, "m.g").real() == 1001.0);
}

TEST_CASE("VAMS __VAMS_ENABLE__ is defined only in VAMS source") {
    auto text = R"(
`ifdef __VAMS_ENABLE__
    localparam del = 2;
`else
    localparam del = 1;
`endif
)";
    auto body = [&](std::string_view prefix) {
        return std::string(prefix) + "module not_gate;" + text + "endmodule\n";
    };
    auto check = [&](const std::string& source, int expected) {
        auto tree = SyntaxTree::fromText(source);
        Compilation compilation;
        compilation.addSyntaxTree(tree);
        NO_COMPILATION_ERRORS;
        CHECK(paramValue(compilation, "not_gate.del").integer() == expected);
    };
    check(body("`begin_keywords \"VAMS-2023\"\n"), 2);
    check(body("`begin_keywords \"VAMS-2.3\"\n"), 2);
    check(body(""), 1);
    check(body("`begin_keywords \"VAMS-2023\"\n`end_keywords\n"), 1);
    check(body("`begin_keywords \"1364-2005\"\n"), 1);

    // It expands to 1.
    auto tokens = lexAll(vams("`__VAMS_ENABLE__"));
    REQUIRE(tokens.size() == 1);
    CHECK(tokens[0].kind == TokenKind::IntegerLiteral);
    CHECK(tokens[0].intValue() == 1);

    // isDefined follows the state; the compact-modeling macro is never defined.
    diagnostics.clear();
    {
        Preprocessor pp(getSourceManager(), alloc, diagnostics);
        pp.pushSource(vams("x"));
        CHECK(pp.next().kind == TokenKind::Identifier);
        CHECK(pp.isVamsSource());
        CHECK(pp.isDefined("__VAMS_ENABLE__"));
        CHECK(!pp.isDefined("__VAMS_COMPACT_MODELING__"));
    }
    {
        Preprocessor pp(getSourceManager(), alloc, diagnostics);
        pp.pushSource("x");
        pp.next();
        CHECK(!pp.isVamsSource());
        CHECK(!pp.isDefined("__VAMS_ENABLE__"));
    }

    // A user definition is an ordinary macro in every mode.
    PreprocessorOptions ppo;
    ppo.predefines.push_back("__VAMS_ENABLE__=5");
    Bag bag;
    bag.set(ppo);
    auto user = lexAll("`__VAMS_ENABLE__", bag);
    REQUIRE(user.size() == 1);
    CHECK(user[0].intValue() == 5);
    auto userV = lexAll(vams("`__VAMS_ENABLE__"), bag);
    REQUIRE(userV.size() == 1);
    CHECK(userV[0].intValue() == 5);
}

TEST_CASE("VAMS source survives a nested non-VAMS begin_keywords (10.6 m2)") {
    auto text = R"(
`begin_keywords "VAMS-2023"
`begin_keywords "1364-2005"
module m2 (sin);
    input sin;
`ifdef __VAMS_ENABLE__
    localparam v = 1k;
`else
    localparam v = 0;
`endif
    wire analog;
endmodule
`end_keywords
`end_keywords
)";
    auto tree = SyntaxTree::fromText(text);
    Compilation compilation;
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;
    CHECK(paramValue(compilation, "m2.v").real() == 1000.0);

    auto tokens = lexAll("`begin_keywords \"VAMS-2023\"\n`begin_keywords \"1364-2005\"\na<+b analog");
    CHECK(kinds(tokens) == std::vector<TokenKind>{TokenKind::Identifier, TokenKind::LessThanPlus,
                                                  TokenKind::Identifier, TokenKind::Identifier});

    // Under the VAMS keywords themselves, sin is reserved (the LRM's second m2 example).
    auto bad = SyntaxTree::fromText(vams("module m2 (sin); input sin; endmodule\n"));
    CHECK(!bad->diagnostics().empty());
}

TEST_CASE("VAMS token pasting re-lexes in the current mode") {
    auto tokens = lexAll(vams(R"(
`define C(a,b) a``b
`C(1,k) `C(d,dt) `C(<,+)
)"));
    CHECK(kinds(tokens) == std::vector<TokenKind>{TokenKind::RealLiteral, TokenKind::DdtKeyword,
                                                  TokenKind::LessThanPlus});
    CHECK(tokens[0].realValue() == 1e3);

    auto sv = lexAll("`define C(a,b) a``b\n`C(d,dt)\n");
    CHECK(kinds(sv) == std::vector<TokenKind>{TokenKind::Identifier});
}

TEST_CASE("VAMS mapping: includes inherit VAMS source, buffers pop their own entry") {
    auto& sm = getSourceManager();
    sm.assignText("vams_h1_body.vh", R"(
`ifdef __VAMS_ENABLE__
in_vams
`else
in_sv
`endif
)");
    sm.assignText("vams_m3_x.vh", "`begin_keywords \"1364-2005\"\nanalog\n");

    PreprocessorOptions ppo;
    ppo.keywordMapping.push_back({"*.vams", KeywordVersion::vams_2023});
    Bag bag;
    bag.set(ppo);

    diagnostics.clear();
    {
        // V-H1: an unmapped include of VAMS source is VAMS source.
        Preprocessor pp(sm, alloc, diagnostics, bag);
        pp.pushSource(sm.assignText("vams_h1_top.vams", "`include \"vams_h1_body.vh\"\nddt\n"));
        auto t1 = pp.next();
        CHECK(t1.valueText() == "in_vams");
        CHECK(pp.next().kind == TokenKind::DdtKeyword);
    }
    {
        // From SystemVerilog the same include is SystemVerilog.
        Preprocessor pp(sm, alloc, diagnostics, bag);
        pp.pushSource(sm.assignText("vams_h1_top2.sv", "`include \"vams_h1_body.vh\"\nddt\n"));
        CHECK(pp.next().valueText() == "in_sv");
        CHECK(pp.next().kind == TokenKind::Identifier);
    }
    {
        // V-M3: the unbalanced begin_keywords in x.vh stays in force after the include (10.6),
        // the include's own entry is gone, and once the mapped file ends nothing VAMS leaks
        // into the next unmapped file.
        Preprocessor pp(sm, alloc, diagnostics, bag);
        std::vector<Token> tokens;
        auto drain = [&] {
            while (true) {
                auto t = pp.next();
                if (t.kind == TokenKind::EndOfFile)
                    break;
                tokens.push_back(t);
            }
        };
        pp.pushSource(sm.assignText("vams_m3_top.vams", "`include \"vams_m3_x.vh\"\nanalog <+\n"));
        drain();
        pp.pushSource(sm.assignText("vams_m3_next.sv", "a<+b\n"));
        drain();
        CHECK(kinds(tokens) == std::vector<TokenKind>{TokenKind::Identifier, TokenKind::Identifier,
                                                      TokenKind::LessThanPlus,
                                                      TokenKind::Identifier, TokenKind::LessThan,
                                                      TokenKind::Plus, TokenKind::Identifier});
        CHECK(!pp.isVamsSource());
    }
    CHECK_DIAGNOSTICS_EMPTY;
}

TEST_CASE("VAMS scaled reals in UDP tables") {
    auto udp = R"(
primitive dff (q, d, clk);
    output q; reg q;
    input d, clk;
    table
        0r : ? : 0;
        1r : ? : 1;
        1p : ? : 1;
        0f : ? : -;
        0n : ? : -;
    endtable
endprimitive
)";
    for (auto prefix : {"", "`begin_keywords \"VAMS-2023\"\n"}) {
        auto tree = SyntaxTree::fromText(std::string(prefix) + udp);
        Compilation compilation;
        compilation.addSyntaxTree(tree);
        // The table is deliberately partial (-Wudp-coverage); only errors matter here.
        auto errors = filterWarnings(compilation.getAllDiagnostics());
        if (!errors.empty())
            FAIL_CHECK(report(errors));
    }
}
