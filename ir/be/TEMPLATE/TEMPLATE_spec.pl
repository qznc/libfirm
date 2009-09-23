# Creation: 2006/02/13
# $Id$
# This is a template specification for the Firm-Backend

$new_emit_syntax = 1;

# the cpu architecture (ia32, ia64, mips, sparc, ppc, ...)

$arch = "TEMPLATE";

# The node description is done as a perl hash initializer with the
# following structure:
#
# %nodes = (
#
# <op-name> => {
#   op_flags  => "N|L|C|X|I|F|Y|H|c|K",                 # optional
#   irn_flags => "R|N|I"                                # optional
#   arity     => "0|1|2|3 ... |variable|dynamic|any",   # optional
#   state     => "floats|pinned|mem_pinned|exc_pinned", # optional
#   args      => [
#                    { type => "type 1", name => "name 1" },
#                    { type => "type 2", name => "name 2" },
#                    ...
#                  ],
#   comment   => "any comment for constructor",  # optional
#   reg_req   => { in => [ "reg_class|register" ], out => [ "reg_class|register|in_rX" ] },
#   cmp_attr  => "c source code for comparing node attributes", # optional
#   outs      => { "out1", "out2" },# optional, creates pn_op_out1, ... consts
#   ins       => { "in1", "in2" },  # optional, creates n_op_in1, ... consts
#   mode      => "mode_Iu",         # optional, predefines the mode
#   emit      => "emit code with templates",   # optional for virtual nodes
#   attr      => "additional attribute arguments for constructor", # optional
#   init_attr => "emit attribute initialization template",         # optional
#   rd_constructor => "c source code which constructs an ir_node", # optional
#   hash_func => "name of the hash function for this operation",   # optional, get the default hash function else
#   latency   => "latency of this operation (can be float)"        # optional
#   attr_type => "name of the attribute struct",                   # optional
# },
#
# ... # (all nodes you need to describe)
#
# ); # close the %nodes initializer

# op_flags: flags for the operation, OPTIONAL (default is "N")
# the op_flags correspond to the firm irop_flags:
#   N   irop_flag_none
#   L   irop_flag_labeled
#   C   irop_flag_commutative
#   X   irop_flag_cfopcode
#   I   irop_flag_ip_cfopcode
#   F   irop_flag_fragile
#   Y   irop_flag_forking
#   H   irop_flag_highlevel
#   c   irop_flag_constlike
#   K   irop_flag_keep
#
# irn_flags: special node flags, OPTIONAL (default is 0)
# following irn_flags are supported:
#   R   rematerializeable
#   N   not spillable
#   I   ignore for register allocation
#
# state: state of the operation, OPTIONAL (default is "floats")
#
# arity: arity of the operation, MUST NOT BE OMITTED
#
# args:  the OPTIONAL arguments of the node constructor (debug, irg and block
#        are always the first 3 arguments and are always autmatically
#        created)
#        If this key is missing the following arguments will be created:
#        for i = 1 .. arity: ir_node *op_i
#        ir_mode *mode
#
# outs:  if a node defines more than one output, the names of the projections
#        nodes having outs having automatically the mode mode_T
#
# comment: OPTIONAL comment for the node constructor
#
# rd_constructor: for every operation there will be a
#      new_rd_<arch>_<op-name> function with the arguments from above
#      which creates the ir_node corresponding to the defined operation
#      you can either put the complete source code of this function here
#
#      This key is OPTIONAL. If omitted, the following constructor will
#      be created:
#      if (!op_<arch>_<op-name>) assert(0);
#      for i = 1 to arity
#         set in[i] = op_i
#      done
#      res = new_ir_node(db, irg, block, op_<arch>_<op-name>, mode, arity, in)
#      return res
#
# NOTE: rd_constructor and args are only optional if and only if arity is 0,1,2 or 3

# register types:
#   0 - no special type
#   1 - caller save (register must be saved by the caller of a function)
#   2 - callee save (register must be saved by the called function)
#   4 - ignore (do not assign this register)
# NOTE: Last entry of each class is the largest Firm-Mode a register can hold
%reg_classes = (
	gp => [
		{ name => "r0", type => 1 },
		{ name => "r1", type => 1 },
		{ name => "r2", type => 1 },
		{ name => "r3", type => 1 },
		{ name => "r4", type => 1 },
		{ name => "r5", type => 1 },
		{ name => "r6", type => 1 },
		{ name => "r7", type => 2 },
		{ name => "r8", type => 2 },
		{ name => "r9", type => 2 },
		{ name => "r10", type => 2 },
		{ name => "r11", type => 2 },
		{ name => "r12", type => 2 },
		{ name => "r13", type => 2 },
		{ name => "sp", realname => "r14", type => 4 },  # stackpointer
		{ name => "bp", realname => "r15", type => 4 },  # basepointer
		{ mode => "mode_Iu" }
	],
	fp => [
		{ name => "f0", type => 1 },
		{ name => "f1", type => 1 },
		{ name => "f2", type => 1 },
		{ name => "f3", type => 1 },
		{ name => "f4", type => 1 },
		{ name => "f5", type => 1 },
		{ name => "f6", type => 1 },
		{ name => "f7", type => 1 },
		{ name => "f8", type => 1 },
		{ name => "f9", type => 1 },
		{ name => "f10", type => 1 },
		{ name => "f11", type => 1 },
		{ name => "f12", type => 1 },
		{ name => "f13", type => 1 },
		{ name => "f14", type => 1 },
		{ name => "f15", type => 1 },
		{ mode => "mode_D" }
	]
);

%emit_templates = (
	S1 => "${arch}_emit_source_register(node, 0);",
	S2 => "${arch}_emit_source_register(node, 1);",
	S3 => "${arch}_emit_source_register(node, 2);",
	S4 => "${arch}_emit_source_register(node, 3);",
	S5 => "${arch}_emit_source_register(node, 4);",
	S6 => "${arch}_emit_source_register(node, 5);",
	D1 => "${arch}_emit_dest_register(node, 0);",
	D2 => "${arch}_emit_dest_register(node, 1);",
	D3 => "${arch}_emit_dest_register(node, 2);",
	D4 => "${arch}_emit_dest_register(node, 3);",
	D5 => "${arch}_emit_dest_register(node, 4);",
	D6 => "${arch}_emit_dest_register(node, 5);",
	C  => "${arch}_emit_immediate(node);"
);

%nodes = (

# Integer nodes

Add => {
	op_flags  => "C",
	irn_flags => "R",
	reg_req   => { in => [ "gp", "gp" ], out => [ "gp" ] },
	emit      => '. add %S1, %S2, %D1'
},

Add_i => {
	irn_flags => "R",
	reg_req   => { in => [ "gp" ], out => [ "gp" ] },
	emit      => '. add %S1, %C, %D1'
},

Mul => {
	op_flags  => "C",
	irn_flags => "R",
	reg_req   => { in => [ "gp", "gp" ], out => [ "gp" ] },
	emit      =>'. mul %S1, %S2, %D1'
},

Mul_i => {
	irn_flags => "R",
	reg_req   => { in => [ "gp" ], out => [ "gp" ] },
	emit      => '. mul %S1, %C, %D1'
},

And => {
	op_flags  => "C",
	irn_flags => "R",
	reg_req   => { in => [ "gp", "gp" ], out => [ "gp" ] },
	emit      => '. and %S1, %S2, %D1'
},

And_i => {
	irn_flags => "R",
	reg_req   => { in => [ "gp" ], out => [ "gp" ] },
	emit      => '. and %S1, %C, %D1'
},

Or => {
	op_flags  => "C",
	irn_flags => "R",
	reg_req   => { in => [ "gp", "gp" ], out => [ "gp" ] },
	emit      => '. or %S1, %S2, %D1'
},

Or_i => {
	op_flags  => "C",
	irn_flags => "R",
	reg_req   => { in => [ "gp" ], out => [ "gp" ] },
	emit      => '. or %S1, %C, %D1'
},

Eor => {
	op_flags  => "C",
	irn_flags => "R",
	reg_req   => { in => [ "gp", "gp" ], out => [ "gp" ] },
	emit      => '. xor %S1, %S2, %D1'
},

Eor_i => {
	irn_flags => "R",
	reg_req   => { in => [ "gp" ], out => [ "gp" ] },
	emit      => '. xor %S1, %C, %D1'
},

Sub => {
	irn_flags => "R",
	reg_req   => { in => [ "gp", "gp" ], out => [ "gp" ] },
	emit      => '. sub %S1, %S2, %D1'
},

Sub_i => {
	irn_flags => "R",
	reg_req   => { in => [ "gp" ], out => [ "gp" ] },
	emit      => '. subl %S1, %C, %D1'
},

Shl => {
	irn_flags => "R",
	reg_req   => { in => [ "gp", "gp" ], out => [ "gp" ] },
	emit      => '. shl %S1, %S2, %D1'
},

Shl_i => {
	irn_flags => "R",
	reg_req   => { in => [ "gp" ], out => [ "gp" ] },
	emit      => '. shl %S1, %C, %D1'
},

Shr => {
	irn_flags => "R",
	reg_req   => { in => [ "gp", "gp" ], out => [ "in_r1" ] },
	emit      => '. shr %S2, %D1'
},

Shr_i => {
	irn_flags => "R",
	reg_req   => { in => [ "gp" ], out => [ "gp" ] },
	emit      => '. shr %S1, %C, %D1'
},

RotR => {
	irn_flags => "R",
	reg_req   => { in => [ "gp", "gp" ], out => [ "gp" ] },
	emit      => '. ror %S1, %S2, %D1'
},

RotL => {
	irn_flags => "R",
	reg_req   => { in => [ "gp", "gp" ], out => [ "gp" ] },
	emit      => '. rol %S1, %S2, %D1'
},

RotL_i => {
	irn_flags => "R",
	reg_req   => { in => [ "gp" ], out => [ "gp" ] },
	emit      => '. rol %S1, %C, %D1'
},

Minus => {
	irn_flags => "R",
	reg_req   => { in => [ "gp" ], out => [ "gp" ] },
	emit      => '. neg %S1, %D1'
},

Inc => {
	irn_flags => "R",
	reg_req   => { in => [ "gp" ], out => [ "gp" ] },
	emit      => '. inc %S1, %D1'
},

Dec => {
	irn_flags => "R",
	reg_req   => { in => [ "gp" ], out => [ "gp" ] },
	emit      => '. dec %S1, %D1'
},

Not => {
	arity       => 1,
	remat       => 1,
	reg_req     => { in => [ "gp" ], out => [ "gp" ] },
	emit        => '. not %S1, %D1'
},

Const => {
	op_flags  => "c",
	irn_flags => "R",
	reg_req   => { out => [ "gp" ] },
	emit      => '. mov %C, %D1',
	cmp_attr  =>
'
	/* TODO: compare Const attributes */
    return 1;
'
},

# Control Flow

Jmp => {
	state    => "pinned",
	op_flags => "X",
	reg_req  => { out => [ "none" ] },
	mode     => "mode_X",
},

# Load / Store

Load => {
	op_flags  => "L|F",
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "none" ], out => [ "gp" ] },
	emit      => '. mov (%S1), %D1'
},

Store => {
	op_flags  => "L|F",
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none" ] },
	emit      => '. movl %S2, (%S1)'
},

# Floating Point operations

fAdd => {
	op_flags  => "C",
	irn_flags => "R",
	reg_req   => { in => [ "fp", "fp" ], out => [ "fp" ] },
	emit      => '. fadd %S1, %S2, %D1'
},

fMul => {
	op_flags  => "C",
	reg_req   => { in => [ "fp", "fp" ], out => [ "fp" ] },
	emit      =>'. fmul %S1, %S2, %D1'
},

fMax => {
	op_flags  => "C",
	irn_flags => "R",
	reg_req   => { in => [ "fp", "fp" ], out => [ "fp" ] },
	emit      =>'. fmax %S1, %S2, %D1'
},

fMin => {
	op_flags  => "C",
	irn_flags => "R",
	reg_req   => { in => [ "fp", "fp" ], out => [ "fp" ] },
	emit      =>'. fmin %S1, %S2, %D1'
},

fSub => {
	irn_flags => "R",
	reg_req   => { in => [ "fp", "fp" ], out => [ "fp" ] },
	emit      => '. fsub %S1, %S2, %D1'
},

fDiv => {
	reg_req   => { in => [ "fp", "fp" ], out => [ "fp" ] },
	emit      => '. fdiv %S1, %S2, %D1'
},

fMinus => {
	irn_flags => "R",
	reg_req   => { in => [ "fp" ], out => [ "fp" ] },
	emit      => '. fneg %S1, %D1'
},

fConst => {
	op_flags  => "c",
	irn_flags => "R",
	reg_req   => { out => [ "fp" ] },
	emit      => '. fmov %C, %D1',
	cmp_attr  =>
'
	/* TODO: compare fConst attributes */
	return 1;
'
},

# Load / Store

fLoad => {
	op_flags  => "L|F",
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "none" ], out => [ "fp" ] },
	emit      => '. fmov (%S1), %D1'
},

fStore => {
	op_flags  => "L|F",
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "fp", "none" ] },
	emit      => '. fmov %S2, (%S1)'
},

);
