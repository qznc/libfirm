# This file is part of libFirm.
# Copyright (C) 2012 Karlsruhe Institute of Technology.
#
# Firm node specifications
# The comments are in (standard python) restructured text format and are used
# to generate documentation.
from spec_util import abstract, op

name = "ir"

@abstract
@op
class Binop(object):
	"""Binary nodes have exactly 2 inputs"""
	name     = "binop"
	ins      = [
		("left",   "first operand"),
		("right", "second operand"),
	]
	op_index       = 0
	pinned         = "no"
	arity_override = "oparity_binary"

@op
class Add(Binop):
	"""returns the sum of its operands"""
	flags = [ "commutative" ]

@op
class Alloc:
	"""Allocates a block of memory on the stack."""
	ins = [
		("mem",  "memory dependency" ),
		("size", "size of the block in bytes" ),
	]
	outs = [
		("M",   "memory result"),
		("res", "pointer to newly allocated memory"),
	]
	attrs = [
		dict(
			name    = "alignment",
			type    = "unsigned",
			comment = "alignment of the memory block (must be a power of 2)",
		),
	]
	flags       = [ "uses_memory" ]
	pinned      = "yes"
	attr_struct = "alloc_attr"

@op
class Anchor:
	"""utiliy node used to "hold" nodes in a graph that might possibly not be
	reachable by other means or which should be reachable immediately without
	searching through the graph.
	Each firm-graph contains exactly one anchor node whose address is always
	known. All other well-known graph-nodes like Start, End, NoMem, Bad, ...
	are found by looking at the respective Anchor operand."""
	mode             = "mode_ANY"
	arity            = "variable"
	flags            = [ "dump_noblock" ]
	pinned           = "yes"
	attr_struct      = "irg_attr"
	knownBlock       = True
	singleton        = True
	noconstructor    = True
	customSerializer = True

@op
class And(Binop):
	"""returns the result of a bitwise and operation of its operands"""
	flags    = [ "commutative" ]

@op
class ASM:
	"""executes assembler fragments of the target machine.

	The node contains a template for an assembler snippet. The compiler will
	replace occurences of %0 to %9 with input/output registers,
	%% with a single % char. Some backends allow additional specifiers (for
	example %w3, %l3, %h3 on x86 to get a 16bit, 8hit low, 8bit high part
	of a register).
	After the replacements the text is emitted into the final assembly.

	The clobber list contains names of registers which have an undefined value
	after the assembler instruction is executed; it may also contain 'memory'
	or 'cc' if global state/memory changes or the condition code registers
	(some backends implicitely set cc, memory clobbers on all ASM statements).

	Example (an i386 instruction)::

		ASM(text="btsl %1, %0",
			input_constraints = ["=m", "r"],
			clobbers = ["cc"])

	As there are no output, the %0 references the first input which is just an
	address which the asm operation writes to. %1 references to an input which
	is passed as a register. The condition code register has an unknown value
	after the instruction.

	(This format is inspired by the gcc extended asm syntax)
	"""
	mode             = "mode_T"
	arity            = "variable"
	input_name       = "input"
	flags            = [ "keep", "uses_memory" ]
	pinned           = "memory"
	pinned_init      = "op_pin_state_pinned"
	attr_struct      = "asm_attr"
	attrs_name       = "assem"
	customSerializer = True
	ins   = [
		("mem",    "memory dependency"),
	]
	attrs = [
		dict(
			name    = "input_constraints",
			type    = "ir_asm_constraint*",
			comment = "input constraints",
		),
		dict(
			name    = "n_output_constraints",
			type    = "size_t",
			noprop  = True,
			comment = "number of output constraints",
		),
		dict(
			name    = "output_constraints",
			type    = "ir_asm_constraint*",
			comment = "output constraints",
		),
		dict(
			name    = "n_clobbers",
			type    = "size_t",
			noprop  = True,
			comment = "number of clobbered registers/memory",
		),
		dict(
			name    = "clobbers",
			type    = "ident**",
			comment = "list of clobbered registers/memory",
		),
		dict(
			name    = "text",
			type    = "ident*",
			comment = "assembler text",
		),
	]
	# constructor is written manually at the moment, because of the clobbers+
	# constraints arrays needing special handling (2 arguments for 1 attribute)
	noconstructor = True

@op
class Bad:
	"""Bad nodes indicate invalid input, which is values which should never be
	computed.

	The typical use case for the Bad node is removing unreachable code.
	Frontends should set the current_block to Bad when it is clear that
	following code must be unreachable (ie. after a goto or return statement).
	Optimisations also set block predecessors to Bad when it becomes clear,
	that a control flow edge can never be executed.

	The gigo optimisations ensures that nodes with Bad as their block, get
	replaced by Bad themselves. Nodes with at least 1 Bad input get exchanged
	with Bad too. Exception to this rule are Block, Phi, Tuple and End node;
	This is because removing inputs from a Block is hairy operation (requiring,
	Phis to be shortened too for example). So instead of removing block inputs
	they are set to Bad, and the actual removal is left to the control flow
	optimisation phase. Block, Phi, Tuple with only Bad inputs however are
	replaced by Bad right away."""
	flags         = [ "start_block", "dump_noblock" ]
	pinned        = "yes"
	knownBlock    = True
	block         = "get_irg_start_block(irg)"
	attr_struct   = "bad_attr"
	init = '''
	res->attr.bad.irg.irg = irg;
	'''

@op
class Deleted:
	"""Internal node which is temporary set to nodes which are already removed
	from the graph."""
	mode             = "mode_Bad"
	flags            = [ ]
	pinned           = "yes"
	noconstructor    = True
	customSerializer = True # this has no serializer

@op
class Block:
	"""A basic block"""
	mode             = "mode_BB"
	knownBlock       = True
	block            = "NULL"
	pinned           = "yes"
	arity            = "variable"
	input_name       = "cfgpred"
	flags            = []
	attr_struct      = "block_attr"
	attrs            = [
		dict(
			name    = "entity",
			type    = "ir_entity*",
			comment = "entity representing this block",
			init    = "NULL",
		),
	]
	customSerializer = True

	init = '''
	res->attr.block.irg.irg     = irg;
	res->attr.block.backedge    = new_backedge_arr(get_irg_obstack(irg), arity);
	set_Block_matured(res, 1);

	/* Create and initialize array for Phi-node construction. */
	if (irg_is_constrained(irg, IR_GRAPH_CONSTRAINT_CONSTRUCTION)) {
		res->attr.block.graph_arr = NEW_ARR_DZ(ir_node*, get_irg_obstack(irg), irg->n_loc);
	}
	'''

@op
class Builtin:
	"""performs a backend-specific builtin."""
	ins         = [
		("mem", "memory dependency"),
	]
	arity       = "variable"
	input_name  = "param"
	outs        = [
		("M", "memory result"),
		# results follow here
	]
	flags       = [ "uses_memory" ]
	attrs       = [
		dict(
			type    = "ir_builtin_kind",
			name    = "kind",
			comment = "kind of builtin",
		),
		dict(
			type    = "ir_type*",
			name    = "type",
			comment = "method type for the builtin call",
		)
	]
	pinned      = "memory"
	pinned_init = "op_pin_state_pinned"
	attr_struct = "builtin_attr"
	init        = '''
	assert((get_unknown_type() == type) || is_Method_type(type));
	'''

@op
class Call:
	"""Calls other code. Control flow is transfered to ptr, additional
	operands are passed to the called code. Called code usually performs a
	return operation. The operands of this return operation are the result
	of the Call node."""
	ins         = [
		("mem",   "memory dependency"),
		("ptr",   "pointer to called code"),
	]
	arity       = "variable"
	input_name  = "param"
	outs        = [
		("M",                "memory result"),
		("T_result",         "tuple containing all results"),
		("X_regular",        "control flow when no exception occurs"),
		("X_except",         "control flow when exception occured"),
	]
	flags       = [ "fragile", "uses_memory" ]
	attrs       = [
		dict(
			type    = "ir_type*",
			name    = "type",
			comment = "type of the call (usually type of the called procedure)",
		),
	]
	attr_struct = "call_attr"
	pinned      = "memory"
	pinned_init = "op_pin_state_pinned"
	throws_init = "false"
	init = '''
	assert((get_unknown_type() == type) || is_Method_type(type));
	'''

@op
class Cmp(Binop):
	"""Compares its two operands and checks whether a specified
	   relation (like less or equal) is fulfilled."""
	flags = []
	mode  = "mode_b"
	attrs = [
		dict(
			type    = "ir_relation",
			name    = "relation",
			comment = "Comparison relation"
		)
	]
	attr_struct = "cmp_attr"

@op
class Cond:
	"""Conditionally change control flow."""
	ins      = [
		("selector",  "condition parameter"),
	]
	outs     = [
		("false", "control flow if operand is \"false\""),
		("true",  "control flow if operand is \"true\""),
	]
	flags    = [ "cfopcode", "forking" ]
	pinned   = "yes"
	attrs    = [
		dict(
			name    = "jmp_pred",
			type    = "cond_jmp_predicate",
			init    = "COND_JMP_PRED_NONE",
			comment = "can indicate the most likely jump",
		),
	]
	attr_struct = "cond_attr"

@op
class Switch:
	"""Change control flow. The destination is choosen based on an integer input value which is looked up in a table.

	Backends can implement this efficiently using a jump table."""
	ins    = [
		("selector", "input selector"),
	]
	outs   = [
		("default", "control flow if no other case matches"),
	]
	flags  = [ "cfopcode", "forking" ]
	pinned = "yes"
	attrs  = [
		dict(
			name    = "n_outs",
			type    = "unsigned",
			comment = "number of outputs (including pn_Switch_default)",
		),
		dict(
			name    = "table",
			type    = "ir_switch_table*",
			comment = "table describing mapping from input values to Proj numbers",
		),
	]
	attr_struct = "switch_attr"
	attrs_name  = "switcha"

@op
class Confirm:
	"""Specifies constraints for a value. This allows explicit representation
	of path-sensitive properties. (Example: This value is always >= 0 on 1
	if-branch then all users within that branch are rerouted to a confirm-node
	specifying this property).

	A constraint is specified for the relation between value and bound.
	value is always returned.
	Note that this node does NOT check or assert the constraint, it merely
	specifies it."""
	ins      = [
		("value",  "value to express a constraint for"),
		("bound",  "value to compare against"),
	]
	mode     = "get_irn_mode(irn_value)"
	flags    = [ "highlevel" ]
	pinned   = "yes"
	attrs    = [
		dict(
			name    = "relation",
			type    = "ir_relation",
			comment = "relation of value to bound",
		),
	]
	attr_struct = "confirm_attr"

@op
class Const:
	"""Returns a constant value."""
	flags      = [ "constlike", "start_block" ]
	block      = "get_irg_start_block(irg)"
	mode       = "get_tarval_mode(tarval)"
	knownBlock = True
	pinned     = "no"
	attrs      = [
		dict(
			type    = "ir_tarval*",
			name    = "tarval",
			comment = "constant value (a tarval object)",
		)
	]
	attr_struct = "const_attr"
	attrs_name  = "con"

@op
class Conv:
	"""Converts values between modes"""
	flags  = []
	pinned = "no"
	ins    = [
		("op", "operand")
	]

@op
class CopyB:
	"""Copies a block of memory with statically known size/type."""
	ins   = [
		("mem",  "memory dependency"),
		("dst",  "destination address"),
		("src",  "source address"),
	]
	outs  = [
		("M",         "memory result"),
		("X_regular", "control flow when no exception occurs"),
		("X_except",  "control flow when exception occured"),
	]
	flags = [ "fragile", "uses_memory" ]
	attrs = [
		dict(
			name    = "type",
			type    = "ir_type*",
			comment = "type of copied data",
		)
	]
	attr_struct = "copyb_attr"
	pinned      = "memory"
	pinned_init = "op_pin_state_pinned"
	throws_init = "false"

@op
class Div:
	"""returns the quotient of its 2 operands"""
	ins   = [
		("mem",   "memory dependency"),
		("left",  "first operand"),
		("right", "second operand"),
	]
	outs  = [
		("M",         "memory result"),
		("res",       "result of computation"),
		("X_regular", "control flow when no exception occurs"),
		("X_except",  "control flow when exception occured"),
	]
	flags = [ "fragile", "uses_memory" ]
	attrs = [
		dict(
			type    = "ir_mode*",
			name    = "resmode",
			comment = "mode of the result value",
		),
		dict(
			name = "no_remainder",
			type = "int",
			init = "0",
		)
	]
	attr_struct = "div_attr"
	pinned      = "exception"
	throws_init = "false"
	op_index    = 1
	arity_override = "oparity_binary"

@op
class Dummy:
	"""A placeholder value. This is used when constructing cyclic graphs where
	you have cases where not all predecessors of a phi-node are known. Dummy
	nodes are used for the unknown predecessors and replaced later."""
	ins        = []
	flags      = [ "cfopcode", "start_block", "constlike", "dump_noblock" ]
	knownBlock = True
	pinned     = "yes"
	block      = "get_irg_start_block(irg)"

@op
class End:
	"""Last node of a graph. It references nodes in endless loops (so called
	keepalive edges)"""
	mode             = "mode_X"
	pinned           = "yes"
	arity            = "dynamic"
	input_name       = "keepalive"
	flags            = [ "cfopcode" ]
	knownBlock       = True
	block            = "get_irg_end_block(irg)"
	singleton        = True

@op
class Eor(Binop):
	"""returns the result of a bitwise exclusive or operation of its operands.

	This is also known as the Xor operation."""
	flags    = [ "commutative" ]

@op
class Free:
	"""Frees a block of memory previously allocated by an Alloc node"""
	ins = [
		("mem", "memory dependency" ),
		("ptr", "pointer to the object to free"),
	]
	mode   = "mode_M"
	flags  = [ "uses_memory" ]
	pinned = "yes"

@op
class Id:
	"""Returns its operand unchanged.

	This is mainly used when exchanging nodes. Usually you shouldn't see Id
	nodes since the getters/setters for node inputs skip them automatically."""
	ins    = [
	   ("pred", "the value which is returned unchanged")
	]
	pinned = "no"
	flags  = []

@op
class IJmp:
	"""Jumps to the code in its argument. The code has to be in the same
	function and the the destination must be one of the blocks reachable
	by the tuple results"""
	mode     = "mode_X"
	pinned   = "yes"
	ins      = [
	   ("target", "target address of the jump"),
	]
	flags    = [ "cfopcode", "forking", "keep", "unknown_jump" ]

@op
class InstOf:
	"""Tests whether an object is an instance of a class-type"""
	ins   = [
	   ("store", "memory dependency"),
	   ("obj",   "pointer to object being queried")
	]
	outs  = [
		("M",         "memory result"),
		("res",       "checked object pointer"),
		("X_regular", "control flow when no exception occurs"),
		("X_except",  "control flow when exception occured"),
	]
	flags = [ "highlevel" ]
	attrs = [
		dict(
			name    = "type",
			type    = "ir_type*",
			comment = "type to check ptr for",
		)
	]
	attr_struct = "io_attr"
	pinned      = "memory"
	pinned_init = "op_pin_state_floats"

@op
class Jmp:
	"""Jumps to the block connected through the out-value"""
	mode     = "mode_X"
	pinned   = "yes"
	ins      = []
	flags    = [ "cfopcode" ]

@op
class Load:
	"""Loads a value from memory (heap or stack)."""
	ins   = [
		("mem", "memory dependency"),
		("ptr",  "address to load from"),
	]
	outs  = [
		("M",         "memory result"),
		("res",       "result of load operation"),
		("X_regular", "control flow when no exception occurs"),
		("X_except",  "control flow when exception occured"),
	]
	flags    = [ "fragile", "uses_memory" ]
	pinned   = "exception"
	attrs    = [
		dict(
			type      = "ir_mode*",
			name      = "mode",
			comment   = "mode of the value to be loaded",
		),
		dict(
			type      = "ir_volatility",
			name      = "volatility",
			comment   = "volatile loads are a visible side-effect and may not be optimized",
			init      = "flags & cons_volatile ? volatility_is_volatile : volatility_non_volatile",
			to_flags  = "%s == volatility_is_volatile ? cons_volatile : cons_none"
		),
		dict(
			type      = "ir_align",
			name      = "unaligned",
			comment   = "pointers to unaligned loads don't need to respect the load-mode/type alignments",
			init      = "flags & cons_unaligned ? align_non_aligned : align_is_aligned",
			to_flags  = "%s == align_non_aligned ? cons_unaligned : cons_none"
		),
	]
	attr_struct = "load_attr"
	constructor_args = [
		dict(
			type    = "ir_cons_flags",
			name    = "flags",
			comment = "specifies alignment, volatility and pin state",
		),
	]
	pinned_init = "flags & cons_floats ? op_pin_state_floats : op_pin_state_pinned"
	throws_init = "(flags & cons_throws_exception) != 0"

@op
class Minus:
	"""returns the additive inverse of its operand"""
	flags  = []
	pinned = "no"
	ins    = [
		("op", "operand")
	]

@op
class Mod:
	"""returns the remainder of its operands from an implied division.

	Examples:

	* mod(5,3)   produces 2
	* mod(5,-3)  produces 2
	* mod(-5,3)  produces -2
	* mod(-5,-3) produces -2
	"""
	ins   = [
		("mem",   "memory dependency"),
		("left",  "first operand"),
		("right", "second operand"),
	]
	outs  = [
		("M",         "memory result"),
		("res",       "result of computation"),
		("X_regular", "control flow when no exception occurs"),
		("X_except",  "control flow when exception occured"),
	]
	flags = [ "fragile", "uses_memory" ]
	attrs = [
		dict(
			type    = "ir_mode*",
			name    = "resmode",
			comment = "mode of the result",
		),
	]
	attr_struct = "mod_attr"
	pinned      = "exception"
	throws_init = "false"
	op_index    = 1
	arity_override = "oparity_binary"

class Mul(Binop):
	"""returns the product of its operands"""
	flags = [ "commutative" ]

class Mulh(Binop):
	"""returns the upper word of the product of its operands (the part which
	would not fit into the result mode of a normal Mul anymore)"""
	flags = [ "commutative" ]

@op
class Mux:
	"""returns the false or true operand depending on the value of the sel
	operand"""
	ins    = [
	   ("sel",   "value making the output selection"),
	   ("false", "selected if sel input is false"),
	   ("true",  "selected if sel input is true"),
	]
	flags  = []
	pinned = "no"

@op
class NoMem:
	"""Placeholder node for cases where you don't need any memory input"""
	mode          = "mode_M"
	flags         = [ "dump_noblock" ]
	pinned        = "yes"
	knownBlock    = True
	block         = "get_irg_start_block(irg)"
	singleton     = True

@op
class Not:
	"""returns the bitwise complement of a value. Works for boolean values, too."""
	flags  = []
	pinned = "no"
	ins    = [
		("op", "operand")
	]

@op
class Or(Binop):
	"""returns the result of a bitwise or operation of its operands"""
	flags = [ "commutative" ]

@op
class Phi:
	"""Choose a value based on control flow. A phi node has 1 input for each
	predecessor of its block. If a block is entered from its nth predecessor
	all phi nodes produce their nth input as result."""
	pinned        = "yes"
	arity         = "variable"
	input_name    = "pred"
	flags         = []
	attr_struct   = "phi_attr"
	init          = '''
	res->attr.phi.u.backedge = new_backedge_arr(get_irg_obstack(irg), arity);'''
	customSerializer = True

@op
class Pin:
	"""Pin the value of the node node in the current block. No users of the Pin
	node can float above the Block of the Pin. The node cannot float behind
	this block. Often used to Pin the NoMem node."""
	ins      = [
		("op", "value which is pinned"),
	]
	mode     = "get_irn_mode(irn_op)"
	flags    = [ "highlevel" ]
	pinned   = "yes"

@op
class Proj:
	"""returns an entry of a tuple value"""
	ins              = [
		("pred", "the tuple value from which a part is extracted"),
	]
	flags            = []
	pinned           = "no"
	knownBlock       = True
	knownGraph       = True
	block            = "get_nodes_block(irn_pred)"
	graph            = "get_irn_irg(irn_pred)"
	attrs      = [
		dict(
			type    = "long",
			name    = "proj",
			comment = "number of tuple component to be extracted",
		),
	]
	attr_struct = "proj_attr"

@op
class Raise:
	"""Raises an exception. Unconditional change of control flow. Writes an
	explicit Except variable to memory to pass it to the exception handler.
	Must be lowered to a Call to a runtime check function."""
	ins    = [
		("mem",     "memory dependency"),
		("exo_ptr", "pointer to exception object to be thrown"),
	]
	outs  = [
		("M", "memory result"),
		("X", "control flow to exception handler"),
	]
	flags  = [ "highlevel", "cfopcode" ]
	pinned = "yes"

@op
class Return:
	"""Returns from the current function. Takes memory and return values as
	operands."""
	ins        = [
		("mem", "memory dependency"),
	]
	arity      = "variable"
	input_name = "res"
	mode       = "mode_X"
	flags      = [ "cfopcode" ]
	pinned     = "yes"

class Rotl(Binop):
	"""Returns its first operand bits rotated left by the amount in the 2nd
	operand"""
	flags    = []

@op
class Sel:
	"""Computes the address of a entity of a compound type given the base
	address of an instance of the compound type.

	Optimisations assume that a Sel node can only produce a NULL pointer if the
	ptr input was NULL."""
	ins         = [
		("mem", "memory dependency"),
		("ptr", "pointer to object to select from"),
	]
	arity       = "variable"
	input_name  = "index"
	flags       = []
	mode        = "is_Method_type(get_entity_type(entity)) ? mode_P_code : mode_P_data"
	pinned      = "no"
	attrs       = [
		dict(
			type    = "ir_entity*",
			name    = "entity",
			comment = "entity which is selected",
		)
	]
	attr_struct = "sel_attr"

@op
class Shl(Binop):
	"""Returns its first operands bits shifted left by the amount of the 2nd
	operand.
	The right input (shift amount) must be an unsigned integer value.
	If the result mode has modulo_shift!=0, then the effective shift amount is
	the right input modulo this modulo_shift amount."""
	flags = []

@op
class Shr(Binop):
	"""Returns its first operands bits shifted right by the amount of the 2nd
	operand. No special handling for the sign bit is performed (zero extension).
	The right input (shift amount) must be an unsigned integer value.
	If the result mode has modulo_shift!=0, then the effective shift amount is
	the right input modulo this modulo_shift amount."""
	flags = []

@op
class Shrs(Binop):
	"""Returns its first operands bits shifted right by the amount of the 2nd
	operand. The leftmost bit (usually the sign bit) stays the same
	(sign extension).
	The right input (shift amount) must be an unsigned integer value.
	If the result mode has modulo_shift!=0, then the effective shift amount is
	the right input modulo this modulo_shift amount."""
	flags = []

@op
class Start:
	"""The first node of a graph. Execution starts with this node."""
	outs       = [
		("X_initial_exec", "control flow"),
		("M",              "initial memory"),
		("P_frame_base",   "frame base pointer"),
		("T_args",         "function arguments")
	]
	mode             = "mode_T"
	pinned           = "yes"
	flags            = [ "cfopcode" ]
	singleton        = True
	knownBlock       = True
	block            = "get_irg_start_block(irg)"

@op
class Store:
	"""Stores a value into memory (heap or stack)."""
	ins   = [
	   ("mem",   "memory dependency"),
	   ("ptr",   "address to store to"),
	   ("value", "value to store"),
	]
	outs  = [
		("M",         "memory result"),
		("X_regular", "control flow when no exception occurs"),
		("X_except",  "control flow when exception occured"),
	]
	flags    = [ "fragile", "uses_memory" ]
	pinned   = "exception"
	attr_struct = "store_attr"
	pinned_init = "flags & cons_floats ? op_pin_state_floats : op_pin_state_pinned"
	throws_init = "(flags & cons_throws_exception) != 0"
	attrs = [
		dict(
			type      = "ir_volatility",
			name      = "volatility",
			comment   = "volatile stores are a visible side-effect and may not be optimized",
			init      = "flags & cons_volatile ? volatility_is_volatile : volatility_non_volatile",
			to_flags  = "%s == volatility_is_volatile ? cons_volatile : cons_none"
		),
		dict(
			type      = "ir_align",
			name      = "unaligned",
			comment   = "pointers to unaligned stores don't need to respect the load-mode/type alignments",
			init      = "flags & cons_unaligned ? align_non_aligned : align_is_aligned",
			to_flags  = "%s == align_non_aligned ? cons_unaligned : cons_none"
		),
	]
	constructor_args = [
		dict(
			type    = "ir_cons_flags",
			name    = "flags",
			comment = "specifies alignment, volatility and pin state",
		),
	]

@op
class Sub(Binop):
	"""returns the difference of its operands"""
	flags = []

@op
class SymConst:
	"""A symbolic constant.

	 - *symconst_type_size* The symbolic constant represents the size of a type.
	                        The type of which the constant represents the size
	                        is given explicitly.
	 - *symconst_type_align* The symbolic constant represents the alignment of a
	                        type.  The type of which the constant represents the
	                        size is given explicitly.
	 - *symconst_addr_ent*  The symbolic constant represents the address of an
	                        entity (variable or method).  The variable is given
	                        explicitly by a firm entity.
	 - *symconst_ofs_ent*   The symbolic constant represents the offset of an
	                        entity in its owner type.
	 - *symconst_enum_const* The symbolic constant is a enumeration constant of
	                        an enumeration type."""
	mode       = "mode_P"
	flags      = [ "constlike", "start_block" ]
	knownBlock = True
	pinned     = "no"
	attrs      = [
		dict(
			type    = "ir_entity*",
			name    = "entity",
			noprop  = True,
			comment = "entity whose address is returned",
		)
	]
	attr_struct = "symconst_attr"
	customSerializer = True
	# constructor is written manually at the moment, because of the strange
	# union argument
	noconstructor = True

@op
class Sync:
	"""The Sync operation unifies several partial memory blocks. These blocks
	have to be pairwise disjunct or the values in common locations have to
	be identical.  This operation allows to specify all operations that
	eventually need several partial memory blocks as input with a single
	entrance by unifying the memories with a preceding Sync operation."""
	mode       = "mode_M"
	flags      = []
	pinned     = "no"
	arity      = "dynamic"
	input_name = "pred"

@op
class Tuple:
	"""Builds a Tuple from single values.

	This is needed to implement optimizations that remove a node that produced
	a tuple.  The node can be replaced by the Tuple operation so that the
	following Proj nodes have not to be changed. (They are hard to find due to
	the implementation with pointers in only one direction.) The Tuple node is
	smaller than any other node, so that a node can be changed into a Tuple by
	just changing its opcode and giving it a new in array."""
	arity      = "variable"
	input_name = "pred"
	mode       = "mode_T"
	pinned     = "no"
	flags      = []

@op
class Unknown:
	"""Returns an unknown (at compile- and runtime) value. It is a valid
	optimisation to replace an Unknown by any other constant value."""
	knownBlock = True
	pinned     = "yes"
	block      = "get_irg_start_block(irg)"
	flags      = [ "start_block", "constlike", "dump_noblock" ]
