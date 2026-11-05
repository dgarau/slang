// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT

#include "Test.h"
#include <fstream>

#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/driver/Driver.h"
#include "slang/util/OS.h"
#include "slang/util/ScopeGuard.h"
#include "slang/util/String.h"

using namespace slang::driver;

TEST_CASE("Duplicate modules in different source libraries") {
    auto lib1 = std::make_unique<SourceLibrary>("lib1", 1);
    auto lib2 = std::make_unique<SourceLibrary>("lib2", 2);

    auto tree1 = SyntaxTree::fromText(R"(
module mod;
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib1.get());
    auto tree2 = SyntaxTree::fromText(R"(
module mod;
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib2.get());
    auto tree3 = SyntaxTree::fromText(R"(
module top;
    mod m();
endmodule
)");

    Compilation compilation;
    compilation.addSyntaxTree(tree1);
    compilation.addSyntaxTree(tree2);
    compilation.addSyntaxTree(tree3);
    NO_COMPILATION_ERRORS;

    auto& lib =
        compilation.getRoot().lookupName<InstanceSymbol>("top.m").getDefinition().sourceLibrary;
    CHECK(&lib == lib1.get());
}

TEST_CASE("Driver library default ordering") {
    auto guard = OS::captureOutput();

    Driver driver;
    driver.addStandardArgs();

    auto testDir = findTestDir();
    auto args = fmt::format("testfoo --libmap \"{0}libtest/testlib.map\" \"{0}libtest/top.sv\"",
                            testDir);
    CHECK(driver.parseCommandLine(args));
    CHECK(driver.processOptions());
    CHECK(driver.parseAllSources());

    auto compilation = driver.createCompilation();
    driver.reportCompilation(*compilation, false);
    CHECK(driver.reportDiagnostics(false));

    auto& m = compilation->getRoot().lookupName<InstanceSymbol>("top.m");
    CHECK(m.getDefinition().sourceLibrary.name == "lib1");
}

TEST_CASE("Driver library explicit ordering") {
    auto guard = OS::captureOutput();

    Driver driver;
    driver.addStandardArgs();

    auto testDir = findTestDir();
    auto args = fmt::format(
        "testfoo --libmap \"{0}libtest/testlib.map\" \"{0}libtest/top.sv\" -Llib2,lib1", testDir);
    CHECK(driver.parseCommandLine(args));
    CHECK(driver.processOptions());
    CHECK(driver.parseAllSources());

    auto compilation = driver.createCompilation();
    driver.reportCompilation(*compilation, false);
    CHECK(driver.reportDiagnostics(false));

    auto& m = compilation->getRoot().lookupName<InstanceSymbol>("top.m");
    CHECK(m.getDefinition().sourceLibrary.name == "lib2");
}

TEST_CASE("Top module in a library") {
    auto lib1 = std::make_unique<SourceLibrary>("lib1", 1);

    auto tree1 = SyntaxTree::fromText(R"(
module mod;
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib1.get());
    auto tree2 = SyntaxTree::fromText(R"(
module top;
endmodule
)");

    CompilationOptions options;
    options.topModules.emplace("lib1.mod");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree1);
    compilation.addSyntaxTree(tree2);
    NO_COMPILATION_ERRORS;

    auto topInstances = compilation.getRoot().topInstances;
    CHECK(topInstances.size() == 1);
    CHECK(topInstances[0]->name == "mod");
}

TEST_CASE("Config block top modules") {
    auto tree = SyntaxTree::fromText(R"(
config cfg1;
    localparam int i = 1;
    design frob;
endconfig

module frob;
endmodule

module bar;
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;

    auto topInstances = compilation.getRoot().topInstances;
    CHECK(topInstances.size() == 1);
    CHECK(topInstances[0]->name == "frob");
}

TEST_CASE("Config in library, disambiguate with module name") {
    auto lib1 = std::make_unique<SourceLibrary>("lib1", 1);
    auto lib2 = std::make_unique<SourceLibrary>("lib2", 2);

    auto tree1 = SyntaxTree::fromText(R"(
module mod;
endmodule

config cfg;
    design mod;
endconfig
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib1.get());
    auto tree2 = SyntaxTree::fromText(R"(
module mod;
endmodule

module cfg;
endmodule

config cfg;
    design mod;
endconfig
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib2.get());
    auto tree3 = SyntaxTree::fromText(R"(
module mod;
endmodule

config cfg;
    design mod;
endconfig
)");

    CompilationOptions options;
    options.topModules.emplace("lib2.cfg:config");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree1);
    compilation.addSyntaxTree(tree2);
    compilation.addSyntaxTree(tree3);
    NO_COMPILATION_ERRORS;

    auto top = compilation.getRoot().topInstances[0];
    CHECK(top->name == "mod");
    CHECK(top->getDefinition().sourceLibrary.name == "lib2");
}

TEST_CASE("Config that targets library cell") {
    auto lib1 = std::make_unique<SourceLibrary>("lib1", 1);

    auto tree1 = SyntaxTree::fromText(R"(
module mod;
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib1.get());
    auto tree2 = SyntaxTree::fromText(R"(
config cfg;
    design lib1.mod;
endconfig
)");

    CompilationOptions options;
    options.topModules.emplace("cfg");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree1);
    compilation.addSyntaxTree(tree2);
    NO_COMPILATION_ERRORS;
}

TEST_CASE("Config block error missing module") {
    auto tree = SyntaxTree::fromText(R"(
config cfg1;
    design frob libfoo.bar;
endconfig
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);

    auto& diags = compilation.getAllDiagnostics();
    REQUIRE(diags.size() == 2);
    CHECK(diags[0].code == diag::InvalidTopModule);
    CHECK(diags[1].code == diag::InvalidTopModule);
}

TEST_CASE("Config default liblist") {
    auto lib1 = std::make_unique<SourceLibrary>("lib1", 1);

    auto tree1 = SyntaxTree::fromText(R"(
module mod;
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib1.get());
    auto tree2 = SyntaxTree::fromText(R"(
module mod;
endmodule

module top;
    mod m1();
endmodule

config cfg;
    design top;
    default liblist lib1;
endconfig
)");

    CompilationOptions options;
    options.topModules.emplace("cfg");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree1);
    compilation.addSyntaxTree(tree2);
    NO_COMPILATION_ERRORS;

    auto& m1 = compilation.getRoot().lookupName<InstanceSymbol>("top.m1");
    auto& lib = m1.getDefinition().sourceLibrary;
    CHECK(lib.name == "lib1");
}

TEST_CASE("Config cell overrides") {
    auto lib1 = std::make_unique<SourceLibrary>("lib1", 1);

    auto tree1 = SyntaxTree::fromText(R"(
module mod;
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib1.get());
    auto tree2 = SyntaxTree::fromText(R"(
module mmm;
endmodule

module nnn;
endmodule

module top;
    mod m1();
    foo f1();
    bar b1();
    nnn n1();
endmodule

config cfg;
    design top;
    cell mod liblist lib1;
    cell foo use mmm;
    cell bar use lib1.mod;
endconfig
)");

    CompilationOptions options;
    options.topModules.emplace("cfg");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree1);
    compilation.addSyntaxTree(tree2);
    NO_COMPILATION_ERRORS;
}

TEST_CASE("Config instance overrides") {
    auto tree = SyntaxTree::fromText(R"(
config cfg1;
    design top;
    instance top.b.f2 use bar;
endconfig

module foo;
endmodule

module bar;
endmodule

module baz;
    foo f1(), f2();
endmodule

module top;
    baz b();
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;

    auto& inst = compilation.getRoot().lookupName<InstanceSymbol>("top.b.f2");
    CHECK(inst.getDefinition().name == "bar");
}

TEST_CASE("Config instance override errors") {
    auto lib1 = std::make_unique<SourceLibrary>("lib1", 1);

    auto tree1 = SyntaxTree::fromText(R"(
module mod;
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib1.get());

    auto tree2 = SyntaxTree::fromText(R"(
config cfg1;
    design top;
    instance top.b.f2 use bar;
    instance top.b.f3 use somelib.foo;
    instance top.b.f4 use lib1.mod;
    instance top.b.f5 use lib1.foo;
    instance top.b.f6 liblist lib1;
    instance top.i.p use foo;
endconfig

module foo;
endmodule

module baz;
    foo f1(), f2();
    foo f3(), f4(), f5(), f6();
endmodule

module top;
    baz b();
    I i();
endmodule

program prog;
endprogram

interface I;
    prog p(), q();
endinterface
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree1);
    compilation.addSyntaxTree(tree2);

    auto& diags = compilation.getAllDiagnostics();
    REQUIRE(diags.size() == 5);
    CHECK(diags[0].code == diag::UnknownModule);
    CHECK(diags[1].code == diag::UnknownLibrary);
    CHECK(diags[2].code == diag::UnknownModule);
    CHECK(diags[3].code == diag::UnknownModule);
    CHECK(diags[4].code == diag::InvalidInstanceForParent);
}

TEST_CASE("Config inherited liblist") {
    auto lib1 = std::make_unique<SourceLibrary>("lib1", 1);
    auto lib2 = std::make_unique<SourceLibrary>("lib2", 2);

    auto tree1 = SyntaxTree::fromText(R"(
module mod;
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib1.get());

    auto tree2 = SyntaxTree::fromText(R"(
module baz;
    mod m();
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib2.get());

    auto tree3 = SyntaxTree::fromText(R"(
config cfg1;
    design top;
    instance top.b liblist lib1 lib2;
endconfig

module top;
    baz b();
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree1);
    compilation.addSyntaxTree(tree2);
    compilation.addSyntaxTree(tree3);
    NO_COMPILATION_ERRORS;
}

TEST_CASE("Config hierarchical config target") {
    auto lib1 = std::make_unique<SourceLibrary>("lib1", 1);

    auto tree1 = SyntaxTree::fromText(R"(
module qq;
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib1.get());

    auto tree2 = SyntaxTree::fromText(R"(
config cfg1;
    design top;
    instance top.b use cfg2;
endconfig

config cfg2;
    design baz;
    instance baz.f1 use mod;
    instance baz.f1 liblist lib1;
endconfig

module mod;
    qq q1();
endmodule

module baz;
    foo f1();
endmodule

module top;
    baz b();
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree2);
    compilation.addSyntaxTree(tree1);
    NO_COMPILATION_ERRORS;
}

TEST_CASE("Config instance paths with two roots") {
    auto tree = SyntaxTree::fromText(R"(
config cfg1;
    design foo bar;
    instance foo.a use m1;
    instance bar.a use m2;
endconfig

module m1;
endmodule

module m2;
endmodule

module foo;
    some_mod a();
endmodule

module bar;
    some_mod a();
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;

    auto& root = compilation.getRoot();
    auto& fooA = root.lookupName<InstanceSymbol>("foo.a");
    CHECK(fooA.getDefinition().name == "m1");

    auto& barA = root.lookupName<InstanceSymbol>("bar.a");
    CHECK(barA.getDefinition().name == "m2");
}

TEST_CASE("Config warning / error cases") {
    auto tree = SyntaxTree::fromText(R"(
config cfg1;
    design top top;
    default liblist foo;
    cell b use c;
    cell b use d;
    instance top.foo use lib1.c;
    instance top.foo use d;
    instance foo.bar use e;
    instance top.blah1 use qq;
    instance top.blah2 use rr;
    instance top.blah2 use #(.A(1));
    cell foo.bar use baz;
endconfig

module a; endmodule
module b; endmodule

config qq;
    design a b;
endconfig

config rr;
    design a;
endconfig

module top;
    asdf blah1();
    asdf blah2();
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);

    auto& diags = compilation.getAllDiagnostics();
    REQUIRE(diags.size() == 10);
    CHECK(diags[0].code == diag::ConfigDupTop);
    CHECK(diags[1].code == diag::WarnUnknownLibrary);
    CHECK(diags[2].code == diag::UnusedConfigCell);
    CHECK(diags[3].code == diag::DupConfigRule);
    CHECK(diags[4].code == diag::UnusedConfigInstance);
    CHECK(diags[5].code == diag::DupConfigRule);
    CHECK(diags[6].code == diag::ConfigInstanceWrongTop);
    CHECK(diags[7].code == diag::ConfigParamsIgnored);
    CHECK(diags[8].code == diag::WarnUnknownLibrary);
    CHECK(diags[9].code == diag::NestedConfigMultipleTops);
}

TEST_CASE("Config rules with param overrides") {
    auto tree = SyntaxTree::fromText(R"(
module adder #(parameter ID = "id", W = 8, D = 512)();
    initial $display("ID = %s, W = %d, D = %d", ID, W, D);
endmodule : adder

module top;
    parameter WIDTH = 16;
    adder a1();
endmodule

config cfg1;
    design work.top;
    instance top use #(.WIDTH(32));
    instance top.a1 use #(.W(top.WIDTH));
endconfig

module top4 ();
    parameter S = 16;
    adder #(.ID("a1")) a1();
    adder #(.ID("a2")) a2();
    adder #(.ID("a3")) a3();
    adder #(.ID("a4")) a4();
endmodule

config cfg2;
    localparam S = 24;
    design top4;
    instance top4.a1 use #(.W(top4.S));
    instance top4.a2 use #(.W(S));
endconfig

module top5 ();
    parameter WIDTH = 64, DEPTH = 1024, ID = "A1";
    adder #(.ID(ID), .W(WIDTH), .D(DEPTH)) a1();
endmodule

config cfg3;
    design top5;
    instance top5.a1 use #(.W());
endconfig

module top6 ();
    adder #(.W(64), .D(1024)) a1();
endmodule

config cfg4;
    design top6;
    instance top6.a1 use #();
endconfig

module test;
    top8 t();
    defparam t.WIDTH = 64;
    defparam t.a1.W = 16;
endmodule

module top8 ();
    parameter WIDTH = 32;
    adder #(.ID("a1")) a1();
    adder #(.ID("a2"), .W(WIDTH)) a2();
endmodule

config cfg6;
    design test;
    instance test.t use #(.WIDTH(48));
endconfig
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");
    options.topModules.emplace("cfg2");
    options.topModules.emplace("cfg3");
    options.topModules.emplace("cfg4");
    options.topModules.emplace("cfg6");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;

    auto getParam = [&](std::string_view name) {
        auto& param = compilation.getRoot().lookupName<ParameterSymbol>(name);
        return param.getValue();
    };

    CHECK(getParam("top.a1.W").integer() == 32);
    CHECK(getParam("top.a1.D").integer() == 512);
    CHECK(getParam("top4.a1.W").integer() == 16);
    CHECK(getParam("top4.a2.W").integer() == 24);
    CHECK(getParam("top5.a1.W").integer() == 8);
    CHECK(getParam("top5.a1.D").integer() == 1024);
    CHECK(getParam("top6.a1.W").integer() == 8);
    CHECK(getParam("top6.a1.D").integer() == 512);
    CHECK(getParam("test.t.a1.W").integer() == 16);
    CHECK(getParam("test.t.a2.W").integer() == 48);
}

TEST_CASE("Config cell overrides with param assignments") {
    auto tree = SyntaxTree::fromText(R"(
config cfg1;
    design top;
    cell top use #(.A(1));
    cell f use #(.B(2));
endconfig

module f #(parameter int B);
endmodule

module top #(parameter int A);
    f foo();
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;

    auto getParam = [&](std::string_view name) {
        auto& param = compilation.getRoot().lookupName<ParameterSymbol>(name);
        return param.getValue();
    };

    CHECK(getParam("top.A").integer() == 1);
    CHECK(getParam("top.foo.B").integer() == 2);
}

TEST_CASE("Config param overrides with nested config path") {
    auto tree = SyntaxTree::fromText(R"(
config cfg1;
    design top;
    cell f use cfg2 : config;
endconfig

config cfg2;
    design f;
    cell f use #(.B(5));
endconfig

module f #(parameter int B);
endmodule

module top;
    f foo();
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;

    auto getParam = [&](std::string_view name) {
        auto& param = compilation.getRoot().lookupName<ParameterSymbol>(name);
        return param.getValue();
    };

    CHECK(getParam("top.foo.B").integer() == 5);
}

TEST_CASE("Config cell overrides with specific target lib") {
    auto lib1 = std::make_unique<SourceLibrary>("lib1", 1);

    auto tree1 = SyntaxTree::fromText(R"(
module qq;
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib1.get());

    auto tree2 = SyntaxTree::fromText(R"(
config cfg1;
    design top;
    cell lib1.f use #(.A(4));
    cell lib1.f use baz;
    cell top liblist lib1;
endconfig

module baz #(parameter int A);
endmodule

module top;
    f foo();
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree2);
    compilation.addSyntaxTree(tree1);
    NO_COMPILATION_ERRORS;

    auto getParam = [&](std::string_view name) {
        auto& param = compilation.getRoot().lookupName<ParameterSymbol>(name);
        return param.getValue();
    };

    CHECK(getParam("top.foo.A").integer() == 4);
}

TEST_CASE("Config override top cell with specific target lib") {
    auto lib1 = std::make_unique<SourceLibrary>("lib1", 1);

    auto tree1 = SyntaxTree::fromText(R"(
module qq #(parameter int A);
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib1.get());

    auto tree2 = SyntaxTree::fromText(R"(
config cfg1;
    design lib1.qq;
    cell lib1.qq use #(.A(4));
    cell qq use #(.A(5));
endconfig
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree2);
    compilation.addSyntaxTree(tree1);

    auto& diags = compilation.getAllDiagnostics();
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == diag::UnusedConfigCell);

    auto getParam = [&](std::string_view name) {
        auto& param = compilation.getRoot().lookupName<ParameterSymbol>(name);
        return param.getValue();
    };

    CHECK(getParam("qq.A").integer() == 4);
}

TEST_CASE("Config invalid top cell override rule") {
    auto tree = SyntaxTree::fromText(R"(
config cfg1;
    design top;
    instance top use foo;
endconfig

config cfg2;
    design top2;
    cell top2 use foo;
endconfig

module top;
endmodule

module top2;
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");
    options.topModules.emplace("cfg2");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);

    auto& diags = compilation.getAllDiagnostics();
    REQUIRE(diags.size() == 2);
    CHECK(diags[0].code == diag::ConfigOverrideTop);
    CHECK(diags[1].code == diag::ConfigOverrideTop);
}

TEST_CASE("Duplicate config blocks") {
    auto tree = SyntaxTree::fromText(R"(
config cfg1;
    design top;
    instance top use foo;
endconfig

config cfg1;
    design top;
endconfig
)");

    Compilation compilation;
    compilation.addSyntaxTree(tree);

    auto& diags = compilation.getAllDiagnostics();
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == diag::Redefinition);
}

TEST_CASE("Config instance path underneath a hierarchical config error") {
    auto tree = SyntaxTree::fromText(R"(
config cfg1;
    design top;
    cell f use cfg2 : config;
    instance top.foo.q use l;
endconfig

config cfg2;
    design bar;
endconfig

module bar;
endmodule

module top;
    f foo();
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);

    auto& diags = compilation.getAllDiagnostics();
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == diag::ConfigInstanceUnderOtherConfig);
}

TEST_CASE("Multiple tops with same name error") {
    auto tree = SyntaxTree::fromText(R"(
config cfg1;
    design top;
endconfig

config cfg2;
    design top;
endconfig

module top;
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");
    options.topModules.emplace("cfg2");
    options.topModules.emplace("top");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);

    auto& diags = compilation.getAllDiagnostics();
    REQUIRE(diags.size() == 2);
    CHECK(diags[0].code == diag::MultipleTopDupName);
    CHECK(diags[1].code == diag::MultipleTopDupName);
}

TEST_CASE("Displaying library binding info") {
    auto lib1 = std::make_unique<SourceLibrary>("lib1", 1);

    auto tree1 = SyntaxTree::fromText(R"(
module foo;
    localparam string S = $sformatf("%L");
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib1.get());

    auto tree2 = SyntaxTree::fromText(R"(
config cfg1;
    design top;
    instance top.foo use lib1.foo;
endconfig

module top;
    f foo();
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree1);
    compilation.addSyntaxTree(tree2);
    NO_COMPILATION_ERRORS;

    auto& param = compilation.getRoot().lookupName<ParameterSymbol>("top.foo.S");
    CHECK(param.getValue().str() == "lib1.foo");
}

TEST_CASE("Config params target primitives") {
    auto tree = SyntaxTree::fromText(R"(
config cfg1;
    design top;
    instance top.foo use p1;
    cell bar use p2;
    cell bar use #(.A(1));
    instance top.foo use #(.B(1));
endconfig

primitive p1 (output c, input a, b);
    table 00:0; endtable
endprimitive

primitive p2 (output c, input a, b);
    table 00:0; endtable
endprimitive

module top;
    wire a, b, c;
    f foo(c, a, b);
    bar bar(c, a, b);
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);

    auto& diags = compilation.getAllDiagnostics();
    REQUIRE(diags.size() == 2);
    CHECK(diags[0].code == diag::ConfigParamsForPrimitive);
    CHECK(diags[1].code == diag::ConfigParamsForPrimitive);
}

TEST_CASE("Invalid top module with unknown library") {
    auto tree = SyntaxTree::fromText(R"(
module top;
endmodule
)");

    CompilationOptions options;
    options.topModules.emplace("nolib.foo");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);

    auto& diags = compilation.getAllDiagnostics();
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == diag::InvalidTopModule);
}

TEST_CASE("Config target multiple primitives") {
    auto tree = SyntaxTree::fromText(R"(
config cfg1;
    design top;
    instance top.b1 use p1;
    instance top.b2 use p1;
endconfig

primitive p1 (output c, input a, b);
    table 00:0; endtable
endprimitive

module top;
    wire a, b, c;
    bar b1(c, a, b), b2(c, a, b);
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;
}

TEST_CASE("Config prim instance with module override") {
    auto tree = SyntaxTree::fromText(R"(
config cfg1;
    design top;
    cell foo use m;
    instance top.b2 use m;
endconfig

module m #(parameter int A);
endmodule

module top;
    foo (supply0, pull1) b1();
    baz #3 b2(), b3();
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");
    options.flags |= CompilationFlags::AllowBareValParamAssignment;

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);

    auto& diags = compilation.getAllDiagnostics();
    REQUIRE(diags.size() == 2);
    CHECK(diags[0].code == diag::InstanceWithStrength);
    CHECK(diags[1].code == diag::UnknownPrimitive);
}

TEST_CASE("Config target multiple primitives with explicit primitive syntax") {
    auto tree = SyntaxTree::fromText(R"(
config cfg1;
    design top;
    instance top.b1 use p1;
    instance top.b2 use p1;
endconfig

primitive p1 (output c, input a, b);
    table 00:0; endtable
endprimitive

module top;
    wire a, b, c;
    bar (supply0, pull1) b1(c, a, b), b2(c, a, b);
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;
}

TEST_CASE("Configs with binds") {
    auto lib1 = std::make_unique<SourceLibrary>("lib1", 1);
    auto lib2 = std::make_unique<SourceLibrary>("lib2", 2);

    auto tree1 = SyntaxTree::fromText(R"(
module mod;
endmodule

module baz #(parameter int A);
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib1.get());

    auto tree2 = SyntaxTree::fromText(R"(
module foo;
    m m2();
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib2.get());

    auto tree3 = SyntaxTree::fromText(R"(
config cfg1;
    design top;
    instance top.m1 liblist lib1;
    cell next liblist work lib2;
    cell baz liblist lib1;
    cell baz use #(.A(1));
endconfig

module m;
    localparam int J = 9;
endmodule

module next;
    bind top.m1 foo f1();
    bind top.m1 baz baz1();
endmodule

module top;
    mod m1();
    next n1();
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree1);
    compilation.addSyntaxTree(tree3);
    compilation.addSyntaxTree(tree2);
    NO_COMPILATION_ERRORS;

    auto& param = compilation.getRoot().lookupName<ParameterSymbol>("top.m1.f1.m2.J");
    CHECK(param.getValue().integer() == 9);
}

TEST_CASE("Configs with libraries and interconnect busses example") {
    auto realLib = std::make_unique<SourceLibrary>("realLib", 1);
    auto logicLib = std::make_unique<SourceLibrary>("logicLib", 2);

    auto cfgs = SyntaxTree::fromText(R"(
config cfgReal;
    design logicLib.top;
    default liblist realLib logicLib;
endconfig

config cfgLogic;
    design logicLib.top;
    default liblist logicLib realLib;
endconfig
)");

    auto topSv = SyntaxTree::fromText(R"(
module top();
    logic rst;
    interconnect aBus[0:3][0:1];
    logic [0:3] dBus;
    driver driverArray[0:3](aBus);
    cmp cmpArray[0:3](aBus,rst,dBus);
endmodule : top
)",
                                      SyntaxTree::getDefaultSourceManager(), "top.sv", "", {},
                                      logicLib.get());

    auto netsPkg = SyntaxTree::fromText(R"(
package NetsPkg;
    nettype real realNet;
endpackage : NetsPkg
)");

    auto driverSvr = SyntaxTree::fromText(R"(
module driver
    import NetsPkg::*;
    #(parameter int delay = 30,
                int iterations = 256)
    (output realNet out[0:1]);
    timeunit 1ns / 1ps;
    real outR[1:0];
    assign out = outR;
    initial begin
        outR[0] = 0.0;
        outR[1] = 3.3;
        for (int i = 0; i < iterations; i++) begin
            #delay outR[0] += 0.2;
            outR[1] -= 0.2;
        end
    end
endmodule : driver
)",
                                          SyntaxTree::getDefaultSourceManager(), "driver.svr", "",
                                          {}, realLib.get());

    auto driverSv = SyntaxTree::fromText(R"(
module driver #(parameter int delay = 30,
                          int iterations = 256)
               (output wire logic out[0:1]);
    timeunit 1ns / 1ps;
    logic [0:1] outvar;

    assign out[0] = outvar[0];
    assign out[1] = outvar[1];

    initial begin
        outvar = '0;
        for (int i = 0; i < iterations; i++)
            #delay outvar++;
    end
endmodule : driver
)",
                                         SyntaxTree::getDefaultSourceManager(), "driver.sv", "", {},
                                         logicLib.get());

    auto cmpSvr = SyntaxTree::fromText(R"(
module cmp
    import NetsPkg::*;
    #(parameter real hyst = 0.65)
    (input realNet inA[0:1],
     input logic rst,
     output logic out);
    timeunit 1ns / 1ps;
    real updatePeriod = 100.0;

    initial out = 1'b0;

    always #updatePeriod begin
        if (rst) out <= 1'b0;
        else if (inA[0] > inA[1]) out <= 1'b1;
        else if (inA[0] < inA[1] - hyst) out <= 1'b0;
    end
endmodule : cmp
)",
                                       SyntaxTree::getDefaultSourceManager(), "cmp.svr", "", {},
                                       realLib.get());

    auto cmpSv = SyntaxTree::fromText(R"(
module cmp #(parameter real hyst = 0.65)
            (input wire logic inA[0:1],
             input logic rst,
             output logic out);

    initial out = 1'b0;

    always @(inA[0], inA[1], rst) begin
        if (rst) out <= 1'b0;
        else if (inA[0] & ~inA[1]) out <= 1'b1;
        else out <= 1'b0;
    end
endmodule : cmp
)",
                                      SyntaxTree::getDefaultSourceManager(), "cmp.sv", "", {},
                                      logicLib.get());

    for (auto cfgName : {"cfgReal", "cfgLogic"}) {
        CompilationOptions options;
        options.topModules.emplace(cfgName);
        options.defaultTimeScale = TimeScale::fromString("1ns/1ns");

        Compilation compilation(options);
        compilation.addSyntaxTree(cfgs);
        compilation.addSyntaxTree(topSv);
        compilation.addSyntaxTree(netsPkg);
        compilation.addSyntaxTree(driverSvr);
        compilation.addSyntaxTree(driverSv);
        compilation.addSyntaxTree(cmpSvr);
        compilation.addSyntaxTree(cmpSv);
        NO_COMPILATION_ERRORS;
    }
}

TEST_CASE("Configs with virtual interfaces") {
    auto tree = SyntaxTree::fromText(R"(
config cfg1;
    design top;
    instance top.i use J;
    cell I use J;
endconfig

interface J;
endinterface

module top;
    I i();
    virtual I vi = i;
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);

    auto& diags = compilation.getAllDiagnostics();
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == diag::VirtualIfaceConfigRule);
}

// SVMake FIELD-d / FIELD-d2 (field report 0005, probe round 18): a virtual interface assignment
// in a config-elaborated design. A configuration does not restrict it (IEEE 1800-2023 25.9
// restricts only the defparam; clause 33 names nothing) and the virtual interface sees the
// instance's configured hierarchy: a virtual interface type's body is bound under the design's
// configuration. Refused by name: a rule that overrides parameters or rebinds to another cell,
// on the instance (VirtualIfaceConfigRule) or nested in it (VirtualIfaceConfigNested), and a
// nested binding that differs from the virtual body's (VirtualIfaceConfigNested).
namespace {

struct VifConfigResult {
    size_t total = 0;
    size_t rule = 0;
    size_t nested = 0;
    size_t nestedRuleNotes = 0;    // NoteConfigRule on a VirtualIfaceConfigNested (R2a)
    size_t nestedBindingNotes = 0; // NoteVirtualIfaceConfigNested (R2b)
    size_t other = 0;              // anything that is not a VirtualIface* diagnostic
    std::vector<SourceRange> ruleNotes;
    std::vector<DiagCode> otherCodes;
};

VifConfigResult countVifDiags(const Diagnostics& diags) {
    VifConfigResult result;
    result.total = diags.size();
    for (auto& diag : diags) {
        if (diag.code == diag::VirtualIfaceConfigRule) {
            result.rule++;
            for (auto& note : diag.notes) {
                if (note.code == diag::NoteConfigRule && !note.ranges.empty())
                    result.ruleNotes.push_back(note.ranges[0]);
            }
        }
        else if (diag.code == diag::VirtualIfaceConfigNested) {
            result.nested++;
            for (auto& note : diag.notes) {
                if (note.code == diag::NoteConfigRule)
                    result.nestedRuleNotes++;
                if (note.code == diag::NoteVirtualIfaceConfigNested)
                    result.nestedBindingNotes++;
            }
        }
        else {
            result.other++;
            result.otherCodes.push_back(diag.code);
        }
    }
    return result;
}

VifConfigResult vifConfigCheck(std::string_view config, std::string_view source,
                               bitmask<CompilationFlags> flags = {}) {
    auto tree = SyntaxTree::fromText(std::string(config) + "\n" + std::string(source));
    CompilationOptions options;
    options.topModules.emplace("cfg");
    options.flags |= flags;

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);
    return countVifDiags(compilation.getAllDiagnostics());
}

// Two libraries: lib1 (priority 2) holds the config and the design, lib2 (priority 1, so the
// higher priority) a J with `logic [W+2:0] a` (4 bits by default) and whatever else is given.
// If vifPath is set, the library of the nested `j` in that variable's virtual interface body is
// returned too.
std::pair<VifConfigResult, std::string> vifTwoLibraries(std::string_view config,
                                                        std::string_view source,
                                                        std::string_view lib2Extra = {},
                                                        std::string_view vifPath = {}) {
    auto lib1 = std::make_unique<SourceLibrary>("lib1", 2);
    auto lib2 = std::make_unique<SourceLibrary>("lib2", 1);
    auto& sm = SyntaxTree::getDefaultSourceManager();
    auto tree1 = SyntaxTree::fromText(std::string(config) + "\n" + std::string(source), sm,
                                      "source", "", {}, lib1.get());
    auto tree2 = SyntaxTree::fromText(R"(
interface J #(parameter int W = 1);
    logic [W+2:0] a;
endinterface
)" + std::string(lib2Extra),
                                      sm, "source", "", {}, lib2.get());
    CompilationOptions options;
    options.topModules.emplace("lib1.cfg:config");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree1);
    compilation.addSyntaxTree(tree2);

    auto result = countVifDiags(compilation.getAllDiagnostics());
    std::string lib;
    if (!vifPath.empty()) {
        auto& var = compilation.getRoot().lookupName<VariableSymbol>(vifPath);
        auto& vit = var.getType().getCanonicalType().as<VirtualInterfaceType>();
        if (auto j = vit.iface.body.find("j"))
            lib = std::string(j->as<InstanceSymbol>().getDefinition().sourceLibrary.name);
    }
    return {result, lib};
}

// Source A: a flat interface; sites in top (three on i, one on ia[1]), in m (u_m.i) and in p
// (through the interface port, on top.i).
constexpr std::string_view vifSourceA = R"(
interface I #(parameter int W = 1);
    logic [W-1:0] a;
    modport mp(input a);
endinterface

module m;
    I i();
    virtual I vi;
    initial vi = i;
endmodule

module other;
endmodule

module p(I ip);
    virtual I v;
    initial v = ip;
endmodule

module top;
    I i();
    I ia[2]();
    m u_m();
    other u_o();
    p u_p(i);
    virtual I vi;
    virtual I.mp vmp;
    function automatic void put(virtual I v);
    endfunction
    initial begin
        vi = i;
        vmp = i.mp;
        put(i);
        vi = ia[1];
    end
endmodule
)";

// Source N: nested instances; vi = i reaches a directly nested J, vk = k a J in a generate
// block and an instance array.
constexpr std::string_view vifSourceN = R"(
interface J #(parameter int W = 1);
    logic [W-1:0] a;
endinterface

interface I2;
    J j();
endinterface

interface I3;
    if (1) begin : g
        J ja[2]();
    end
endinterface

module top;
    I2 i();
    I3 k();
    virtual I2 vi;
    virtual I3 vk;
    initial begin
        vi = i;
        vk = k;
    end
endmodule
)";

// Source D: depth two, J in K in I4.
constexpr std::string_view vifSourceD = R"(
interface J #(parameter int W = 1);
    logic [W-1:0] a;
endinterface
interface K;
    J j();
endinterface
interface I4;
    K k();
endinterface
module top;
    I4 x();
    virtual I4 vx;
    initial vx = x;
endmodule
)";

} // namespace

TEST_CASE("Configs with virtual interfaces: rules that do not rebind (SVMake FIELD-d2)") {
    // D1 rule-less.
    auto r = vifConfigCheck("config cfg; design top; endconfig", vifSourceA);
    CHECK(r.total == 0);

    // D2 localparam (before the design statement) and a default liblist.
    r = vifConfigCheck("config cfg; localparam X = 1; design top; default liblist work; endconfig",
                       vifSourceA);
    CHECK(r.total == 0);

    // D3 a rule on an unrelated instance.
    r = vifConfigCheck("config cfg; design top; instance top.u_o liblist work; endconfig",
                       vifSourceA);
    CHECK(r.total == 0);

    // D4 a rule on the interface's ancestor.
    r = vifConfigCheck("config cfg; design top; instance top.u_m liblist work; endconfig",
                       vifSourceA);
    CHECK(r.total == 0);

    // D5 a hierarchical config with no rule of its own.
    r = vifConfigCheck("config cfgm; design m; endconfig\n"
                       "config cfg; design top; instance top.u_m use cfgm:config; endconfig",
                       vifSourceA);
    CHECK(r.total == 0);

    // D18 a flat interface under a default liblist.
    r = vifConfigCheck("config cfg; design top; default liblist work; endconfig",
                       "interface I; logic a; endinterface\n"
                       "module top; I i(); virtual I vi; initial vi = i; endmodule");
    CHECK(r.total == 0);

    // D6 an instance rule on top.i, the same cell (round 18 P2).
    r = vifConfigCheck("config cfg; design top; instance top.i use I; endconfig", vifSourceA);
    CHECK(r.total == 0);

    // D8 a cell rule, the same cell (P4).
    r = vifConfigCheck("config cfg; design top; cell I use I; endconfig", vifSourceA);
    CHECK(r.total == 0);

    // D9 an instance rule on an array, the same cell.
    r = vifConfigCheck("config cfg; design top; instance top.ia use I; endconfig", vifSourceA);
    CHECK(r.total == 0);

    // D10 a liblist-only rule on the instance (P3).
    r = vifConfigCheck("config cfg; design top; instance top.i liblist work; endconfig",
                       vifSourceA);
    CHECK(r.total == 0);
}

TEST_CASE("Configs with virtual interfaces: an undecided rule on the instance (SVMake FIELD-d2)") {
    // D6b a parameter override on top.i (round 18 P5, undecided): the three top sites on i and
    // p's port site.
    std::string_view cfg6 = "config cfg; design top; instance top.i use #(.W(1)); endconfig";
    auto r = vifConfigCheck(cfg6, vifSourceA);
    CHECK(r.rule == 4);
    CHECK(r.total == 4);
    REQUIRE(r.ruleNotes.size() == 4);
    CHECK(r.ruleNotes[0].start().offset() == cfg6.find("instance top.i"));

    // D7 a parameter override on u_m.i.
    r = vifConfigCheck("config cfg; design top; instance top.u_m.i use #(.W(1)); endconfig",
                       vifSourceA);
    CHECK(r.rule == 1);
    CHECK(r.total == 1);

    // D8b a cell rule with a parameter override: every site.
    r = vifConfigCheck("config cfg; design top; cell I use #(.W(1)); endconfig", vifSourceA);
    CHECK(r.rule == 6);
    CHECK(r.total == 6);

    // D9b an instance rule with parameters on an array: vi = ia[1], so every element has it.
    r = vifConfigCheck("config cfg; design top; instance top.ia use #(.W(1)); endconfig",
                       vifSourceA);
    CHECK(r.rule == 1);
    CHECK(r.total == 1);
}

TEST_CASE("Configs with virtual interfaces: nested instances (SVMake FIELD-d2)") {
    // D11 a parameter override on the nested instance (round 18 P6, undecided): vi = i only.
    auto r = vifConfigCheck("config cfg; design top; instance top.i.j use #(.W(8)); endconfig",
                            vifSourceN);
    CHECK(r.nested == 1);
    CHECK(r.nestedRuleNotes == 1);
    CHECK(r.total == 1);

    // D11b the same through a generate block and an array: vk = k.
    r = vifConfigCheck("config cfg; design top; instance top.k.g.ja use #(.W(8)); endconfig",
                       vifSourceN);
    CHECK(r.nested == 1);
    CHECK(r.nestedRuleNotes == 1);
    CHECK(r.total == 1);

    // D11c the default value as the override: the bindings agree, only the rule refuses it.
    r = vifConfigCheck("config cfg; design top; instance top.k.g.ja use #(.W(1)); endconfig",
                       vifSourceN);
    CHECK(r.nested == 1);
    CHECK(r.nestedRuleNotes == 1);
    CHECK(r.total == 1);

    // D13 a cell rule on the nested cell, the same cell: it binds the virtual body's J too.
    r = vifConfigCheck("config cfg; design top; cell J use J; endconfig", vifSourceN);
    CHECK(r.total == 0);

    // D14 rule-less (round 18 P1).
    r = vifConfigCheck("config cfg; design top; endconfig", vifSourceN);
    CHECK(r.total == 0);

    // D19 no config at all.
    {
        auto tree = SyntaxTree::fromText(vifSourceN);
        Compilation compilation;
        compilation.addSyntaxTree(tree);
        NO_COMPILATION_ERRORS;
    }

    // D17 depth two.
    r = vifConfigCheck("config cfg; design top; instance top.x.k.j use #(.W(8)); endconfig",
                       vifSourceD);
    CHECK(r.nested == 1);
    CHECK(r.nestedRuleNotes == 1);
    CHECK(r.total == 1);

    // D17b depth two, the default value as the override.
    r = vifConfigCheck("config cfg; design top; instance top.x.k.j use #(.W(1)); endconfig",
                       vifSourceD);
    CHECK(r.nested == 1);
    CHECK(r.nestedRuleNotes == 1);
    CHECK(r.total == 1);

    // D22 (c3) an untaken generate block holding a nested J, with CheckUninstantiated.
    r = vifConfigCheck("config cfg; design top; instance top.i.g.j use #(.W(8)); endconfig", R"(
interface J #(parameter int W = 1);
    logic [W-1:0] a;
endinterface
interface I5 #(parameter bit G = 0);
    if (G) begin : g
        J j();
    end
endinterface
module top;
    I5 i();
    virtual I5 vi;
    initial vi = i;
endmodule
)",
                       CompilationFlags::CheckUninstantiated);
    CHECK(r.rule == 0);
    CHECK(r.nested == 0);
    CHECK(r.total == 0);

    // D26 an unassigned instance with a nested rule: only assignments are checked.
    r = vifConfigCheck("config cfg; design top; instance top.i2.j use #(.W(8)); endconfig", R"(
interface J #(parameter int W = 1);
    logic [W-1:0] a;
endinterface
interface I2;
    J j();
endinterface
module top;
    I2 i1();
    I2 i2();
    virtual I2 vi;
    initial vi = i1;
endmodule
)");
    CHECK(r.total == 0);
}

TEST_CASE("Configs with virtual interfaces: two libraries (SVMake FIELD-d2)") {
    // D12 a default liblist that binds the nested J from lib2 (round 18 P7): the virtual body too.
    auto [r12, lib12] = vifTwoLibraries(
        "config cfg; design top; default liblist lib2 lib1; endconfig", vifSourceN, {}, "top.vi");
    CHECK(r12.total == 0);
    CHECK(lib12 == "lib2");

    // D12b the other order: both bind lib1's J (without the configuration the virtual body
    // would take the higher-priority lib2).
    auto [r12b, lib12b] = vifTwoLibraries(
        "config cfg; design top; default liblist lib1 lib2; endconfig", vifSourceN, {}, "top.vi");
    CHECK(r12b.total == 0);
    CHECK(lib12b == "lib1");

    // D16 rule-less: both fall back to the parent's library, lib1, though lib2 has priority.
    constexpr std::string_view source16 = R"(
interface J #(parameter int W = 1);
    logic [W-1:0] a;
endinterface
interface I2;
    J j();
endinterface
module top;
    I2 i();
    virtual I2 vi;
    initial vi = i;
endmodule
)";
    auto [r16, lib16] = vifTwoLibraries("config cfg; design top; endconfig", source16, {},
                                        "top.vi");
    CHECK(r16.total == 0);
    CHECK(lib16 == "lib1");

    // D20 a liblist rule on an ancestor: u_m.i.j and u_m.k.g.ja bind lib2's J, the virtual
    // bodies lib1's (fallback).
    constexpr std::string_view source20 = R"(
interface J #(parameter int W = 1);
    logic [W-1:0] a;
endinterface
interface I2;
    J j();
endinterface
interface I3;
    if (1) begin : g
        J ja[2]();
    end
endinterface
module m;
    I2 i();
    I3 k();
    virtual I2 vi;
    virtual I3 vk;
    initial begin
        vi = i;
        vk = k;
    end
endmodule
module top;
    m u_m();
endmodule
)";
    auto [r20, lib20] = vifTwoLibraries(
        "config cfg; design top; instance top.u_m liblist lib2 lib1; endconfig", source20);
    CHECK(r20.nested == 2);
    CHECK(r20.nestedBindingNotes == 2);
    CHECK(r20.nestedRuleNotes == 0);
    CHECK(r20.total == 2);

    // D20b the same at depth two, below a pair that agrees: x.k is lib1's K on both sides,
    // x.k.j differs.
    auto [r20b, lib20b] = vifTwoLibraries(
        "config cfg; design top; instance top.u_m liblist lib2 lib1; endconfig", R"(
interface J #(parameter int W = 1);
    logic [W-1:0] a;
endinterface
interface K;
    J j();
endinterface
interface I4;
    K k();
endinterface
module m;
    I4 x();
    virtual I4 vx;
    initial vx = x;
endmodule
module top;
    m u_m();
endmodule
)");
    CHECK(r20b.nested == 1);
    CHECK(r20b.nestedBindingNotes == 1);
    CHECK(r20b.total == 1);

    // D21 a nested use of another library's same-named cell, on an instance named after its
    // definition: the virtual body's j has the path top.I2.j too, and no instance rule may reach it.
    auto [r21, lib21] = vifTwoLibraries(
        "config cfg; design top; instance top.I2.j use lib2.J; endconfig", R"(
interface J #(parameter int W = 1);
    logic [W-1:0] a;
endinterface
interface I2;
    J j();
endinterface
module top;
    I2 I2();
    virtual I2 vi;
    initial vi = I2;
endmodule
)",
        {}, "top.vi");
    CHECK(r21.nested == 1);
    CHECK(r21.nestedBindingNotes == 1);
    CHECK(r21.total == 1);
    CHECK(lib21 == "lib1");

    // D23 a comparison is not an assignment: R2b does not apply.
    auto [r23, lib23] = vifTwoLibraries(
        "config cfg; design top; instance top.u_m liblist lib2 lib1; endconfig", R"(
interface J #(parameter int W = 1);
    logic [W-1:0] a;
endinterface
interface I2;
    J j();
endinterface
module m;
    I2 i();
    virtual I2 vi;
    initial if (vi == i) $display("same");
endmodule
module top;
    m u_m();
endmodule
)");
    CHECK(r23.total == 0);

    // D24 (plan review H2) a config type override on the top names a virtual interface type,
    // which is created while the top is: its body must still be bound under the configuration
    // (lib1's J by the liblist; without the configuration it would take the higher-priority lib2).
    auto [r24, lib24] = vifTwoLibraries(
        "config cfg; design top; instance top use #(.T(virtual I2)); "
        "default liblist lib1 lib2; endconfig",
        R"(
interface J #(parameter int W = 1);
    logic [W-1:0] a;
endinterface
interface I2;
    J j();
endinterface
module top #(parameter type T = int);
    I2 i();
    T vt;
    virtual I2 vi;
    initial begin
        vi = i;
        vt = i;
    end
endmodule
)",
        {}, "top.vi");
    CHECK(r24.total == 0);
    CHECK(lib24 == "lib1");

    // D25 (plan review H3) a cell the design's configuration cannot resolve in the virtual body
    // (J is only in lib2, the top's liblist is lib1) is looked up as without a configuration:
    // no new error, and the bindings agree (lib2's J on both sides).
    auto [r25, lib25] = vifTwoLibraries(
        "config cfg; design top; default liblist lib1; instance top.u liblist lib1 lib2; "
        "endconfig",
        R"(
interface I2;
    J j();
endinterface
module m;
    I2 i();
    virtual I2 vi;
    initial vi = i;
endmodule
module top;
    m u();
endmodule
)",
        {}, "top.u.vi");
    CHECK(r25.other == 0);
    CHECK(r25.total == 0);
    CHECK(lib25 == "lib2");

    // Attack 4 (plan review): a use of another library's same-named cell on the interface
    // instance itself is not an undecided rule; slang's type check refuses it (both types print
    // as I), with no VirtualIface* diagnostic. Pinned as K-2028's companion.
    auto [ra4, liba4] = vifTwoLibraries(
        "config cfg; design top; instance top.i use lib2.I; endconfig", R"(
interface I;
    logic a;
endinterface
module top;
    I i();
    virtual I vi;
    initial vi = i;
endmodule
)",
        "interface I; logic [3:0] a; endinterface\n");
    CHECK(ra4.rule == 0);
    CHECK(ra4.nested == 0);
    CHECK(ra4.total == 1);
    REQUIRE(ra4.otherCodes.size() == 1);
    CHECK(ra4.otherCodes[0] == diag::BadAssignment);

    // D27 (closing review C2) a liblist rule on the interface instance itself: the real i.j
    // binds lib2's J by the rule's liblist, the virtual body lib1's (no instance-path rule
    // reaches it; the parent's library). Not an undecided rule, so R2b refuses it.
    auto [r27, lib27] = vifTwoLibraries(
        "config cfg; design top; instance top.i liblist lib2 lib1; endconfig", R"(
interface J #(parameter int W = 1);
    logic [W-1:0] a;
endinterface
interface I2;
    J j();
endinterface
module top;
    I2 i();
    virtual I2 vi;
    initial vi = i;
endmodule
)",
        {}, "top.vi");
    CHECK(r27.rule == 0);
    CHECK(r27.nested == 1);
    CHECK(r27.nestedBindingNotes == 1);
    CHECK(r27.nestedRuleNotes == 0);
    CHECK(r27.total == 1);
    CHECK(lib27 == "lib1");
}

TEST_CASE("Configs with virtual interfaces: the fallback picks another cell (SVMake FIELD-d2t)") {
    // D25b (closing review C1) three libraries: c (highest priority) holds an 8-bit J, a the
    // design and the config, b a 4-bit J. The design's liblist (a) resolves no J in the virtual
    // body, so it falls back to the config-less lookup and takes c's J; the instance's own
    // liblist (a b) binds b's J. R2b refuses vi = i once.
    auto libC = std::make_unique<SourceLibrary>("c", 1);
    auto libA = std::make_unique<SourceLibrary>("a", 2);
    auto libB = std::make_unique<SourceLibrary>("b", 3);
    auto& sm = SyntaxTree::getDefaultSourceManager();
    auto treeA = SyntaxTree::fromText(R"(
config cfg;
    design a.tb;
    default liblist a;
    instance tb.u liblist a b;
endconfig
interface I;
    J j();
endinterface
module m;
    I i();
    virtual I vi;
    initial vi = i;
endmodule
module tb;
    m u();
endmodule
)",
                                      sm, "source", "", {}, libA.get());
    auto treeB = SyntaxTree::fromText("interface J; logic [3:0] a; endinterface\n", sm, "source",
                                      "", {}, libB.get());
    auto treeC = SyntaxTree::fromText("interface J; logic [7:0] a; endinterface\n", sm, "source",
                                      "", {}, libC.get());
    CompilationOptions options;
    options.topModules.emplace("a.cfg:config");

    Compilation compilation(options);
    compilation.addSyntaxTree(treeA);
    compilation.addSyntaxTree(treeB);
    compilation.addSyntaxTree(treeC);

    auto r = countVifDiags(compilation.getAllDiagnostics());
    CHECK(r.rule == 0);
    CHECK(r.nested == 1);
    CHECK(r.nestedBindingNotes == 1);
    CHECK(r.nestedRuleNotes == 0);
    CHECK(r.total == 1);

    auto& root = compilation.getRoot();
    auto& real = root.lookupName<InstanceSymbol>("tb.u.i.j");
    CHECK(real.getDefinition().sourceLibrary.name == "b");
    auto& var = root.lookupName<VariableSymbol>("tb.u.vi");
    auto& vit = var.getType().getCanonicalType().as<VirtualInterfaceType>();
    auto j = vit.iface.body.find("j");
    REQUIRE(j);
    CHECK(j->as<InstanceSymbol>().getDefinition().sourceLibrary.name == "c");
}

TEST_CASE("Configs with virtual interfaces: a bound instance (SVMake FIELD-d2)") {
    constexpr std::string_view source = R"(
interface I #(parameter int W = 4);
    logic [W-1:0] a;
endinterface
module m;
endmodule
module top;
    m u_m();
    virtual I vb;
    initial vb = u_m.bi;
    bind m I bi();
endmodule
)";

    // D15a a cell rule selects the bound instance, the same cell.
    auto r = vifConfigCheck("config cfg; design top; cell I use I; endconfig", source);
    CHECK(r.total == 0);

    // D15b rule-less.
    r = vifConfigCheck("config cfg; design top; endconfig", source);
    CHECK(r.total == 0);

    // D15c a cell rule with parameters: the bound instance carries its rule.
    r = vifConfigCheck("config cfg; design top; cell I use #(.W(4)); endconfig", source);
    CHECK(r.rule == 1);
    CHECK(r.total == 1);
    CHECK(r.ruleNotes.size() == 1);
}

TEST_CASE("Configs with instance caching") {
    auto tree = SyntaxTree::fromText(R"(
config cfg1;
    design top;
    instance top.i2.j.k use L#(.b(3));
endconfig

module I;
    J j();
endmodule

module J;
    K k();
endmodule

module K;
endmodule

module L #(parameter int b);
    if (b == 3) begin : blk
        $warning("b is 3");
    end
endmodule

module top;
    I i1();
    I i2();
endmodule
)");

    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);

    auto& diags = compilation.getAllDiagnostics();
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == diag::WarningTask);
}

TEST_CASE("Config cell override keeps subtree active for defparams") {
    auto tree = SyntaxTree::fromText(R"(
module leaf #(parameter int p = 0);
    if (p) begin : blk
        $info("Hello");
    end
endmodule

module b_alt;
    leaf l();
    defparam l.p = 1;
endmodule

module b;
endmodule

module a;
    b b1();
endmodule

module top;
    a a1();
endmodule

config cfg1;
    design top;
    cell b use b_alt;
endconfig
)");

    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);

    auto& diags = compilation.getAllDiagnostics();
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == diag::InfoTask);
}

TEST_CASE("Config instance override keeps subtree active for defparams") {
    auto tree = SyntaxTree::fromText(R"(
module leaf #(parameter int p = 0);
    if (p) begin : blk
        $info("Hello");
    end
endmodule

module b_alt;
    leaf l();
    defparam l.p = 1;
endmodule

module b;
endmodule

module a;
    b b1();
endmodule

module top;
    a a1();
endmodule

config cfg1;
    design top;
    instance top.a1.b1 use b_alt;
endconfig
)");

    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);

    auto& diags = compilation.getAllDiagnostics();
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == diag::InfoTask);
}

// SVMake FRONT-D065 (K-1521): IEEE 1800-2023 33.3.1 -- "If multiple cells with the same name map
// to the same library, then the last cell encountered shall be written to the library ... a
// warning shall be issued." A library map's library takes the last definition, with a warning;
// a library no map declares keeps the 3.13 a) error.
namespace {

struct DupCellRun {
    std::unique_ptr<Driver> driver;
    std::unique_ptr<Compilation> compilation;
    bool reportOk = false;

    explicit DupCellRun(std::string_view argsTail) {
        driver = std::make_unique<Driver>();
        driver->addStandardArgs();
        auto args = fmt::format("testfoo {}", fmt::format(fmt::runtime(argsTail),
                                                         findTestDir() + "dupcell/"));
        CHECK(driver->parseCommandLine(args));
        CHECK(driver->processOptions());
        CHECK(driver->parseAllSources());
        compilation = driver->createCompilation();
        driver->reportCompilation(*compilation, false);
        reportOk = driver->reportDiagnostics(false);
    }

    std::vector<DiagCode> codes() const {
        std::vector<DiagCode> result;
        for (auto& d : compilation->getAllDiagnostics())
            result.push_back(d.code);
        return result;
    }

    std::string fileOf(std::string_view instPath) const {
        auto& inst = compilation->getRoot().lookupName<InstanceSymbol>(instPath);
        auto name = compilation->getSourceManager()->getFileName(
            inst.getDefinition().location);
        return std::string(std::filesystem::path(std::string(name)).filename().string());
    }
};

} // namespace

TEST_CASE("Duplicate cell in a library map's library: the last wins, with a warning (33.3.1)") {
    auto guard = OS::captureOutput();
    DupCellRun run("--libmap \"{0}one.map\" --top tb");
    CHECK(run.reportOk);
    auto codes = run.codes();
    REQUIRE(codes.size() == 1);
    CHECK(codes[0] == diag::DuplicateLibraryCell);
    CHECK(run.driver->diagEngine.getSeverity(diag::DuplicateLibraryCell, {}) ==
          DiagnosticSeverity::Warning);
    CHECK(run.fileOf("tb.h") == "second.sv");
}

TEST_CASE("Duplicate cell in a library map's library ignores --allow-lib-module-redef (33.3.1)") {
    auto guard = OS::captureOutput();
    DupCellRun run("--libmap \"{0}one.map\" --top tb --allow-lib-module-redef");
    CHECK(run.reportOk);
    auto codes = run.codes();
    REQUIRE(codes.size() == 1);
    CHECK(codes[0] == diag::DuplicateLibraryCell);
    CHECK(run.fileOf("tb.h") == "second.sv");
}

TEST_CASE("Duplicate cell across two library maps declaring one library (33.3.1)") {
    auto guard = OS::captureOutput();
    DupCellRun run("--libmap \"{0}a.map\" --libmap \"{0}b.map\" --top a_tb");
    CHECK(run.reportOk);
    auto codes = run.codes();
    REQUIRE(codes.size() == 1);
    CHECK(codes[0] == diag::DuplicateLibraryCell);
    CHECK(run.fileOf("a_tb.h") == "helper.sv");
    auto& inst = run.compilation->getRoot().lookupName<InstanceSymbol>("a_tb.h");
    auto name = std::string(
        run.compilation->getSourceManager()->getFileName(inst.getDefinition().location));
    CHECK(name.find("/b/helper.sv") != std::string::npos);
}

TEST_CASE("Duplicate module with no library map stays an error (3.13 a)") {
    auto guard = OS::captureOutput();
    DupCellRun run("\"{0}first.sv\" \"{0}second.sv\" \"{0}tb.sv\" --top tb");
    CHECK_FALSE(run.reportOk);
    auto codes = run.codes();
    REQUIRE(codes.size() == 1);
    CHECK(codes[0] == diag::DuplicateDefinition);
}

TEST_CASE("Cells of different kinds with one name in a library map's library stay an error") {
    auto guard = OS::captureOutput();
    DupCellRun run("--libmap \"{0}kinds.map\" --top tb");
    auto codes = run.codes();
    // A module and an interface are both definitions: slang's existing duplicate error.
    CHECK_FALSE(run.reportOk);
    CHECK(std::ranges::find(codes, diag::DuplicateDefinition) != codes.end());
    CHECK(std::ranges::find(codes, diag::DuplicateLibraryCell) == codes.end());
}

TEST_CASE("Cells of different kinds in a library map's library stay an error under "
          "--allow-lib-module-redef") {
    // The keep-first shortcut of --allow-lib-module-redef is for -v / -y library files; a
    // library map's library is not one, whatever the kinds (SVMake FRONT-D065 closing review
    // LOW-1: it used to keep the module silently).
    auto guard = OS::captureOutput();
    DupCellRun run("--libmap \"{0}kinds.map\" --top tb --allow-lib-module-redef");
    auto codes = run.codes();
    CHECK_FALSE(run.reportOk);
    CHECK(std::ranges::find(codes, diag::DuplicateDefinition) != codes.end());
    CHECK(std::ranges::find(codes, diag::DuplicateLibraryCell) == codes.end());
}

TEST_CASE("Duplicate cells under one wildcard file spec: the lexically last wins (33.3.1)") {
    // "The last cell encountered" inside one wildcard file specification is the expansion's
    // order, which is sorted lexically: directory iteration order differs between file systems
    // (SVMake FRONT-D065 closing review MEDIUM-1). The files are created in an order unlike
    // their lexical one, so creation order (tmpfs) cannot pass this by chance either.
    auto guard = OS::captureOutput();

    std::error_code ec;
    auto root = fs::temp_directory_path(ec);
    REQUIRE(!ec);
    root /= fmt::format("slang-dupcell-glob-{}", OS::getpid());
    fs::remove_all(root, ec);
    fs::create_directories(root / "src", ec);
    REQUIRE(!ec);
    ScopeGuard cleanup([&] { fs::remove_all(root, ec); });

    auto writeFile = [](const fs::path& path, std::string_view text) {
        std::ofstream out(path);
        REQUIRE(out.good());
        out << text;
    };

    const char* names[] = {"h_m", "h_z", "h_c", "h_x", "h_a", "h_w", "h_e", "h_y", "h_b", "h_d"};
    for (auto name : names)
        writeFile(root / "src" / (std::string(name) + ".sv"), "module helper; endmodule\n");
    writeFile(root / "src" / "tb.sv", "module tb; helper h(); endmodule\n");
    writeFile(root / "lib.map", "library work \"src/*.sv\";\n");

    DupCellRun run(fmt::format("--libmap \"{}\" --top tb", getU8Str(root / "lib.map")));
    CHECK(run.reportOk);
    auto codes = run.codes();
    CHECK(codes.size() == 9);
    CHECK(std::ranges::count(codes, diag::DuplicateLibraryCell) == (std::ptrdiff_t)codes.size());
    CHECK(run.fileOf("tb.h") == "h_z.sv");
}

TEST_CASE("Duplicate primitive in a library map's library: the last wins, with a warning (33.3.1)") {
    auto guard = OS::captureOutput();
    DupCellRun run("--libmap \"{0}udps.map\" --top ut");
    CHECK(run.reportOk);
    auto codes = run.codes();
    REQUIRE(codes.size() == 1);
    CHECK(codes[0] == diag::DuplicateLibraryCell);
    // udp1.sv is a buffer and udp2.sv an inverter: the instance binds the second.
    auto& inst = run.compilation->getRoot().lookupName<PrimitiveInstanceSymbol>("ut.u");
    auto name = std::string(
        run.compilation->getSourceManager()->getFileName(inst.primitiveType.location));
    CHECK(std::filesystem::path(name).filename().string() == "udp2.sv");
}
