// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT

#include "AnalysisTests.h"

#include "slang/analysis/DataFlowAnalysis.h"
#include "slang/diagnostics/DiagnosticEngine.h"

class TestAnalysis : public AbstractFlowAnalysis<TestAnalysis, int> {
public:
    TestAnalysis(const Symbol& symbol) : AbstractFlowAnalysis(symbol, {}) {}

    void handle(const NamedValueExpression& expr) {
        getState() += 1;
        visitExpr(expr);
    }

    void joinState(int& state, const int& other) const { state += other; }
    void meetState(int& state, const int& other) const { state = std::min(state, other); }
    int copyState(const int& state) const { return state; }
    int unreachableState() const { return 0; }
    int topState() const { return 0; }

    int getCurrentState() const { return getState(); }
};

TEST_CASE("Basic flow analysis") {
    auto tree = SyntaxTree::fromText(R"(
module m(input clk, input rst, output logic a, input logic b);
    always @(posedge clk) begin
        if (rst)
            a <= 0;
        else
            a <= b;
    end
endmodule
)");

    Compilation compilation;
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;

    auto& m = compilation.getRoot().topInstances[0]->body;
    auto& proc = m.membersOfType<ProceduralBlockSymbol>().front();

    TestAnalysis analysis(proc);
    analysis.run(proc.getBody());
    CHECK(!analysis.bad);
    CHECK(analysis.getCurrentState() == 5);
}

TEST_CASE("Inferred latch test") {
    auto& code = R"(
module m(input clk, input rst, input [2:0] in, output logic out);
    struct { logic a; logic b; } s;
    always_comb begin
        if (rst) begin
            s.a = 0;
            s.b = 0;
        end
        else begin
            s.a = 1;
        end
    end

    always_comb begin
        case(in[1:0])
            2'b00:  out = 1'b0;
            2'b01:  out = 1'b1;
            2'b10:  out = 1'b1;
            // inferred latch "out" :: missing condition 2'b11/default
        endcase
    end

    logic flop0, flop1, flop2, flop3;
    logic next0, next1, next2, next3;
    logic b,c;

    always_comb begin
        next0 = flop0;
        next1 = flop1;
        // inferred latch "next2" :: missing initial condition
        next3 = flop3;
        case(in[2:0])
            3'b001:             next0 = in[0];
            3'b010:  if(b)      next1 = in[0];
            3'b100:  if(c)      next2 = in[0];
            default: if(!b&&!c) next3 = in[0];
        endcase
    end

    parameter p = 4;
    logic [p-1:0] slots;
    logic q,r,t;
    always_comb begin
        for (int i = 0; i < p; i++) begin
            if (b) slots[i] = 1;
            else   slots[i] = 0;
        end

        for (;;) begin
            q = 1;
            break;
        end

        forever begin
            r = 1;
            break;
            t = 1;
        end
    end

    logic u, v;
    always_comb begin
        while (0) begin
            u = 1;
        end
        while (1) begin
            v = 1;
            break;
        end
    end

    I iface();
    n n1(iface);

    typedef enum logic [2:0] {
        A, B, C, D,
        E, F, G, H
    } e_t;

    e_t e;
    logic x;

    always_comb begin
        case (e)
            A, B, C, D: x = 1'b1;
            default: x = 1'b0;
        endcase
    end
endmodule

interface I;
    logic a;
    modport m(output a);
endinterface

module n(I.m i);
    always_comb begin
        i.a = 1;
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    REQUIRE(diags.size() == 4);
    CHECK(diags[0].code == diag::InferredLatch);
    CHECK(diags[1].code == diag::InferredLatch);
    CHECK(diags[2].code == diag::InferredLatch);
    CHECK(diags[3].code == diag::CaseEnumExplicit);
}

TEST_CASE("Inferred latch warning correct LSP in message") {
    auto& code = R"(
module m;
    struct packed { int a; int b; } s;
    logic c;

    always_comb begin
        if (c) begin
            s.a = 1;
            s.b[0] = 1;
            s.b[1] = 1;
        end
        else begin
            s[0] = 1;
        end
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    std::string result = "\n" + report(diags);
    CHECK(result == R"(
source:8:13: warning: latch inferred for 's.a' because it is not assigned on all control paths [-Winferred-latch]
            s.a = 1;
            ^~~
source:10:13: warning: latch inferred for 's.b[1]' because it is not assigned on all control paths [-Winferred-latch]
            s.b[1] = 1;
            ^~~~~~
)");
}

TEST_CASE("Data flow with class members") {
    auto& code = R"(
class C;
    int i;
endclass

function C foo;
    return null;
endfunction

module m(input a);
    C c;
    always_comb begin
        if (a) begin
            foo().i = 1;
            foo().i = 2;
            c.i = 3;
        end
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("Data flow with constants in repeat, randcase, foreach") {
    auto& code = R"(
module m;
    int i, j, k, l, n, o, p;
    int d[3][][2];
    logic [1:0] e[2][1];
    always_comb begin
        repeat (1) begin
            i = 1;
        end

        randcase
            0: ;
            1: j = 1;
        endcase

        randcase
            2: k = 1;
            1: k = 1;
        endcase

        randcase
            0: l = 1;
            0: l = 2;
        endcase

        foreach (d[a, b, c]) begin
            n = 1;
        end

        foreach (d[a, , c]) begin
            o = 1;
        end

        foreach (e[a, b, c]) begin
            p = 1;
        end
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == diag::InferredLatch);
}

TEST_CASE("Functions with missing returns") {
    auto& code = R"(
function int foo;
endfunction

function int bar(input a);
    if (a) begin
        return 1;
    end
endfunction

function int baz;
    while (1) begin
        return 1;
    end
endfunction

function int boz(input a);
    if (a) begin
        return 1;
    end
    boz = 2;
endfunction

function int fiz(input a);
    if (a) begin
        fiz = 1;
    end
endfunction
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    REQUIRE(diags.size() == 3);
    CHECK(diags[0].code == diag::MissingReturn);
    CHECK(diags[1].code == diag::IncompleteReturn);
    CHECK(diags[2].code == diag::IncompleteReturn);
}

TEST_CASE("Unevaluated syscall args don't contribute to data flow") {
    auto& code = R"(
module m;
    int i, j;
    always_comb begin
        if (i) begin
            i = 1;
            j = 2;
        end
        else begin
            j = $bits((i = 2));
        end
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == diag::InferredLatch);
}

TEST_CASE("Analysis with syscalls that never return") {
    auto& code = R"(
function int foo(int i);
    if (i == 0)
        return 1;
    else if (i == 1)
        return 2;
    else
        $fatal();
endfunction
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("For loop analysis crash regress -- GH #1292") {
    auto& code = R"(
module Test (
  input  logic clk,
  input  logic in,
  output logic out
);

  logic internal[4];

  assign internal[0] = in;
  assign out = internal[3];

  always_ff @(posedge clk) begin
    internal[1] <= internal[0];
    for (int stage = 2; stage <= 3; stage++) begin
      internal[stage] <= internal[stage-1];
    end
  end

endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("For loop unrolling with automatic var declared outside loop header") {
    // An automatic loop variable initialized in the for header (but declared
    // before it) should be successfully unrolled, just like one declared inside.
    auto& code = R"(
module Test;
  logic [3:0] a;
  always_comb begin
    automatic int i;
    for (i = 0; i < 4; i++) begin
      a[i] = 1'b1;
    end
  end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("For loop unrolling with multiple automatic vars declared outside loop header") {
    // Multiple automatic loop variables initialized in the for header (but
    // declared before it) should all be successfully unrolled.
    auto& code = R"(
module Test;
  logic [3:0] a;
  logic [3:0] b;
  always_comb begin
    automatic int i;
    automatic int j;
    for (i = 0, j = 3; i < 4; i++, j--) begin
      a[i] = 1'b1;
      b[j] = 1'b0;
    end
  end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("For loop with missing steps: body executes at least once (automatic var)") {
    auto& code = R"(
module Test;
  logic [3:0] a;
  always_comb begin
    automatic int i;
    for (i = 0; i < 4; ) begin
      a[i] = 1'b1;
      i++;
    end
  end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("For loop with missing steps: body never executes when condition initially false") {
    auto& code = R"(
module Test;
  logic [3:0] a;
  always_comb begin
    automatic int i;
    // condition is false from the start: loop body never runs
    for (i = 10; i < 4; ) begin
      a[i] = 1'b1;
    end
  end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == diag::LoopCondNotModified);
}

TEST_CASE("For loop with no stop expression: body always executes") {
    // A loop with no stop expression is infinite. The body is guaranteed to
    // execute at least once and the normal exit path is unreachable.
    auto& code = R"(
module Test;
  logic [3:0] a;
  always_comb begin
    automatic int i = 0;
    for (; ; i++) begin
      a[i] = 1'b1;
      if (i == 3) break;
    end
  end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("Latch with conditions inside loop regress -- GH #1364") {
    auto& code = R"(
module Test;
  logic [3:0][31:0] a;

  always_comb begin
    for (int i = 0; i < 4; i++) begin
      if (i == 0) begin
        a[i] = 0;
      end
      else if (i == 1) begin
        a[i] = 1;
      end
      else if (i == 2) begin
        a[i] = 2;
      end
      else if (i == 3) begin
        a[i] = 3;
      end
    end
  end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("For loop body modifying iter var stops unrolling -- GH #1817") {
    // The loop body assigns to the iteration variable, so the iteration values
    // computed from the loop header don't match what the body actually sees.
    auto& code = R"(
module top;
  logic a;
  always_latch begin
    for (int i = 0; i < 2; ++i) begin
      i = 100;
      if (i == 1)
        a = 1;
    end
  end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == diag::LoopVarModify);
}

TEST_CASE("For loop body modifying iter var infers latch -- GH #1817") {
    // Because the body assigns i = 2, the test i == 2 can no longer be proven
    // false during unrolling. With unrolling disabled, 'a' is correctly seen as
    // only conditionally assigned, which infers a latch.
    auto& code = R"(
module top;
  logic a;
  always_comb begin
    for (int i = 0; i < 2; ++i) begin
      i = 2;
      if (i == 2)
        a = 1;
    end
  end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    REQUIRE(diags.size() == 2);
    CHECK(diags[0].code == diag::LoopVarModify);
    CHECK(diags[1].code == diag::InferredLatch);
}

TEST_CASE("For loop reading but not writing iter var still unrolls -- GH #1817") {
    // Reading the iteration variable in the body (without writing it) must not
    // disable unrolling. Here the branch guard depends on the iteration variable
    // being a known constant; if the loop weren't unrolled, 'a' would be only
    // conditionally assigned and infer a latch. Correct unrolling proves every
    // bit of 'a' is assigned, so there are no diagnostics.
    auto& code = R"(
module top;
  logic [3:0] a;
  always_comb begin
    for (int i = 0; i < 4; i++) begin
      if (i < 4)
        a[i] = 1'b1;
    end
  end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("No latch inferred when first loop iteration is proven -- GH #1870") {
    // Same loop shape as above inside always_comb. Since at least one iteration
    // is guaranteed, the assignment to c must not be discarded.
    auto& code = R"(
module m(input in);
    reg c;
    always_comb begin
        automatic int j = 4;
        for (int i = 0; i < (i == 0 ? 4 : j); i++) begin
            c = in;
        end
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("No latch inferred for always_latch") {
    auto& code = R"(
module m(input clk, input rst, input [2:0] in, output logic out, output logic out1);
    struct { logic a; logic b; } s, s1;
    always_latch begin
        if (rst) begin
            s.a = 0;
            s.b = 0;
        end
        else begin
            s.a = 1;
            s.b = 0;
        end
    end
    always_latch begin
        if (rst) begin
            s1.a = 0;
            s1.b = 0;
        end
        // inferred latches on "s1.a" and "s1.b" :: missing else branch
    end
    always_latch begin
        case(in[1:0])
            2'b00:  out1 = 1'b0;
            2'b01:  out1 = 1'b1;
            2'b10:  out1 = 1'b1;
            // inferred latch "out" :: missing condition 2'b11/default
        endcase
    end
    always_latch begin
        case(in[1:0])
            2'b00:  out = 1'b0;
            2'b01:  out = 1'b1;
            2'b10:  out = 1'b1;
            // inferred latch "out" :: missing condition 2'b11/default
        endcase
    end
    logic flop0, flop1, flop2, flop3;
    logic next0, next1, next2, next3;
    logic b,c;
    always_latch begin
        next0 = flop0;
        next1 = flop1;
        // inferred latch "next2" :: missing initial condition
        next3 = flop3;
        case(in[2:0])
            3'b001:             next0 = in[0];
            3'b010:  if(b)      next1 = in[0];
            3'b100:  if(c)      next2 = in[0];
            default: if(!b&&!c) next3 = in[0];
        endcase
    end
    parameter p = 4;
    logic [p-1:0] slots;
    logic q,r,t;
    always_latch begin
        for (int i = 0; i < p; i++) begin
            if (b) slots[i] = 1;
            else   slots[i] = 0;
        end
        for (;;) begin
            q = 1;
            break;
        end
        forever begin
            r = 1;
            break;
            t = 1;
        end
    end
    logic u, v;
    always_latch begin
        while (0) begin
            u = 1;
        end
        while (1) begin
            v = 1;
            break;
        end
    end
    I iface();
    n n1(iface);
    typedef enum logic [2:0] {
        A, B, C, D,
        E, F, G, H
    } e_t;
    e_t e;
    logic x;
    always_latch begin
        case (e)
            A, B, C, D: x = 1'b1;
            default: x = 1'b0;
        endcase
    end
endmodule
interface I;
    logic a;
    modport m(output a);
endinterface
module n(I.m i);
    always_latch begin
        i.a = 1;
    end
endmodule
module test10(out);
  output reg [2:0] out;
  reg en;
  always_latch begin
    out[1] <= 1;
    case (en)
      2'b00 : out[1] <= 1'b1;
      2'b10 : out[2] <= 1'b1;
      default : ;
    endcase
  end
endmodule // test
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    REQUIRE(diags.size() == 13);
    CHECK(diags[0].code == diag::InferredComb);
    CHECK(diags[1].code == diag::InferredComb);
    CHECK(diags[2].code == diag::InferredComb);
    CHECK(diags[3].code == diag::InferredComb);
    CHECK(diags[4].code == diag::InferredComb);
    CHECK(diags[5].code == diag::InferredComb);
    CHECK(diags[6].code == diag::InferredComb);
    CHECK(diags[7].code == diag::InferredComb);
    CHECK(diags[8].code == diag::InferredComb);
    CHECK(diags[9].code == diag::CaseEnumExplicit);
    CHECK(diags[10].code == diag::InferredComb);
    CHECK(diags[11].code == diag::InferredComb);
    CHECK(diags[12].code == diag::InferredComb);
}

TEST_CASE("DFA with missing function call error -- GH #1426") {
    auto& code = R"(
function reg foo0(reg q);
    return bar(q);
endfunction
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto tree = SyntaxTree::fromText(code);
    compilation.addSyntaxTree(tree);
    compilation.getAllDiagnostics();
    compilation.freeze();

    analysisManager.analyze(compilation);
    auto diags = analysisManager.getDiagnostics();
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("Always without timing control analysis") {
    auto& code = R"(
module timing_test;
    logic clk, a, b, c;
    // This should trigger AlwaysWithoutTimingControl
    always begin
        a = b;
    end
    // This should be fine (has timing control)
    always @(posedge clk) begin
        b = a;
    end
    // This should trigger it (assertion doesn't count as timing control)
    always begin
        assert property (@(posedge clk) a |=> b);
    end
    // Test subroutine calls
    function void my_func();
        // No timing control in function
    endfunction
    task my_task();
        @(posedge clk);  // Has timing control
    endtask
    task my_task_2();
        my_task();
    endtask
    // This should be fine (calls task which calls task with timing)
    always begin
        my_task_2();
    end
    // This should trigger diagnostic (calls function without timing)
    always begin
        my_func();
    end
    // This should be fine (calls task with timing)
    always begin
        my_task();
    end
    // Test with wait statements (should be valid)
    always begin
        wait(clk);
        c = a;
    end
    // Test with delay control (should be valid)
    always begin
        #10 c = b;
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    REQUIRE(diags.size() == 3);
    CHECK(diags[0].code == diag::AlwaysWithoutTimingControl);
    CHECK(diags[1].code == diag::AlwaysWithoutTimingControl);
    CHECK(diags[2].code == diag::AlwaysWithoutTimingControl);
}

TEST_CASE("Always with intra-assignment timing controls") {
    {
        // with timing control
        auto& code = R"(
module m;
    logic clk;
    always clk = #1 ~clk;
endmodule
)";

        Compilation compilation;
        AnalysisManager analysisManager;

        auto diags = analyze(code, compilation, analysisManager);
        CHECK_DIAGS_EMPTY;
    }

    {

        // without timing control
        auto& code = R"(
    module m;
        logic clk;
        always clk = ~clk;
        endmodule
    )";
        Compilation compilation;
        AnalysisManager analysisManager;

        auto diags = analyze(code, compilation, analysisManager);
        REQUIRE(diags.size() == 1);
        CHECK(diags[0].code == diag::AlwaysWithoutTimingControl);
    }
}

struct CustomState : public DataFlowState {};

class CustomDFA : public DataFlowAnalysis<CustomDFA, CustomState> {
public:
    std::string& callName;

    CustomDFA(AnalysisContext& context, const Symbol& symbol, std::string& callName) :
        DataFlowAnalysis(context, symbol, false), callName(callName) {}

    void handle(const CallExpression& call) {
        DataFlowAnalysis::handle(call);
        callName = std::string(call.getSubroutineName());
    }
};

TEST_CASE("Custom DFA API test") {
    auto& code = R"(
function void foo;
endfunction

module m;
    always_comb foo();
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    std::string callName;

    analysisManager.setCustomDFAProvider([&](AnalysisContext& context, const Symbol& symbol,
                                             const AnalyzedProcedure* parentProcedure) {
        CustomDFA dfa(context, symbol, callName);
        dfa.run();

        if (dfa.bad)
            return AnalyzedProcedure(symbol, parentProcedure);
        else
            return AnalyzedProcedure(context, symbol, parentProcedure, dfa);
    });

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;

    CHECK(callName == "foo");
}

TEST_CASE("always_comb task call with delay") {
    auto& code = R"(
module m;
    task t;
        #5;
    endtask

    always_comb begin
        t();
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == diag::BlockingDelayInTask);
}

TEST_CASE("DFA handles ambiguous conditional expression") {
    auto& code = R"(
module m(input l);
    int a;
    always_comb begin
        if (l) begin
            automatic int i = 'x ? (a = 1) : 3;
        end
        else begin
            a = 1;
        end
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("DFA general randsequence") {
    auto& code = R"(
module m;
    int a;
    int cnt;

    always_comb begin
        randsequence (main)
            main : first second done ;
            first : add("foo") | dec ;
            second : pop | push ;
            done : { $display("done"); return; } ;
            add(string s) : { $display(s); } ;
            dec : { begin : foo $display("dec"); break; end } ;
            pop : repeat($urandom_range( 2, 6 )) push;
            push : if (1) done else pop | rand join (0.5) first second done;
            baz : case (a & 7) 1, 2: push; 3: pop; default done; endcase;
        endsequence

        randsequence( bin_op )
            void bin_op : value operator value // void type is optional
            { $display("%s %b %b", operator, value[1], value[2]); }
            ;
            bit [7:0] value : { return 8'($urandom); } ;
            string operator : add := 5 { return "+" ; }
                            | dec := 2 { return "-" ; }
                            | mult := 1 { return "*" ; }
            ;
            add : { $display("add"); };
            dec : { $display("dec"); };
            mult : { $display("mult"); };
        endsequence

        cnt = 0;
        randsequence( A )
            void A : A1 A2;
            void A1 : { cnt = 1; } B repeat(5) C B
            { $display("c=%d, b1=%d, b2=%d", C, B[1], B[2]); }
            ;
            void A2 : if (a != 0) D(5) else D(20)
            { $display("d1=%d, d2=%d", D[1], D[2]); }
            ;
            int B : C { return C;}
                  | C C { return C[2]; }
                  | C C C { return C[3]; }
            ;
            int C : { cnt = cnt + 1; return cnt; };
            int D (int prm) : { return prm; };
        endsequence
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("Randsequence in unrolled for loop with side-effecting index -- GH #1941") {
    // The for loop gets unrolled while the randsequence production body modifies
    // the iteration variable via a side-effecting index (++i). This could bake in
    // an out-of-bounds constant select, producing an lvalue path with no static
    // prefix. Building a driver from such a path used to hit an internal assertion.
    auto& code = R"(
module t;
  int counts[8];
  initial begin
    for (int i = 0; i < 10; ++i) begin
      randsequence(main)
        main: count_1;
        count_1: { ++counts[++i]; };
      endsequence
    end
    $finish;
  end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("DFA unsequenced expression warnings") {
    auto& code = R"(
function void f1(input int a, output int b);
endfunction

function void f2(input int a, b);
endfunction

class C;
    function void foo(output C c); endfunction
    function void bar(C c); endfunction
endclass

module m;
    int i, j;
    C c;
    initial begin
        j = i++ + (i = i - 1);
        j = i++ + (i = 1);
        j[j++] = i; // ok
        j[j++] = j;
        j[(i = 1)] = i;
        j = (j = 1); // ok
        j += (j = 1);

        f1(i, i); // ok
        f1(i++, i);
        f2(i++, (i = 1));

        if ((++i || i == 1) + i) begin end

        c.foo(c);
        c.bar((c = null));
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    REQUIRE(diags.size() == 10);
    CHECK(diags[0].code == diag::MultiWriteExpr);
    CHECK(diags[1].code == diag::ReadWriteExpr);
    CHECK(diags[2].code == diag::MultiWriteExpr);
    CHECK(diags[3].code == diag::ReadWriteExpr);
    CHECK(diags[4].code == diag::ReadWriteExpr);
    CHECK(diags[5].code == diag::ReadWriteExpr);
    CHECK(diags[6].code == diag::ReadWriteExpr);
    CHECK(diags[7].code == diag::MultiWriteExpr);
    CHECK(diags[8].code == diag::ReadWriteExpr);
    CHECK(diags[9].code == diag::ReadWriteExpr);
}

TEST_CASE("DFA unsequenced expression with class members") {
    auto& code = R"(
class A;
    rand int x;
    A other;
    A next[4];

    function void f();
        int i;
        i = x++ + x;
        i = x++ + this.x;
        i = this.x++ + x;
        i = x++ + other.x; // ok

        // The handle expression is evaluated only once.
        i = other.next[i++].randomize() with { x < 1; this.x > 0; };
    endfunction
endclass
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    REQUIRE(diags.size() == 3);
    CHECK(diags[0].code == diag::ReadWriteExpr);
    CHECK(diags[1].code == diag::ReadWriteExpr);
    CHECK(diags[2].code == diag::ReadWriteExpr);
}

TEST_CASE("DFA unsequenced expression corner cases") {
    auto& code = R"(
module m;
    int i, j;
    initial begin
        // ok due to intra-assignment delay
        j[(i = 1)] = #3 i;

        if (++i || i == 1) begin end
        if (++i && i == 1) begin end
        if ((i = 1) ? i : i) begin end
    end

    function automatic void foo;
        logic [4:0] data[2];
        logic [4:0] a;
        logic b,c,d,e,f;
        '{ a, '{ b,c,d,e,f } } = data;
    endfunction
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("DFA unsequenced expression dynamic selects") {
    auto& code = R"(
module m;
    struct { logic a, b; } s[2];
    logic d[];
    int i;

    initial begin
        {d[0], d[1]} = 2;
        i = s[i].a++ + s[i].b++;

        // These should still error.
        {d[0], d[0]} = 2;
        i = s[i].a++ + s[i].a++;
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    REQUIRE(diags.size() == 2);
    CHECK(diags[0].code == diag::MultiWriteExpr);
    CHECK(diags[1].code == diag::MultiWriteExpr);
}

TEST_CASE("Fork loop var: basic for loop with join_none") {
    auto& code = R"(
module m;
    task automatic do_something(int x); endtask
    initial begin
        for (int i = 0; i < 4; i++) begin
            fork
                do_something(i);
            join_none
        end
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == diag::ForkLoopVar);
}

TEST_CASE("Fork loop var: no warning for fork-join (join_all)") {
    auto& code = R"(
module m;
    task automatic do_something(int x); endtask
    initial begin
        for (int i = 0; i < 4; i++) begin
            fork
                do_something(i);
            join
        end
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("Fork loop var using automatic local") {
    auto& code = R"(
module m;
    task automatic do_something(int x); endtask
    initial begin
        for (int i = 0; i < 4; i++) begin
            fork
                automatic int inst = i;
                do_something(inst);
            join_none
        end
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("Fork loop var: foreach loop variable") {
    auto& code = R"(
module m;
    task automatic do_something(int x); endtask
    initial begin
        int arr[4];
        foreach (arr[i]) begin
            fork
                do_something(i);
            join_none
        end
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == diag::ForkLoopVar);
}

TEST_CASE("Fork loop var: nested loops, only outer loop var referenced") {
    auto& code = R"(
module m;
    task automatic do_something(int x, int y); endtask
    initial begin
        for (int i = 0; i < 4; i++) begin
            for (int j = 0; j < 4; j++) begin
                fork
                    do_something(i, j);
                join_none
            end
        end
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    REQUIRE(diags.size() == 2);
    CHECK(diags[0].code == diag::ForkLoopVar);
    CHECK(diags[1].code == diag::ForkLoopVar);
}

TEST_CASE("Loop var double step: prefix increment in step and body") {
    auto& code = R"(
module m;
    initial begin
        for (int i = 0; i < 10; i++) begin
            i++;
        end
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == diag::LoopVarModify);
}

TEST_CASE("Loop var double step: compound add assignment in body") {
    auto& code = R"(
module m;
    initial begin
        for (int i = 0; i < 10; i++) begin
            i += 2;
        end
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == diag::LoopVarModify);
}

TEST_CASE("Loop var double step: no warning when only body modifies") {
    auto& code = R"(
module m;
    initial begin
        // No step expression that increments i; only body does it.
        for (int i = 0; i < 10; ) begin
            i++;
        end
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("Loop cond not modified: variable updated in body") {
    auto& code = R"(
module m;
    int limit;
    initial begin
        limit = 10;
        for (int i = 0; i < limit; ) begin
            i++;
            limit--;
        end
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("Loop cond not modified: all condition variables unmodified") {
    auto& code = R"(
module m;
    int lo, hi;
    initial begin
        lo = 0;
        hi = 10;
        for (int i = lo; i < hi; ) begin
        end
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == diag::LoopCondNotModified);
}

TEST_CASE("Loop cond not modified: nested loop regress") {
    auto& code = R"(
class C;
    function void f();
        logic [7:0][3:0] a;

        for (int i = 0; i < 8; i++) begin
            for (int unsigned j = 0; j < a[i]; j++) begin
            end
        end
    endfunction
endclass

module m;
    initial begin
        for (int i = 0; i < 10; i = i + 1) begin
        end
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("Nested loop: double-step detected in nested loop body") {
    auto& code = R"(
module m;
    initial begin
        for (int i = 0; i < 4; i++) begin
            for (int j = 0; j < 4; j++) begin
                i++;
            end
        end
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == diag::LoopVarModify);
}

TEST_CASE("Fork loop var: loop nested inside fork body does not warn") {
    auto& code = R"(
module m;
    task automatic do_something(int x); endtask
    initial begin
        for (int i = 0; i < 4; i++) begin
            fork
                for (int j = 0; j < 4; j++) begin
                    do_something(j);
                end
            join_none
        end
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("Const eval step limit in loop condition doesn't crash -- GH #1889") {
    auto& code = R"(
module t;
  function int unsigned foo(input int unsigned n);
    foo = 1;
    while (n != 0) begin
      n = n + 1;
    end
  endfunction

  initial begin
    while (foo(1) != 1) begin end
  end
endmodule
)";

    CompilationOptions options;
    options.maxConstexprSteps = 10;

    Compilation compilation(options);
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("Lvalue assignment pattern with multiple element conversions") {
    auto& code = R"(
module m;
    typedef enum { A, B } e_t;
    function automatic void foo;
        logic [4:0] data[2];
        e_t [4:0] a;
        logic b, c, d, e, f;
        '{ a, '{ b, c, d, e, f } } = data;
    endfunction
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("Analysis of nested class that extends a specialization of its enclosing class") {
    // All class specializations need to be created during elaboration;
    // analysis runs with a frozen compilation and can't create more.
    auto& code = R"(
package p;
    class Base #(type T = int);
        T t;
        class Config extends Base #(real);
            class Inner extends Base #(byte); endclass
        endclass
    endclass
endpackage

class Base2 #(type T = int);
    class Config extends Base2 #(real); endclass
endclass

module m;
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    CHECK_DIAGS_EMPTY;
}

TEST_CASE("Data flow through pattern-matching if clauses (SVMake AGG-f)") {
    // A `matches` clause's subject is the value matched, not a boolean condition: it must not be
    // folded as one, and the clause does not lose the state assigned before the if.
    auto& code = R"(
typedef union tagged packed { logic [7:0] V; logic [7:0] W; } tu_t;
module m;
    tu_t p;
    int z, y;
    localparam tu_t P = tagged V (8'd1);
    always_comb begin
        z = 1;
        if (p matches tagged V) ;
    end
    always_comb begin
        if (P matches tagged W) y = 1;
    end
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;

    auto diags = analyze(code, compilation, analysisManager);
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == diag::InferredLatch);   // y: the constant P may not match; z is always assigned
}

namespace {

// SVMake K-2437 helpers: analyze `code` with or without FullCaseUniquePriority and return the
// sorted analysis diagnostic codes, the driver counts of the named variables, and the stats.
struct FcupRun {
    std::vector<std::string> codes;
    std::vector<size_t> drivers;
    AnalysisManager::Stats stats;
};

FcupRun fcupRun(const std::string& code, bool fcup, std::initializer_list<std::string_view> vars) {
    AnalysisOptions options;
    if (fcup)
        options.flags |= AnalysisFlags::FullCaseUniquePriority;

    Compilation compilation;
    AnalysisManager analysisManager(options);
    auto diags = analyze(code, compilation, analysisManager);

    FcupRun result;
    for (auto& d : diags)
        result.codes.emplace_back(toString(d.code));
    std::ranges::sort(result.codes);

    // Walk the instance path with Scope::find (the compilation is frozen: no lookups that allocate).
    for (auto name : vars) {
        const Scope* scope = nullptr;
        const Symbol* sym = nullptr;
        size_t start = 0;
        while (true) {
            auto dot = name.find('.', start);
            auto part = name.substr(start, dot == std::string_view::npos ? dot : dot - start);
            if (!scope) {
                for (auto inst : compilation.getRoot().topInstances) {
                    if (inst->name == part)
                        sym = inst;
                }
            }
            else {
                sym = scope->find(part);
            }
            REQUIRE(sym);
            if (dot == std::string_view::npos)
                break;
            scope = &sym->as<InstanceSymbol>().body;
            start = dot + 1;
        }
        result.drivers.push_back(analysisManager.getDrivers(sym->as<ValueSymbol>()).size());
    }
    result.stats = analysisManager.getStats();
    return result;
}

std::vector<std::string> sortedCodes(std::initializer_list<std::string> codes) {
    std::vector<std::string> v(codes);
    std::ranges::sort(v);
    return v;
}

} // namespace

TEST_CASE("SVMake K-2437: FullCaseUniquePriority keeps the drivers and references of the code it assumes is not run") {
    // 12.5: "If all comparisons fail and the default item is given, then the default item statement
    // shall be executed"; FullCaseUniquePriority is an optimisation hint, not a proof that an arm is
    // dead. 9.2.2.2: an always_comb's variables "shall not be assigned by any other process";
    // 23.3.3.2: "Assignments to variables declared as input ports shall be illegal". With the flag,
    // each design below gives the FCUP-off drivers and driver diagnostics; the joined-flow codes
    // (InferredLatch, IncompleteReturn) stay what the flag gives.
    struct Case {
        const char* name;
        std::string code;
        std::initializer_list<std::string_view> vars;
        std::vector<std::string> on, off;
    };
    const Case cases[] = {
        {"fc2: a default-only writer that an initial also writes", R"(
module t;
  logic [1:0] s; logic a, b, y; int i;
  always_comb begin
    unique case (s)
      2'd0: y = a;
      2'd1: y = b;
      default: begin y = (a == a); i = i++ + i++; end
    endcase
  end
  initial begin s = 0; a = 1; b = 0; i = 0; end
endmodule
)",
         {"t.i"},
         sortedCodes({"MultipleAlwaysAssigns", "MultiWriteExpr"}),
         sortedCodes({"MultipleAlwaysAssigns", "MultiWriteExpr", "InferredLatch"})},
        {"fc_pri: the same with priority", R"(
module t;
  logic [1:0] s; logic a, b, y; int i;
  always_comb begin
    priority case (s)
      2'd0: y = a;
      2'd1: y = b;
      default: begin y = 1'b0; i = 1; end
    endcase
  end
  initial begin s = 0; a = 1; b = 0; i = 0; end
endmodule
)",
         {"t.i"},
         sortedCodes({"MultipleAlwaysAssigns"}),
         sortedCodes({"MultipleAlwaysAssigns", "InferredLatch"})},
        {"ip1: an input var port written in the default arm", R"(
module c(input var logic i, input logic [1:0] s, output logic y);
  always_comb unique case (s) 2'd0: y = 1; 2'd1: y = 0; default: begin i = 0; y = i; end endcase
endmodule
module t; logic a = 1, y; logic [1:0] s = 0; c u(.i(a), .s(s), .y(y)); endmodule
)",
         {"t.u.i"},
         sortedCodes({"InputPortAssign"}),
         sortedCodes({"InputPortAssign", "InferredLatch"})},
        {"fc3: a multi-write in the default arm", R"(
module t;
  logic [1:0] s; logic a, b, y; int i;
  always_comb begin
    unique case (s)
      2'd0: y = a;
      2'd1: y = b;
      default: begin y = 1'b0; i = i++ + i++; end
    endcase
  end
  initial begin s = 0; a = 1; b = 0; end
endmodule
)",
         {"t.i"},
         sortedCodes({"MultiWriteExpr"}),
         sortedCodes({"MultiWriteExpr", "InferredLatch"})},
        {"n1: no default, every item disables the block, a write after the case", R"(
module t;
  logic [1:0] s; logic y; int i;
  always_comb begin : b
    unique case (s)
      2'd0: begin y = 0; disable b; end
      2'd1: begin y = 1; disable b; end
    endcase
    i = 1;
  end
  initial begin s = 3; i = 0; end
endmodule
)",
         {"t.i"},
         sortedCodes({"MultipleAlwaysAssigns"}),
         sortedCodes({"MultipleAlwaysAssigns", "InferredLatch", "InferredLatch"})},
        {"n1f: a function, every item returns, no default, a write after the case", R"(
module t;
  logic [1:0] s; int i, y;
  function automatic int f(logic [1:0] s);
    unique case (s)
      2'd0: return 1;
      2'd1: return 2;
    endcase
    i = 1;
    return 0;
  endfunction
  always_comb y = f(s);
  initial begin s = 3; i = 0; end
endmodule
)",
         {"t.i"},
         sortedCodes({"MultipleAlwaysAssigns"}),
         sortedCodes({"MultipleAlwaysAssigns"})},
        {"n1b: a loop body, every item breaks, no default, a write after the case", R"(
module t;
  logic [1:0] s; logic y; int i;
  always_comb begin
    y = 0;
    for (int k = 0; k < 2; k++) begin
      unique case (s)
        2'd0: begin y = 1; break; end
        2'd1: begin y = 0; break; end
      endcase
      i = k;
    end
  end
  initial begin s = 3; i = 0; end
endmodule
)",
         {"t.i"},
         sortedCodes({"MultipleAlwaysAssigns"}),
         sortedCodes({"MultipleAlwaysAssigns", "InferredLatch"})},
        {"pc1: a constant selector whose known branch is the default", R"(
module t;
  localparam logic [1:0] P = 2'd3;
  logic y; int i;
  always_comb begin
    unique case (P)
      2'd0: y = 1'b0;
      2'd1: y = 1'b1;
      default: y = 1'bx;
    endcase
    i = 1;
  end
  initial begin i = 0; end
endmodule
)",
         {"t.i"},
         sortedCodes({"MultipleAlwaysAssigns"}),
         sortedCodes({"MultipleAlwaysAssigns"})},
        {"pc3: every item disables the block, default: ;, a write after the case", R"(
module t;
  logic [1:0] s; logic y; int i;
  always_comb begin : b
    unique case (s)
      2'd0: begin y = 0; disable b; end
      2'd1: begin y = 1; disable b; end
      default: y = 0;
    endcase
    i = 1;
  end
  initial begin s = 3; i = 0; end
endmodule
)",
         {"t.i"},
         sortedCodes({"MultipleAlwaysAssigns"}),
         sortedCodes({"MultipleAlwaysAssigns", "InferredLatch"})},
    };

    for (auto& c : cases) {
        INFO(c.name);
        auto on = fcupRun(c.code, true, c.vars);
        auto off = fcupRun(c.code, false, c.vars);
        CHECK(on.codes == c.on);
        CHECK(off.codes == c.off);
        CHECK(on.drivers == off.drivers);
        CHECK(on.stats.numFlowOnlyPasses >= 1);
        CHECK(off.stats.numFlowOnlyPasses == 0);
    }
}

TEST_CASE("SVMake K-2437: the joined-flow codes keep FullCaseUniquePriority's answer") {
    // What the flag is for: a unique case whose items cover the values in use is not a latch, and a
    // function whose items all return does not fall off its end. These stay as the flag gives them.
    struct Case {
        const char* name;
        std::string code;
        std::vector<std::string> on, off;
    };
    const Case cases[] = {
        {"fc_latch: default: ;", R"(
module t;
  logic [1:0] s; logic a, y;
  always_comb begin
    unique case (s)
      2'd0: y = a;
      2'd1: y = ~a;
      default: ;
    endcase
  end
endmodule
)",
         {}, sortedCodes({"InferredLatch"})},
        {"n2: every item returns, no default", R"(
module t;
  function automatic int f(logic [1:0] s);
    unique case (s)
      2'd0: return 1;
      2'd1: return 2;
    endcase
  endfunction
endmodule
)",
         {}, sortedCodes({"IncompleteReturn"})},
        {"n3: every item returns, default: ;", R"(
module t;
  function automatic int f(logic [1:0] s);
    unique case (s)
      2'd0: return 1;
      2'd1: return 2;
      default: ;
    endcase
  endfunction
endmodule
)",
         {}, sortedCodes({"IncompleteReturn"})},
        {"n4: default: disable b;", R"(
module t;
  logic [1:0] s; logic y;
  always_comb begin : b
    unique case (s)
      2'd0: y = 0;
      2'd1: y = 1;
      default: disable b;
    endcase
  end
endmodule
)",
         {}, sortedCodes({"InferredLatch"})},
        {"n5: a default-only error flag", R"(
module t;
  logic [1:0] s; logic y, err;
  always_comb begin
    unique case (s)
      2'd0: y = 0;
      2'd1: y = 1;
      default: begin y = 'x; err = 1; end
    endcase
  end
endmodule
)",
         {}, sortedCodes({"InferredLatch"})},
        {"n6: default: break; in a loop", R"(
module t;
  logic [1:0] s; logic y;
  always_comb begin
    y = 0;
    for (int k = 0; k < 4; k++) begin
      unique case (s)
        2'd0: y = 1;
        2'd1: y = 0;
        default: break;
      endcase
    end
  end
endmodule
)",
         {}, {}},
    };

    for (auto& c : cases) {
        INFO(c.name);
        CHECK(fcupRun(c.code, true, {}).codes == c.on);
        CHECK(fcupRun(c.code, false, {}).codes == c.off);
    }
}

TEST_CASE("SVMake K-2437: the flow-only pass runs only where FullCaseUniquePriority changes the flow") {
    auto run = [](const std::string& code) { return fcupRun(code, true, {}).stats.numFlowOnlyPasses; };

    // No unique or priority case.
    CHECK(run(R"(
module t; logic [1:0] s; logic y;
  always_comb case (s) 2'd0: y = 0; default: y = 1; endcase
endmodule
)") == 0);

    // A unique case proven to cover its (2-state) input: the flag changes nothing.
    CHECK(run(R"(
module t; bit [1:0] s; logic y; int i;
  always_comb unique case (s) 2'd0, 2'd1: y = 0; 2'd2, 2'd3: y = 1; default: i = 1; endcase
endmodule
)") == 0);

    // unique0 is not assumed full by the flag.
    CHECK(run(R"(
module t; logic [1:0] s; logic y;
  always_comb unique0 case (s) 2'd0: y = 0; 2'd1: y = 1; endcase
endmodule
)") == 0);

    // One procedure with an assumed case, one without.
    CHECK(run(R"(
module t; logic [1:0] s; logic y, z;
  always_comb unique case (s) 2'd0: y = 0; 2'd1: y = 1; default: y = 0; endcase
  always_comb z = s[0];
endmodule
)") == 1);
}

TEST_CASE("SVMake K-2433: an always procedure without timing control is a default-on warning") {
    // 9.2.2.1: "If an always procedure has no control for simulation time to advance, it will create
    // a simulation deadlock condition." A consequence, not a rule the design breaks: a warning in
    // the default group with its own option, so -Wno-, -Werror= and pragmas reach it.
    auto& code = R"(
module t;
    int n = 0;
    always n = n + 1;
endmodule
)";

    Compilation compilation;
    AnalysisManager analysisManager;
    auto diags = analyze(code, compilation, analysisManager);
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == diag::AlwaysWithoutTimingControl);
    CHECK(!diags[0].isError());

    auto& sm = SyntaxTree::getDefaultSourceManager();
    DiagnosticEngine engine(sm);
    CHECK(engine.getOptionName(diag::AlwaysWithoutTimingControl) == "always-no-timing");
    REQUIRE(engine.findFromOptionName("always-no-timing").size() == 1);

    auto severityWith = [&](std::vector<std::string> options) {
        DiagnosticEngine e(sm);
        e.setWarningOptions(options);
        return e.getSeverity(diag::AlwaysWithoutTimingControl, diags[0].location);
    };
    CHECK(severityWith({}) == DiagnosticSeverity::Warning);   // "default" is on unless turned off
    CHECK(severityWith({"no-unused"}) == DiagnosticSeverity::Warning);
    CHECK(severityWith({"no-always-no-timing"}) == DiagnosticSeverity::Ignored);
    CHECK(severityWith({"error=always-no-timing"}) == DiagnosticSeverity::Error);
}
