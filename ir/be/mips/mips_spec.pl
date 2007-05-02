# Creation: 2006/02/13
# $Id$
# This is a template specification for the Firm-Backend

# the cpu architecture (ia32, ia64, mips, sparc, ppc, ...)

$arch = "mips";
$new_emit_syntax = 1;

# The node description is done as a perl hash initializer with the
# following structure:
#
# %nodes = (
#
# <op-name> => {
#   op_flags  => "N|L|C|X|I|F|Y|H|c|K",
#   "arity"     => "0|1|2|3 ... |variable|dynamic|any",
#   "state"     => "floats|pinned|mem_pinned|exc_pinned",
#   "args"      => [
#                    { "type" => "type 1", "name" => "name 1" },
#                    { "type" => "type 2", "name" => "name 2" },
#                    ...
#                  ],
#   "comment"   => "any comment for constructor",
#   reg_req   => { in => [ "reg_class|register" ], out => [ "reg_class|register|in_rX" ] },
#   "cmp_attr"  => "c source code for comparing node attributes",
#   emit      => "emit code with templates",
#   "rd_constructor" => "c source code which constructs an ir_node"
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
# NOTE: Last entry of each class is the largest Firm-Mode a register can hold\
%reg_classes = (
	"gp" => [
		{ name => "zero", type => 4+2 },  # always zero
		{ name => "at", type => 4 }, # reserved for assembler
		{ name => "v0", realname => "2", type => 1 }, # first return value
		{ name => "v1", realname => "3", type => 1 }, # second return value
		{ name => "a0", realname => "4", type => 1 }, # first argument
		{ name => "a1", realname => "5", type => 1 }, # second argument
		{ name => "a2", realname => "6", type => 1 }, # third argument
		{ name => "a3", realname => "7", type => 1 }, # fourth argument
		{ name => "t0", realname => "8", type => 1 },
		{ name => "t1", realname => "9", type => 1 },
		{ name => "t2", realname => "10", type => 1 },
		{ name => "t3", realname => "11", type => 1 },
		{ name => "t4", realname => "12", type => 1 },
		{ name => "t5", realname => "13", type => 1 },
		{ name => "t6", realname => "14", type => 1 },
		{ name => "t7", realname => "15", type => 1 },
		{ name => "s0", realname => "16", type => 2 },
		{ name => "s1", realname => "17", type => 2 },
		{ name => "s2", realname => "18", type => 2 },
		{ name => "s3", realname => "19", type => 2 },
		{ name => "s4", realname => "20", type => 2 },
		{ name => "s5", realname => "21", type => 2 },
		{ name => "s6", realname => "22", type => 2 },
		{ name => "s7", realname => "23", type => 2 },
		{ name => "t8", realname => "24", type => 1 },
		{ name => "t9", realname => "25", type => 1 },
		{ name => "kt0", type => 4 }, # reserved for OS
		{ name => "kt1", type => 4 }, # reserved for OS
		{ name => "gp", type => 4 }, # general purpose
		{ name => "sp", type => 4 }, # stack pointer
		{ name => "fp", type => 4 }, # frame pointer
		{ name => "ra", type => 2+1 }, # return address
		{ mode => "mode_Iu" }
	],
); # %reg_classes

%emit_templates = (
    S1 => "${arch}_emit_source_register(env, node, 0);",
    S2 => "${arch}_emit_source_register(env, node, 1);",
    S3 => "${arch}_emit_source_register(env, node, 2);",
    D1 => "${arch}_emit_dest_register(env, node, 0);",
    D2 => "${arch}_emit_dest_register(env, node, 1);",
    D3 => "${arch}_emit_dest_register(env, node, 2);",
	C  => "${arch}_emit_immediate(env, node);",
	JumpTarget => "${arch}_emit_jump_target(env, node);",
	JumpTarget1 => "${arch}_emit_jump_target_proj(env, node, 1);",
);


#--------------------------------------------------#
#                        _                         #
#                       (_)                        #
#  _ __   _____      __  _ _ __    ___  _ __  ___  #
# | '_ \ / _ \ \ /\ / / | | '__|  / _ \| '_ \/ __| #
# | | | |  __/\ V  V /  | | |    | (_) | |_) \__ \ #
# |_| |_|\___| \_/\_/   |_|_|     \___/| .__/|___/ #
#                                      | |         #
#                                      |_|         #
#--------------------------------------------------#

%nodes = (

#-----------------------------------------------------------------#
#  _       _                                         _            #
# (_)     | |                                       | |           #
#  _ _ __ | |_ ___  __ _  ___ _ __   _ __   ___   __| | ___  ___  #
# | | '_ \| __/ _ \/ _` |/ _ \ '__| | '_ \ / _ \ / _` |/ _ \/ __| #
# | | | | | ||  __/ (_| |  __/ |    | | | | (_) | (_| |  __/\__ \ #
# |_|_| |_|\__\___|\__, |\___|_|    |_| |_|\___/ \__,_|\___||___/ #
#                   __/ |                                         #
#                  |___/                                          #
#-----------------------------------------------------------------#

# commutative operations

addu => {
	op_flags  => "C",
	reg_req   => { in => [ "gp", "gp" ], out => [ "gp" ] },
	emit      => '. addu %D1, %S1, %S2',
	mode      => "mode_Iu",
},

addiu => {
	reg_req   => { in => [ "gp" ], out => [ "gp" ] },
	emit      => '. addiu %D1, %S1, %C',
	cmp_attr  => 'return attr_a->tv != attr_b->tv;',
	mode      => "mode_Iu",
},

and => {
	op_flags  => "C",
	reg_req   => { in => [ "gp", "gp" ], out => [ "gp" ] },
	emit      => '. and %D1, %S1, %S2',
	mode      => "mode_Iu",
},

andi => {
	reg_req   => { in => [ "gp" ], out => [ "gp" ] },
	emit      => '. andi %D1, %S1, %C',
	cmp_attr  => 'return attr_a->tv != attr_b->tv;',
	mode      => "mode_Iu",
},

div => {
	reg_req   => { in => [ "gp", "gp" ], out => [ "none" ] },
	emit      => '. div %S1, %S2',
	mode      => "mode_M",
},

divu => {
	reg_req   => { in => [ "gp", "gp" ], out => [ "none" ] },
	emit      => '. divu %S1, %S2',
	mode      => "mode_M",
},

mult => {
	op_flags  => "C",
	reg_req   => { in => [ "gp", "gp" ], out => [ "none" ] },
	emit      => '. mult %S1, %S2',
	mode      => "mode_M"
},

multu => {
	op_flags  => "C",
	reg_req   => { in => [ "gp", "gp" ], out => [ "none" ] },
	emit      => '. multu %S1, %S2',
	mode      => "mode_M",
},

nor => {
	op_flags  => "C",
	reg_req   => { in => [ "gp", "gp" ], out => [ "gp" ] },
	emit      => '. nor %D1, %S1, %S2',
	mode      => "mode_Iu"
},

not => {
	reg_req   => { in => [ "gp" ], out => [ "gp" ] },
	emit      => '. nor %D1, %S1, $zero',
	mode      => "mode_Iu"
},

or => {
	op_flags  => "C",
	reg_req   => { in => [ "gp", "gp" ], out => [ "gp" ] },
	emit      => '. or %D1, %S1, %S2',
	mode      => "mode_Iu"
},

ori => {
	reg_req   => { in => [ "gp" ], out => [ "gp" ] },
	emit      => '. ori %D1, %S1, %C',
	cmp_attr  => 'return attr_a->tv != attr_b->tv;',
	mode      => "mode_Iu"
},

sl => {
	reg_req   => { in => [ "gp", "gp" ], out => [ "gp" ] },
	emit      => '
	if (mode_is_signed(get_irn_mode(node))) {
		. sal %D1, %S1, %S2
	} else {
		. sll %D1, %S1, %S2
	}
',
	mode      => "mode_Iu",
},

sli => {
	reg_req   => { in => [ "gp" ], out => [ "gp" ] },
	emit      => '
	if (mode_is_signed(get_irn_mode(node))) {
		. sal %D1, %S1, %C
	} else {
		. sll %D1, %S1, %C
	}
',
	mode      => "mode_Iu",
},

sra => {
	reg_req   => { in => [ "gp", "gp" ], out => [ "gp" ] },
	emit      => '. sra %D1, %S1, %S2',
	mode      => "mode_Iu"
},

srai => {
	reg_req   => { in => [ "gp" ], out => [ "gp" ] },
	emit      => '. sra %D1, %S1, %C',
	cmp_attr  => 'return attr_a->tv != attr_b->tv;',
	mode      => "mode_Iu",
},

sr => {
	reg_req   => { in => [ "gp", "gp" ], out => [ "gp" ] },
	emit      => '
	if (mode_is_signed(get_irn_mode(node))) {
		. sra %D1, %S1, %S2
	} else {
		. srl %D1, %S1, %S2
	}
',
	mode      => "mode_Iu",
},

sri => {
	reg_req   => { in => [ "gp" ], out => [ "gp" ] },
	emit      => '
	if (mode_is_signed(get_irn_mode(node))) {
		. sra %D1, %S1, %C
	} else {
		. srl %D1, %S1, %C
	}
',
	mode      => "mode_Iu"
},

srlv => {
	reg_req   => { in => [ "gp", "gp" ], out => [ "gp" ] },
	emit      => '. srlv %D1, %S1, %S2',
	mode      => "mode_Iu"
},

sllv => {
	reg_req   => { in => [ "gp", "gp" ], out => [ "gp" ] },
	emit      => '. sllv %D1, %S1, %S2',
	mode      => "mode_Iu"
},

sub => {
	reg_req   => { in => [ "gp", "gp" ], out => [ "gp" ] },
	emit      => '. subu %D1, %S1, %S2',
	mode      => "mode_Iu"
},

subuzero => {
	reg_req	  => { in => [ "gp" ], out => [ "gp" ] },
	emit      => '. subu %D1, $zero, %S1',
	mode      => "mode_Iu",
},

xor => {
	reg_req   => { in => [ "gp", "gp" ], out => [ "gp" ] },
	emit      => '. xor %D1, %S1, %S2',
	mode      => "mode_Iu",
},

xori => {
	reg_req   => { in => [ "gp" ], out => [ "gp" ] },
	emit      => '. xori %D1, %S1, %C',
	cmp_attr  => 'return attr_a->tv != attr_b->tv;',
	mode      => "mode_Iu",
},

#   ____                _              _
#  / ___|___  _ __  ___| |_ __ _ _ __ | |_ ___
# | |   / _ \| '_ \/ __| __/ _` | '_ \| __/ __|
# | |__| (_) | | | \__ \ || (_| | | | | |_\__ \
#  \____\___/|_| |_|___/\__\__,_|_| |_|\__|___/
#

# load upper imediate
lui => {
	op_flags  => "c",
	reg_req   => { out => [ "gp" ] },
	emit      => '. lui %D1, %C',
	cmp_attr  => 'return attr_a->tv != attr_b->tv;',
	mode      => "mode_Iu",
},

# load lower immediate
lli => {
	op_flags  => "c",
	reg_req   => { in => [ "gp" ], out => [ "gp" ] },
	emit      => '. ori %D1, %S1, %C',
	cmp_attr  => 'return attr_a->tv != attr_b->tv;',
	mode      => "mode_Iu",
},

la => {
	op_flags  => "c",
	reg_req   => { out => [ "gp" ] },
	emit      => '. la %D1, %C',
	cmp_attr  => 'return attr_a->symconst_id != attr_b->symconst_id;',
	mode      => "mode_Iu",
},

mflo => {
	reg_req   => { in => [ "none" ], out => [ "gp" ] },
	emit      => '. mflo %D1',
	mode      => "mode_Iu"
},

mfhi => {
	reg_req   => { in => [ "none" ], out => [ "gp" ] },
	emit      => '. mfhi %D1',
	mode      => "mode_Iu"
},

zero => {
	reg_req   => { out => [ "zero" ] },
	emit      => '',
	mode      => "mode_Iu"
},

#
#  ____                       _        __  _
# | __ ) _ __ __ _ _ __   ___| |__    / / | |_   _ _ __ ___  _ __
# |  _ \| '__/ _` | '_ \ / __| '_ \  / /  | | | | | '_ ` _ \| '_ \
# | |_) | | | (_| | | | | (__| | | |/ / |_| | |_| | | | | | | |_) |
# |____/|_|  \__,_|_| |_|\___|_| |_/_/ \___/ \__,_|_| |_| |_| .__/
#                                                           |_|
#

slt => {
	reg_req => { in => [ "gp", "gp" ], out => [ "gp" ] },
	emit => '
	if (mode_is_signed(get_irn_mode(node))) {
		. slt %D1, %S1, %S2
	}
	else {
		. sltu %D1, %S1, %S2
	}
',
	mode    => "mode_Iu",
},

slti => {
	reg_req => { in => [ "gp" ], out => [ "gp" ] },
	emit => '
	if (mode_is_signed(get_irn_mode(node))) {
		. slti %D1, %S1, %C
	}
	else {
		. sltiu %D1, %S1, %C
	}
',
	cmp_attr => 'return attr_a->tv != attr_b->tv;',
	mode    => "mode_Iu",
},

beq => {
	op_flags  => "X|Y",
	# TxT -> TxX
	reg_req => { in => [ "gp", "gp" ], out => [ "in_r0", "none" ] },
	emit => '. beq %S1, %S2, %JumpTarget1'
},

bne => {
	op_flags  => "X|Y",
	# TxT -> TxX
	reg_req => { in => [ "gp", "gp" ], out => [ "in_r0", "none" ] },
	emit => '. bne %S1, %S2, %JumpTarget1'
},

bgtz => {
	op_flags  => "X|Y",
	# TxT -> TxX
	reg_req => { in => [ "gp" ], out => [ "in_r0", "none" ] },
	emit => '. bgtz %S1, %JumpTarget1'
},

blez => {
	op_flags  => "X|Y",
	# TxT -> TxX
	reg_req => { in => [ "gp" ], out => [ "in_r0", "none" ] },
	emit => '. blez %S1, %JumpTarget1'
},

j => {
  op_flags => "X",
  reg_req => { in => [ "gp" ] },
  emit => '. j %S1',
},

b => {
	op_flags => "X",
	# -> X
	reg_req => { in => [ ], out => [ "none" ] },
	emit => '. b %JumpTarget'
},

fallthrough => {
	op_flags => "X",
	# -> X
	reg_req => { in => [ ], out => [ "none" ] },
	emit => '. /* fallthrough to %JumpTarget */'
},

SwitchJump => {
	op_flags => "X",
	# -> X,X,...
	reg_req => { in => [ "gp" ], out => [ "none" ] },
	emit => '. j %S1'
},

#  _                    _
# | |    ___   __ _  __| |
# | |   / _ \ / _` |/ _` |
# | |__| (_) | (_| | (_| |
# |_____\___/ \__,_|\__,_|
#

load_r => {
	reg_req	=> { in => [ "none", "gp" ], out => [ "none", "none", "gp" ] },
	emit => '
	mips_attr_t* attr = get_mips_attr(node);
	ir_mode *mode;

	mode = attr->modes.load_store_mode;

	switch (get_mode_size_bits(mode)) {
	case 8:
		if (mode_is_signed(mode)) {
			. lb %D3, %C(%S2)
		} else {
			. lbu %D3, %C(%S2)
		}
		break;
	case 16:
		if (mode_is_signed(mode)) {
			. lh %D3, %C(%S2)
		} else {
			. lhu %D3, %C(%S2)
		}
		break;
	case 32:
		. lw %D3, %C(%S2)
		break;
	default:
		assert(! "Only 8, 16 and 32 bit loads supported");
		break;
	}
',
	cmp_attr => 'return attr_a->tv != attr_b->tv || attr_a->stack_entity != attr_b->stack_entity;',
},


#  _                    _    ______  _
# | |    ___   __ _  __| |  / / ___|| |_ ___  _ __ ___
# | |   / _ \ / _` |/ _` | / /\___ \| __/ _ \| '__/ _ \
# | |__| (_) | (_| | (_| |/ /  ___) | || (_) | | |  __/
# |_____\___/ \__,_|\__,_/_/  |____/ \__\___/|_|  \___|
#

store_r => {
	reg_req	=> { in => [ "none", "gp", "gp" ], out => [ "none", "none" ] },
	emit => '
	mips_attr_t* attr = get_mips_attr(node);
	ir_mode* mode;

	mode = attr->modes.load_store_mode;

	switch (get_mode_size_bits(mode)) {
	case 8:
		if (mode_is_signed(mode))
			. sb %S3, %C(%S2)
		break;
	case 16:
		if (mode_is_signed(mode))
			. sh %S3, %C(%S2)
		break;
	case 32:
			. sw %S3, %C(%S2)
		break;
	default:
		assert(! "Only 8, 16 and 32 bit stores supported");
		break;
	}
',
	cmp_attr => 'return attr_a->tv != attr_b->tv;',
},

store_i => {
	reg_req	=> { in => [ "none", "none", "gp" ], out => [ "none", "none" ] },
	emit => '
	mips_attr_t* attr = get_mips_attr(node);
	ir_mode *mode;

	mode = attr->modes.load_store_mode;

	switch (get_mode_size_bits(mode)) {
	case 8:
		. sb %S3, %C
		break;
	case 16:
		. sh %S3, %C
		break;
	case 32:
		. sw %S3, %C
		break;
	default:
		assert(! "Only 8, 16 and 32 bit stores supported");
		break;
	}
',
	cmp_attr => 'return attr_a->stack_entity != attr_b->stack_entity;',
},

move => {
	reg_req  => { in => [ "gp" ], out => [ "gp" ] },
	emit     => '. move %D1, %S1',
	mode     => "mode_Iu"
},

#
# Conversion
#

reinterpret_conv => {
	reg_req  => { in => [ "gp" ], out => [ "in_r1" ] },
	emit     => '. # reinterpret %S1 -> %D1',
	mode     => "mode_Iu"
},

#
# Miscelaneous
#

nop => {
	op_flags => "K",
	reg_req	 => { in => [], out => [ "none" ] },
	emit     => '. nop',
},

); # end of %nodes
