// SVMake VAMS P0.1 Driver test: an unmapped SystemVerilog file using VAMS keywords as names.
`include "c1body.vh"
module c1plain;
  logic ddt, analog, sin;
  localparam y = `C1_BODY + (2<+3);
endmodule
