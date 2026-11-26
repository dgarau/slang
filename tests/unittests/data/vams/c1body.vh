// SVMake VAMS P0.1: an unmapped include. Included from VAMS source it is VAMS source
// (__VAMS_ENABLE__ defined, 1k a scaled real); included from SystemVerilog it is not.
`ifdef __VAMS_ENABLE__
`define C1_BODY 1k
`else
`define C1_BODY 0
`endif
