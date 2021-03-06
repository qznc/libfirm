/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief   emit assembler for a backend graph
 * @author  Hannes Rapp, Matthias Braun
 */
#include <limits.h>

#include "bitfiddle.h"
#include "xmalloc.h"
#include "tv.h"
#include "iredges.h"
#include "debug.h"
#include "irgwalk.h"
#include "irop_t.h"
#include "irargs_t.h"
#include "irprog.h"
#include "irargs_t.h"
#include "error.h"
#include "raw_bitset.h"
#include "dbginfo.h"
#include "heights.h"
#include "pmap.h"
#include "execfreq_t.h"

#include "besched.h"
#include "beblocksched.h"
#include "beirg.h"
#include "begnuas.h"
#include "bedwarf.h"
#include "benode.h"
#include "bestack.h"
#include "bepeephole.h"

#include "sparc_emitter.h"
#include "gen_sparc_emitter.h"
#include "sparc_nodes_attr.h"
#include "sparc_new_nodes.h"
#include "gen_sparc_regalloc_if.h"

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

static ir_heights_t *heights;
static unsigned     *delay_slot_fillers;
static pmap         *delay_slots;

static bool emitting_delay_slot;

/**
 * indent before instruction. (Adds additional indentation when emitting
 * delay slots)
 */
static void sparc_emit_indent(void)
{
	be_emit_char('\t');
	if (emitting_delay_slot)
		be_emit_char(' ');
}

static void sparc_emit_immediate(int32_t value, ir_entity *entity)
{
	if (entity == NULL) {
		assert(sparc_is_value_imm_encodeable(value));
		be_emit_irprintf("%d", value);
	} else {
		if (get_entity_owner(entity) == get_tls_type()) {
			be_emit_cstring("%tle_lox10(");
		} else {
			be_emit_cstring("%lo(");
		}
		be_gas_emit_entity(entity);
		if (value != 0) {
			be_emit_irprintf("%+d", value);
		}
		be_emit_char(')');
	}
}

static void sparc_emit_high_immediate(ir_node const *node)
{
	const sparc_attr_t *attr   = get_sparc_attr_const(node);
	ir_entity          *entity = attr->immediate_value_entity;

	if (entity == NULL) {
		uint32_t value = (uint32_t) attr->immediate_value;
		be_emit_irprintf("%%hi(0x%X)", value);
	} else {
		if (get_entity_owner(entity) == get_tls_type()) {
			be_emit_cstring("%tle_hix22(");
		} else {
			be_emit_cstring("%hi(");
		}
		be_gas_emit_entity(entity);
		if (attr->immediate_value != 0) {
			be_emit_irprintf("%+d", attr->immediate_value);
		}
		be_emit_char(')');
	}
}

static void sparc_emit_register(const arch_register_t *const reg)
{
	be_emit_char('%');
	be_emit_string(reg->name);
}

static void sparc_emit_source_register(ir_node const *node, int const pos)
{
	const arch_register_t *reg = arch_get_irn_register_in(node, pos);
	sparc_emit_register(reg);
}

static void sparc_emit_dest_register(ir_node const *const node, int const pos)
{
	const arch_register_t *reg = arch_get_irn_register_out(node, pos);
	sparc_emit_register(reg);
}

/**
 * emit SP offset
 */
static void sparc_emit_offset(const ir_node *node, int offset_node_pos)
{
	const sparc_load_store_attr_t *attr = get_sparc_load_store_attr_const(node);

	if (attr->is_reg_reg) {
		assert(!attr->is_frame_entity);
		assert(attr->base.immediate_value == 0);
		assert(attr->base.immediate_value_entity == NULL);
		be_emit_char('+');
		sparc_emit_source_register(node, offset_node_pos);
	} else if (attr->is_frame_entity) {
		int32_t offset = attr->base.immediate_value;
		if (offset != 0) {
			assert(sparc_is_value_imm_encodeable(offset));
			be_emit_irprintf("%+ld", offset);
		}
	} else if (attr->base.immediate_value != 0
			|| attr->base.immediate_value_entity != NULL) {
		be_emit_char('+');
		sparc_emit_immediate(attr->base.immediate_value,
		                     attr->base.immediate_value_entity);
	}
}

/**
 *  Emit load mode
 */
static void sparc_emit_load_mode(ir_node const *const node)
{
	const sparc_load_store_attr_t *attr = get_sparc_load_store_attr_const(node);
	ir_mode *mode      = attr->load_store_mode;
	int      bits      = get_mode_size_bits(mode);
	bool     is_signed = mode_is_signed(mode);

	switch (bits) {
	case   8: be_emit_string(is_signed ? "sb" : "ub"); break;
	case  16: be_emit_string(is_signed ? "sh" : "uh"); break;
	case  32: break;
	case  64: be_emit_char('d'); break;
	case 128: be_emit_char('q'); break;
	default:  panic("invalid load/store mode %+F", mode);
	}
}

/**
 * Emit store mode char
 */
static void sparc_emit_store_mode(ir_node const *const node)
{
	const sparc_load_store_attr_t *attr = get_sparc_load_store_attr_const(node);
	ir_mode *mode      = attr->load_store_mode;
	int      bits      = get_mode_size_bits(mode);

	switch (bits) {
	case   8: be_emit_char('b'); break;
	case  16: be_emit_char('h'); break;
	case  32: break;
	case  64: be_emit_char('d'); break;
	case 128: be_emit_char('q'); break;
	default:  panic("invalid load/store mode %+F", mode);
	}
}

static void emit_fp_suffix(const ir_mode *mode)
{
	assert(mode_is_float(mode));
	switch (get_mode_size_bits(mode)) {
	case  32: be_emit_char('s'); break;
	case  64: be_emit_char('d'); break;
	case 128: be_emit_char('q'); break;
	default:  panic("invalid FP mode");
	}
}

static void set_jump_target(ir_node *jump, ir_node *target)
{
	set_irn_link(jump, target);
}

static ir_node *get_jump_target(const ir_node *jump)
{
	return (ir_node*)get_irn_link(jump);
}

/**
 * Returns the target label for a control flow node.
 */
static void sparc_emit_cfop_target(const ir_node *node)
{
	ir_node *block = get_jump_target(node);
	be_gas_emit_block_name(block);
}

/**
 * returns true if a sparc_call calls a register and not an immediate
 */
static bool is_sparc_reg_call(const ir_node *node)
{
	const sparc_attr_t *attr = get_sparc_attr_const(node);
	return attr->immediate_value_entity == NULL;
}

static int get_sparc_Call_dest_addr_pos(const ir_node *node)
{
	assert(is_sparc_reg_call(node));
	return get_irn_arity(node)-1;
}

static bool ba_is_fallthrough(const ir_node *node)
{
	ir_node *block      = get_nodes_block(node);
	ir_node *next_block = (ir_node*)get_irn_link(block);
	return get_jump_target(node) == next_block;
}

static bool is_no_instruction(const ir_node *node)
{
	/* copies are nops if src_reg == dest_reg */
	if (be_is_Copy(node) || be_is_CopyKeep(node)) {
		const arch_register_t *src_reg  = arch_get_irn_register_in(node, 0);
		const arch_register_t *dest_reg = arch_get_irn_register_out(node, 0);

		if (src_reg == dest_reg)
			return true;
	}
	if (be_is_IncSP(node) && be_get_IncSP_offset(node) == 0)
		return true;
	/* Ba is not emitted if it is a simple fallthrough */
	if (is_sparc_Ba(node) && ba_is_fallthrough(node))
		return true;

	return be_is_Keep(node) || be_is_Start(node) || is_Phi(node);
}

static bool has_delay_slot(const ir_node *node)
{
	if (is_sparc_Ba(node)) {
		return !ba_is_fallthrough(node);
	}

	return arch_get_irn_flags(node) & sparc_arch_irn_flag_has_delay_slot;
}

/** returns true if the emitter for this sparc node can produce more than one
 * actual sparc instruction.
 * Usually it is a bad sign if we have to add instructions here. We should
 * rather try to get them lowered down. So we can actually put them into
 * delay slots and make them more accessible to the scheduler.
 */
static bool emits_multiple_instructions(const ir_node *node)
{
	if (has_delay_slot(node))
		return true;

	if (is_sparc_Call(node))
		return arch_get_irn_flags(node) & sparc_arch_irn_flag_aggregate_return;

	return is_sparc_SMulh(node) || is_sparc_UMulh(node)
		|| is_sparc_SDiv(node) || is_sparc_UDiv(node)
		|| be_is_MemPerm(node) || be_is_Perm(node)
		|| is_sparc_SubSP(node) || is_sparc_ASM(node);
}

static bool uses_reg(const ir_node *node, unsigned reg_index, unsigned width)
{
	int arity = get_irn_arity(node);
	for (int i = 0; i < arity; ++i) {
		const arch_register_t     *in_reg = arch_get_irn_register_in(node, i);
		const arch_register_req_t *in_req = arch_get_irn_register_req_in(node, i);
		if (in_reg == NULL)
			continue;
		if (reg_index < (unsigned)in_reg->global_index + in_req->width
			&& reg_index + width > in_reg->global_index)
			return true;
	}
	return false;
}

static bool writes_reg(const ir_node *node, unsigned reg_index, unsigned width)
{
	be_foreach_out(node, o) {
		const arch_register_t *out_reg = arch_get_irn_register_out(node, o);
		if (out_reg == NULL)
			continue;
		const arch_register_req_t *out_req = arch_get_irn_register_req_out(node, o);
		if (reg_index < (unsigned)out_reg->global_index + out_req->width
			&& reg_index + width > out_reg->global_index)
			return true;
	}
	return false;
}

static bool is_legal_delay_slot_filler(const ir_node *node)
{
	if (is_no_instruction(node))
		return false;
	if (emits_multiple_instructions(node))
		return false;
	if (rbitset_is_set(delay_slot_fillers, get_irn_idx(node)))
		return false;
	return true;
}

static bool can_move_down_into_delayslot(const ir_node *node, const ir_node *to)
{
	if (!is_legal_delay_slot_filler(node))
		return false;

	if (!be_can_move_down(heights, node, to))
		return false;

	if (is_sparc_Call(to)) {
		ir_node *check;
		/** all inputs are used after the delay slot so, we're fine */
		if (!is_sparc_reg_call(to))
			return true;

		check = get_irn_n(to, get_sparc_Call_dest_addr_pos(to));
		if (skip_Proj(check) == node)
			return false;

		/* the Call also destroys the value of %o7, but since this is
		 * currently marked as ignore register in the backend, it
		 * should never be used by the instruction in the delay slot. */
		if (uses_reg(node, REG_O7, 1))
			return false;
		return true;
	} else if (is_sparc_Return(to)) {
		/* return uses the value of %o7, all other values are not
		 * immediately used */
		if (writes_reg(node, REG_O7, 1))
			return false;
		return true;
	} else {
		/* the node must not use our computed values */
		int arity = get_irn_arity(to);
		for (int i = 0; i < arity; ++i) {
			ir_node *in = get_irn_n(to, i);
			if (skip_Proj(in) == node)
				return false;
		}
		return true;
	}
}

static bool can_move_up_into_delayslot(const ir_node *node, const ir_node *to)
{
	if (!be_can_move_up(heights, node, to))
		return false;

	/* node must not use any results of 'to' */
	for (int i = 0, arity = get_irn_arity(node); i < arity; ++i) {
		ir_node *in      = get_irn_n(node, i);
		ir_node *skipped = skip_Proj(in);
		if (skipped == to)
			return false;
	}

	/* register window cycling effects at Restore aren't correctly represented
	 * in the graph yet so we need this exception here */
	if (is_sparc_Restore(node) || is_sparc_RestoreZero(node)) {
		return false;
	} else if (is_sparc_Call(to)) {
		/* node must not overwrite any of the inputs of the call,
		 * (except for the dest_addr) */
		int dest_addr_pos = is_sparc_reg_call(to)
			? get_sparc_Call_dest_addr_pos(to) : -1;

		int call_arity = get_irn_arity(to);
		for (int i = 0; i < call_arity; ++i) {
			if (i == dest_addr_pos)
				continue;
			const arch_register_t *reg = arch_get_irn_register_in(to, i);
			if (reg == NULL)
				continue;
			const arch_register_req_t *req = arch_get_irn_register_req_in(to, i);
			if (writes_reg(node, reg->global_index, req->width))
				return false;
		}

		/* node must not write to one of the call outputs */
		be_foreach_out(to, o) {
			const arch_register_t *reg = arch_get_irn_register_out(to, o);
			if (reg == NULL)
				continue;
			const arch_register_req_t *req = arch_get_irn_register_req_out(to, o);
			if (writes_reg(node, reg->global_index, req->width))
				return false;
		}
	} else if (is_sparc_SDiv(to) || is_sparc_UDiv(to)) {
		/* node will be inserted between wr and div so it must not overwrite
		 * anything except the wr input */
		for (int i = 0, arity = get_irn_arity(to); i < arity; ++i) {
			assert((long)n_sparc_SDiv_dividend_high == (long)n_sparc_UDiv_dividend_high);
			if (i == n_sparc_SDiv_dividend_high)
				continue;
			const arch_register_t *reg = arch_get_irn_register_in(to, i);
			if (reg == NULL)
				continue;
			const arch_register_req_t *req = arch_get_irn_register_req_in(to, i);
			if (writes_reg(node, reg->global_index, req->width))
				return false;
		}
	}
	return true;
}

static void optimize_fallthrough(ir_node *node)
{
	ir_node *proj_true  = NULL;
	ir_node *proj_false = NULL;

	assert((long)pn_sparc_Bicc_false == (long)pn_sparc_fbfcc_false);
	assert((long)pn_sparc_Bicc_true  == (long)pn_sparc_fbfcc_true);
	foreach_out_edge(node, edge) {
		ir_node *proj = get_edge_src_irn(edge);
		long nr = get_Proj_proj(proj);
		if (nr == pn_sparc_Bicc_true) {
			proj_true = proj;
		} else {
			assert(nr == pn_sparc_Bicc_false);
			proj_false = proj;
		}
	}
	assert(proj_true != NULL && proj_false != NULL);

	/* for now, the code works for scheduled and non-schedules blocks */
	const ir_node *block = get_nodes_block(node);

	/* we have a block schedule */
	const ir_node *next_block = (ir_node*)get_irn_link(block);

	if (get_jump_target(proj_true) == next_block) {
		/* exchange both proj destinations so the second one can be omitted */
		set_Proj_proj(proj_true,  pn_sparc_Bicc_false);
		set_Proj_proj(proj_false, pn_sparc_Bicc_true);

		sparc_jmp_cond_attr_t *attr = get_sparc_jmp_cond_attr(node);
		attr->relation = get_negated_relation(attr->relation);
	}
}

/**
 * search for an instruction that can fill the delay slot of @p node
 */
static ir_node *pick_delay_slot_for(ir_node *node)
{
	static const unsigned PICK_DELAY_SLOT_MAX_DISTANCE = 10;
	assert(has_delay_slot(node));

	if (is_sparc_Bicc(node) || is_sparc_fbfcc(node)) {
		optimize_fallthrough(node);
	}

	unsigned tries = 0;
	sched_foreach_reverse_before(node, schedpoint) {
		if (has_delay_slot(schedpoint))
			break;
		if (tries++ >= PICK_DELAY_SLOT_MAX_DISTANCE)
			break;

		if (!can_move_down_into_delayslot(schedpoint, node))
			continue;

		/* found something */
		return schedpoint;
	}

	/* search after the current position */
	tries = 0;
	sched_foreach_after(node, schedpoint) {
		if (has_delay_slot(schedpoint))
			break;
		if (tries++ >= PICK_DELAY_SLOT_MAX_DISTANCE)
			break;
		if (!is_legal_delay_slot_filler(schedpoint))
			continue;
		if (!can_move_up_into_delayslot(schedpoint, node))
			continue;

		/* found something */
		return schedpoint;
	}

	/* look in successor blocks */
	ir_node *block = get_nodes_block(node);
	/* TODO: sort succs by execution frequency */
	foreach_block_succ(block, edge) {
		ir_node *succ = get_edge_src_irn(edge);
		/* we can't easily move up stuff from blocks with multiple predecessors
		 * since the instruction is lacking for the other preds then.
		 * (We also don't have to do any phi translation) */
		if (get_Block_n_cfgpreds(succ) > 1)
			continue;

		tries = 0;
		sched_foreach(succ, schedpoint) {
			if (has_delay_slot(schedpoint))
				break;
			/* can't move pinned nodes accross blocks */
			if (get_irn_pinned(schedpoint) == op_pin_state_pinned)
				continue;
			/* restore doesn't model register window switching correctly,
			 * so it appears like we could move it, which is not true */
			if (is_sparc_Restore(schedpoint)
			    || is_sparc_RestoreZero(schedpoint))
				continue;
			if (tries++ >= PICK_DELAY_SLOT_MAX_DISTANCE)
				break;
			if (!is_legal_delay_slot_filler(schedpoint))
				continue;
			if (can_move_up_into_delayslot(schedpoint, node)) {
				/* it's fine to move the insn accross blocks */
				return schedpoint;
			} else if (is_sparc_Bicc(node) || is_sparc_fbfcc(node)) {
				ir_node *proj = get_Block_cfgpred(succ, 0);
				long     nr   = get_Proj_proj(proj);
				if ((nr == pn_sparc_Bicc_true || nr == pn_sparc_fbfcc_true)
					&& be_can_move_up(heights, schedpoint, succ)) {
					/* we can use it with the annul flag */
					sparc_jmp_cond_attr_t *attr = get_sparc_jmp_cond_attr(node);
					attr->annul_delay_slot = true;
					return schedpoint;
				}
			}
		}
	}

	return NULL;
}

void sparc_emitf(ir_node const *const node, char const *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	sparc_emit_indent();
	for (;;) {
		char const *start = fmt;

		while (*fmt != '%' && *fmt != '\0')
			++fmt;
		be_emit_string_len(start, fmt - start);
		if (*fmt == '\0')
			break;
		++fmt;

		bool plus = false;
		if (*fmt == '+') {
			plus = true;
			++fmt;
		}

		switch (*fmt++) {
		case '%':
			be_emit_char('%');
			break;

		case 'A': {
			const sparc_jmp_cond_attr_t *attr
				= get_sparc_jmp_cond_attr_const(node);
			if (attr->annul_delay_slot) {
				be_emit_cstring(",a");
			}
			break;
		}

		case 'D':
			if (*fmt < '0' || '9' <= *fmt)
				goto unknown;
			sparc_emit_dest_register(node, *fmt++ - '0');
			break;

		case 'E': {
			sparc_attr_t const *const attr = get_sparc_attr_const(node);
			be_gas_emit_entity(attr->immediate_value_entity);
			if (attr->immediate_value != 0) {
				be_emit_irprintf(plus ? "%+d" : "%d", attr->immediate_value);
			}
			break;
		}

		case 'F': {
			ir_mode *mode;
			switch (*fmt++) {
			case 'D': mode = get_sparc_fp_conv_attr_const(node)->dest_mode; break;
			case 'M': mode = get_sparc_fp_attr_const(node)->fp_mode;        break;
			case 'S': mode = get_sparc_fp_conv_attr_const(node)->src_mode;  break;
			default:  goto unknown;
			}
			emit_fp_suffix(mode);
			break;
		}

		case 'H':
			sparc_emit_high_immediate(node);
			break;

		case 'L': {
			ir_node *n = va_arg(ap, ir_node*);
			sparc_emit_cfop_target(n);
			break;
		}

		case 'M':
			switch (*fmt++) {
			case 'L': sparc_emit_load_mode(node);  break;
			case 'S': sparc_emit_store_mode(node); break;
			default:  goto unknown;
			}
			break;

		case 'O':
			if (*fmt < '0' || '9' <= *fmt)
				goto unknown;
			sparc_emit_offset(node, *fmt++ - '0');
			break;

		case 'R': {
			arch_register_t const *const reg = va_arg(ap, const arch_register_t*);
			be_emit_char('%');
			be_emit_string(reg->name);
			break;
		}

		case 'S': {
			bool imm = false;
			if (*fmt == 'I') {
				imm = true;
				++fmt;
			}
			if (*fmt < '0' || '9' <= *fmt)
				goto unknown;
			unsigned const pos = *fmt++ - '0';
			if (imm && arch_get_irn_flags(node) & (arch_irn_flags_t)sparc_arch_irn_flag_immediate_form) {
				const sparc_attr_t *const attr = get_sparc_attr_const(node);
				sparc_emit_immediate(attr->immediate_value,
				                     attr->immediate_value_entity);
			} else {
				sparc_emit_source_register(node, pos);
			}
			break;
		}

		case 'd': {
			int const num = va_arg(ap, int);
			be_emit_irprintf(plus ? "%+d" : "%d", num);
			break;
		}

		case 's': {
			char const *const str = va_arg(ap, char const*);
			be_emit_string(str);
			break;
		}

		case 'u': {
			unsigned const num = va_arg(ap, unsigned);
			be_emit_irprintf(plus ? "%+u" : "%u", num);
			break;
		}

		case 'X': {
			unsigned const num = va_arg(ap, unsigned);
			be_emit_irprintf("%X", num);
			break;
		}

		default:
unknown:
			panic("unknown format conversion in sparc_emitf()");
		}
	}
	be_emit_finish_line_gas(node);
	va_end(ap);
}

/**
 * Emits code for stack space management
 */
static void emit_be_IncSP(const ir_node *irn)
{
	int offset = be_get_IncSP_offset(irn);

	if (offset == 0)
		return;

	/* SPARC stack grows downwards */
	char const *const insn = offset > 0 ? offset = -offset, "add" : "sub";
	sparc_emitf(irn, "%s %S0, %d, %D0", insn, offset);
}

static const char *emit_asm_operand(const ir_node *node, const char *s)
{
	assert(*s == '%');
	char c = *(++s);
	/* parse modifiers */
	if (c == '\0') {
		ir_fprintf(stderr, "Warning: asm text (%+F) ends with %%\n", node);
		be_emit_char('%');
		return s;
	} else if (c == '%') {
		be_emit_char('%');
		return s+1;
	} else if (c < '0' || c > '9') {
		ir_fprintf(stderr, "Warning: asm text (%+F) contains unknown modifier '%c' for asm op\n",
		           node, c);
		return s+1;
	}

	/* parse number */
	int num = 0;
	int p   = 0;
	sscanf(s, "%d%n", &num, &p);
	s += p;

	const sparc_asm_attr_t     *const attr     = get_sparc_asm_attr_const(node);
	const sparc_asm_operand_t  *const operands = attr->operands;
	if ((size_t)num > ARR_LEN(operands)) {
		ir_fprintf(stderr,
		           "Error: Custom assembler references invalid input/output (%+F)\n",
		           node);
		return s;
	}

	const sparc_asm_operand_t *const operand = &operands[num];
	const arch_register_t *reg = NULL;
	switch (operand->kind) {
	case ASM_OPERAND_IMMEDIATE:
		sparc_emit_immediate(operand->immediate_value,
		                     operand->immediate_value_entity);
		return s;
	case ASM_OPERAND_INPUT_VALUE:
		reg = arch_get_irn_register_in(node, operand->pos);
		break;
	case ASM_OPERAND_OUTPUT_VALUE:
		reg = arch_get_irn_register_out(node, operand->pos);
		break;
	}

	/* emit the register */
	sparc_emit_register(reg);
	return s;
}

static void emit_sparc_ASM(const ir_node *node)
{
	be_emit_cstring("#APP\n");
	be_emit_write_line();

	const sparc_asm_attr_t *attr = get_sparc_asm_attr_const(node);
	const char             *s    = get_id_str(attr->text);

	if (s[0] != '\t')
		be_emit_char('\t');
	while (*s != 0) {
		if (*s == '%') {
			s = emit_asm_operand(node, s);
		} else {
			be_emit_char(*s++);
		}
	}

	be_emit_cstring("\n#NO_APP\n");
	be_emit_write_line();

}

/**
 * Emits code for stack space management.
 */
static void emit_sparc_SubSP(const ir_node *irn)
{
	sparc_emitf(irn, "sub %S0, %SI1, %D0");
	sparc_emitf(irn, "add %S0, %u, %D1", SPARC_MIN_STACKSIZE);
}

static void fill_delay_slot(const ir_node *node)
{
	emitting_delay_slot = true;
	const ir_node *filler = pmap_get(ir_node, delay_slots, node);
	if (filler != NULL) {
		assert(!is_no_instruction(filler));
		assert(!emits_multiple_instructions(filler));
		be_emit_node(filler);
	} else {
		sparc_emitf(NULL, "nop");
	}
	emitting_delay_slot = false;
}

static void emit_sparc_Div(const ir_node *node, char const *const insn)
{
	sparc_emitf(node, "wr %S0, 0, %%y");

	/* TODO: we should specify number of delayslots in an architecture
	 * specification */
	unsigned wry_delay_count = 3;
	for (unsigned i = 0; i < wry_delay_count; ++i) {
		if (i == 0) {
			fill_delay_slot(node);
		} else {
			emitting_delay_slot = true;
			sparc_emitf(NULL, "nop");
			emitting_delay_slot = false;
		}
	}

	sparc_emitf(node, "%s %S1, %SI2, %D0", insn);
}

static void emit_sparc_SDiv(const ir_node *node)
{
	emit_sparc_Div(node, "sdiv");
}

static void emit_sparc_UDiv(const ir_node *node)
{
	emit_sparc_Div(node, "udiv");
}

static void emit_sparc_Call(const ir_node *node)
{
	if (is_sparc_reg_call(node)) {
		int dest_addr = get_sparc_Call_dest_addr_pos(node);
		sparc_emitf(node, "call %R", arch_get_irn_register_in(node, dest_addr));
	} else {
		sparc_emitf(node, "call %E, 0");
	}

	fill_delay_slot(node);

	if (arch_get_irn_flags(node) & sparc_arch_irn_flag_aggregate_return) {
		sparc_emitf(NULL, "unimp 8");
	}
}

static void emit_sparc_Cas(const ir_node *node)
{
	/* custom emiter is just here to check for should_be_same constraint
	 * which isn't guaranteed to be fulfilled in current firm backend */
	if (arch_get_irn_register_out(node, pn_sparc_Cas_res) !=
	    arch_get_irn_register_in(node, n_sparc_Cas_new)) {
	    panic("sparc: should_be_same in Cas not fulfilled");
	}
	/* except for some patched gaisler binutils nobody understands cas
	 * in v8/leon mode, so we encode the cas in binary form */
#if 0
	sparc_emitf(node, "cas [%S0], %S1, %S2");
#else
	const arch_register_t *reg_new
		= arch_get_irn_register_in(node, n_sparc_Cas_new);
	const arch_register_t *reg_ptr
		= arch_get_irn_register_in(node, n_sparc_Cas_ptr);
	const arch_register_t *reg_old
		= arch_get_irn_register_in(node, n_sparc_Cas_old);
	uint32_t encoding = 3u<<30 | (reg_new->encoding<<25) | (0x3C << 19)
	       | (reg_ptr->encoding<<14) | (0x80<<5) | (reg_old->encoding);
	sparc_emitf(node, ".long 0x%X  /* cas [%S0], %S1, %S2", encoding);
#endif
}

static void emit_be_Perm(const ir_node *irn)
{
	ir_mode *mode = get_irn_mode(get_irn_n(irn, 0));
	if (mode_is_float(mode)) {
		arch_register_t const *const reg0 = arch_get_irn_register_out(irn, 0);
		arch_register_t const *const reg1 = arch_get_irn_register_out(irn, 1);
		unsigned reg_idx0 = reg0->global_index;
		unsigned reg_idx1 = reg1->global_index;
		unsigned width    = arch_get_irn_register_req_out(irn, 0)->width;
		for (unsigned i = 0; i < width; ++i) {
			const arch_register_t *r0 = &sparc_registers[reg_idx0+i];
			const arch_register_t *r1 = &sparc_registers[reg_idx1+i];
			sparc_emitf(irn, "fmovs %R, %%f31", r0);
			sparc_emitf(irn, "fmovs %R, %R", r1, r0);
			sparc_emitf(irn, "fmovs %%f31, %R", r1);
		}
	} else {
		sparc_emitf(irn, "xor %D1, %D0, %D0");
		sparc_emitf(irn, "xor %D1, %D0, %D1");
		sparc_emitf(irn, "xor %D1, %D0, %D0");
	}
}

/* The stack pointer must always be SPARC_STACK_ALIGNMENT bytes aligned, so get
 * the next bigger integer that's evenly divisible by it. */
static unsigned get_aligned_sp_change(const unsigned num_regs)
{
	const unsigned bytes = num_regs * SPARC_REGISTER_SIZE;
	return round_up2(bytes, SPARC_STACK_ALIGNMENT);
}

/* Spill register l0 or both l0 and l1, depending on n_spilled and n_to_spill.*/
static void memperm_emit_spill_registers(const ir_node *node, int n_spilled,
                                         int n_to_spill)
{
	assert(n_spilled < n_to_spill);

	if (n_spilled == 0) {
		/* We always reserve stack space for two registers because during copy
		 * processing we don't know yet if we also need to handle a cycle which
		 * needs two registers.  More complicated code in emit_MemPerm would
		 * prevent wasting SPARC_REGISTER_SIZE bytes of stack space but
		 * it is not worth the worse readability of emit_MemPerm. */

		/* Keep stack pointer aligned. */
		unsigned sp_change = get_aligned_sp_change(2);
		sparc_emitf(node, "sub %%sp, %u, %%sp", sp_change);

		/* Spill register l0. */
		sparc_emitf(node, "st %%l0, [%%sp%+d]", SPARC_MIN_STACKSIZE);
	}

	if (n_to_spill == 2) {
		/* Spill register l1. */
		sparc_emitf(node, "st %%l1, [%%sp%+d]", SPARC_MIN_STACKSIZE + SPARC_REGISTER_SIZE);
	}
}

/* Restore register l0 or both l0 and l1, depending on n_spilled. */
static void memperm_emit_restore_registers(const ir_node *node, int n_spilled)
{
	if (n_spilled == 2) {
		/* Restore register l1. */
		sparc_emitf(node, "ld [%%sp%+d], %%l1", SPARC_MIN_STACKSIZE + SPARC_REGISTER_SIZE);
	}

	/* Restore register l0. */
	sparc_emitf(node, "ld [%%sp%+d], %%l0", SPARC_MIN_STACKSIZE);

	/* Restore stack pointer. */
	unsigned sp_change = get_aligned_sp_change(2);
	sparc_emitf(node, "add %%sp, %u, %%sp", sp_change);
}

/* Emit code to copy in_ent to out_ent.  Only uses l0. */
static void memperm_emit_copy(const ir_node *node, ir_entity *in_ent,
                              ir_entity *out_ent)
{
	ir_graph          *irg     = get_irn_irg(node);
	be_stack_layout_t *layout  = be_get_irg_stack_layout(irg);
	int                off_in  = be_get_stack_entity_offset(layout, in_ent, 0);
	int                off_out = be_get_stack_entity_offset(layout, out_ent, 0);

	/* Load from input entity. */
	sparc_emitf(node, "ld [%%fp%+d], %%l0", off_in);
	/* Store to output entity. */
	sparc_emitf(node, "st %%l0, [%%fp%+d]", off_out);
}

/* Emit code to swap ent1 and ent2.  Uses l0 and l1. */
static void memperm_emit_swap(const ir_node *node, ir_entity *ent1,
                              ir_entity *ent2)
{
	ir_graph          *irg     = get_irn_irg(node);
	be_stack_layout_t *layout  = be_get_irg_stack_layout(irg);
	int                off1    = be_get_stack_entity_offset(layout, ent1, 0);
	int                off2    = be_get_stack_entity_offset(layout, ent2, 0);

	/* Load from first input entity. */
	sparc_emitf(node, "ld [%%fp%+d], %%l0", off1);
	/* Load from second input entity. */
	sparc_emitf(node, "ld [%%fp%+d], %%l1", off2);
	/* Store first value to second output entity. */
	sparc_emitf(node, "st %%l0, [%%fp%+d]", off2);
	/* Store second value to first output entity. */
	sparc_emitf(node, "st %%l1, [%%fp%+d]", off1);
}

/* Find the index of ent in ents or return -1 if not found. */
static int get_index(ir_entity **ents, int n, ir_entity *ent)
{
	for (int i = 0; i < n; ++i) {
		if (ents[i] == ent)
			return i;
	}

	return -1;
}

/*
 * Emit code for a MemPerm node.
 *
 * Analyze MemPerm for copy chains and cyclic swaps and resolve them using
 * loads and stores.
 * This function is conceptually very similar to permute_values in
 * beprefalloc.c.
 */
static void emit_be_MemPerm(const ir_node *node)
{
	int         memperm_arity = be_get_MemPerm_entity_arity(node);
	/* Upper limit for the number of participating entities is twice the
	 * arity, e.g., for a simple copying MemPerm node with one input/output. */
	int         max_size      = 2 * memperm_arity;
	ir_entity **entities      = ALLOCANZ(ir_entity *, max_size);
	/* sourceof contains the input entity for each entity.  If an entity is
	 * never used as an output, its entry in sourceof is a fix point. */
	int        *sourceof      = ALLOCANZ(int,         max_size);
	/* n_users counts how many output entities use this entity as their input.*/
	int        *n_users       = ALLOCANZ(int,         max_size);
	/* n_spilled records the number of spilled registers, either 1 or 2. */
	int         n_spilled     = 0;

#ifndef NDEBUG
	/* This implementation currently only works with frame pointers. */
	ir_graph          *irg    = get_irn_irg(node);
	be_stack_layout_t *layout = be_get_irg_stack_layout(irg);
	assert(!layout->sp_relative && "MemPerms currently do not work without frame pointers");
#endif

	for (int i = 0; i < max_size; ++i) {
		sourceof[i] = i;
	}

	int n = 0;
	for (int i = 0; i < memperm_arity; ++i) {
		ir_entity *out  = be_get_MemPerm_out_entity(node, i);
		ir_entity *in   = be_get_MemPerm_in_entity(node, i);

		/* Insert into entities to be able to operate on unique indices. */
		if (get_index(entities, n, out) == -1)
			entities[n++] = out;
		if (get_index(entities, n, in) == -1)
			entities[n++] = in;

		int oidx = get_index(entities, n, out);
		int iidx = get_index(entities, n, in);

		sourceof[oidx] = iidx; /* Remember the source. */
		++n_users[iidx]; /* Increment number of users of this entity. */
	}

	/* First do all the copies. */
	for (int oidx = 0; oidx < n; /* empty */) {
		int iidx = sourceof[oidx];

		/* Nothing to do for fix points.
		 * Also, if entities[oidx] is used as an input by another copy, we
		 * can't overwrite entities[oidx] yet.*/
		if (iidx == oidx || n_users[oidx] > 0) {
			++oidx;
			continue;
		}

		/* We found the end of a 'chain', so do the copy. */
		if (n_spilled == 0) {
			memperm_emit_spill_registers(node, n_spilled, /*n_to_spill=*/1);
			n_spilled = 1;
		}
		memperm_emit_copy(node, entities[iidx], entities[oidx]);

		/* Mark as done. */
		sourceof[oidx] = oidx;

		assert(n_users[iidx] > 0);
		/* Decrementing the number of users might enable us to do another
		 * copy. */
		--n_users[iidx];

		if (iidx < oidx && n_users[iidx] == 0) {
			oidx = iidx;
		} else {
			++oidx;
		}
	}

	/* The rest are cycles. */
	for (int oidx = 0; oidx < n; /* empty */) {
		int iidx = sourceof[oidx];

		/* Nothing to do for fix points. */
		if (iidx == oidx) {
			++oidx;
			continue;
		}

		assert(n_users[iidx] == 1);

		/* Swap the two values to resolve the cycle. */
		if (n_spilled < 2) {
			memperm_emit_spill_registers(node, n_spilled, /*n_to_spill=*/2);
			n_spilled = 2;
		}
		memperm_emit_swap(node, entities[iidx], entities[oidx]);

		int tidx = sourceof[iidx];
		/* Mark as done. */
		sourceof[iidx] = iidx;

		/* The source of oidx is now the old source of iidx, because we swapped
		 * the two entities. */
		sourceof[oidx] = tidx;
	}

#ifdef DEBUG_libfirm
	/* Only fix points should remain. */
	for (int i = 0; i < max_size; ++i) {
		assert(sourceof[i] == i);
	}
#endif

	assert(n_spilled > 0 && "Useless MemPerm node");

	memperm_emit_restore_registers(node, n_spilled);
}

static void emit_sparc_Return(const ir_node *node)
{
	ir_graph  *irg    = get_irn_irg(node);
	ir_entity *entity = get_irg_entity(irg);
	ir_type   *type   = get_entity_type(entity);

	const char *destreg = "%o7";

	/* hack: we don't explicitely model register changes because of the
	 * restore node. So we have to do it manually here */
	const ir_node *delay_slot = pmap_get(ir_node, delay_slots, node);
	if (delay_slot != NULL &&
	    (is_sparc_Restore(delay_slot) || is_sparc_RestoreZero(delay_slot))) {
		destreg = "%i7";
	}
	char const *const offset = get_method_calling_convention(type) & cc_compound_ret ? "12" : "8";
	sparc_emitf(node, "jmp %s+%s", destreg, offset);
	fill_delay_slot(node);
}

static const arch_register_t *map_i_to_o_reg(const arch_register_t *reg)
{
	unsigned idx = reg->global_index;
	if (idx < REG_I0 || idx > REG_I7)
		return reg;
	idx += REG_O0 - REG_I0;
	assert(REG_O0 <= idx && idx <= REG_O7);
	return &sparc_registers[idx];
}

static void emit_sparc_Restore(const ir_node *node)
{
	const arch_register_t *destreg
		= arch_get_irn_register_out(node, pn_sparc_Restore_res);
	sparc_emitf(node, "restore %S2, %SI3, %R", map_i_to_o_reg(destreg));
}

static void emit_sparc_FrameAddr(const ir_node *node)
{
	const sparc_attr_t *attr   = get_sparc_attr_const(node);
	int32_t             offset = attr->immediate_value;

	char const *const insn = offset > 0 ? offset = -offset, "sub" : "add";
	assert(sparc_is_value_imm_encodeable(offset));
	sparc_emitf(node, "%s %S0, %d, %D0", insn, offset);
}

static const char *get_icc_unsigned(ir_relation relation)
{
	switch (relation & (ir_relation_less_equal_greater)) {
	case ir_relation_false:              return "bn";
	case ir_relation_equal:              return "be";
	case ir_relation_less:               return "blu";
	case ir_relation_less_equal:         return "bleu";
	case ir_relation_greater:            return "bgu";
	case ir_relation_greater_equal:      return "bgeu";
	case ir_relation_less_greater:       return "bne";
	case ir_relation_less_equal_greater: return "ba";
	default: panic("Cmp has unsupported relation");
	}
}

static const char *get_icc_signed(ir_relation relation)
{
	switch (relation & (ir_relation_less_equal_greater)) {
	case ir_relation_false:              return "bn";
	case ir_relation_equal:              return "be";
	case ir_relation_less:               return "bl";
	case ir_relation_less_equal:         return "ble";
	case ir_relation_greater:            return "bg";
	case ir_relation_greater_equal:      return "bge";
	case ir_relation_less_greater:       return "bne";
	case ir_relation_less_equal_greater: return "ba";
	default: panic("Cmp has unsupported relation");
	}
}

static const char *get_fcc(ir_relation relation)
{
	switch (relation) {
	case ir_relation_false:                   return "fbn";
	case ir_relation_equal:                   return "fbe";
	case ir_relation_less:                    return "fbl";
	case ir_relation_less_equal:              return "fble";
	case ir_relation_greater:                 return "fbg";
	case ir_relation_greater_equal:           return "fbge";
	case ir_relation_less_greater:            return "fblg";
	case ir_relation_less_equal_greater:      return "fbo";
	case ir_relation_unordered:               return "fbu";
	case ir_relation_unordered_equal:         return "fbue";
	case ir_relation_unordered_less:          return "fbul";
	case ir_relation_unordered_less_equal:    return "fbule";
	case ir_relation_unordered_greater:       return "fbug";
	case ir_relation_unordered_greater_equal: return "fbuge";
	case ir_relation_unordered_less_greater:  return "fbne";
	case ir_relation_true:                    return "fba";
	}
	panic("invalid relation");
}

typedef const char* (*get_cc_func)(ir_relation relation);

static void emit_sparc_branch(const ir_node *node, get_cc_func get_cc)
{
	const sparc_jmp_cond_attr_t *attr = get_sparc_jmp_cond_attr_const(node);
	ir_relation    relation    = attr->relation;
	const ir_node *proj_true   = NULL;
	const ir_node *proj_false  = NULL;

	assert((long)pn_sparc_Bicc_false == (long)pn_sparc_fbfcc_false);
	assert((long)pn_sparc_Bicc_true  == (long)pn_sparc_fbfcc_true);
	foreach_out_edge(node, edge) {
		ir_node *proj = get_edge_src_irn(edge);
		long nr = get_Proj_proj(proj);
		if (nr == pn_sparc_Bicc_true) {
			proj_true = proj;
		} else {
			assert(nr == pn_sparc_Bicc_false);
			proj_false = proj;
		}
	}

	/* emit the true proj */
	sparc_emitf(node, "%s%A %L", get_cc(relation), proj_true);
	fill_delay_slot(node);

	const ir_node *block      = get_nodes_block(node);
	const ir_node *next_block = (ir_node*)get_irn_link(block);

	if (get_jump_target(proj_false) == next_block) {
		if (be_options.verbose_asm) {
			sparc_emitf(node, "/* fallthrough to %L */", proj_false);
		}
	} else {
		sparc_emitf(node, "ba %L", proj_false);
		/* TODO: fill this slot as well */
		emitting_delay_slot = true;
		sparc_emitf(NULL, "nop");
		emitting_delay_slot = false;
	}
}

static void emit_sparc_Bicc(const ir_node *node)
{
	const sparc_jmp_cond_attr_t *attr = get_sparc_jmp_cond_attr_const(node);
	bool             is_unsigned = attr->is_unsigned;
	emit_sparc_branch(node, is_unsigned ? get_icc_unsigned : get_icc_signed);
}

static void emit_sparc_fbfcc(const ir_node *node)
{
	/* if the flags producing node was immediately in front of us, emit
	 * a nop */
	ir_node *flags = get_irn_n(node, n_sparc_fbfcc_flags);
	ir_node *prev  = sched_prev(node);
	if (is_Block(prev)) {
		/* TODO: when the flags come from another block, then we have to do
		 * more complicated tests to see whether the flag producing node is
		 * potentially in front of us (could happen for fallthroughs) */
		panic("TODO: fbfcc flags come from other block");
	}
	if (skip_Proj(flags) == prev) {
		sparc_emitf(NULL, "nop");
	}
	emit_sparc_branch(node, get_fcc);
}

static void emit_sparc_Ba(const ir_node *node)
{
	if (ba_is_fallthrough(node)) {
		if (be_options.verbose_asm) {
			sparc_emitf(node, "/* fallthrough to %L */", node);
		}
	} else {
		sparc_emitf(node, "ba %L", node);
		fill_delay_slot(node);
	}
}

static void emit_sparc_SwitchJmp(const ir_node *node)
{
	const sparc_switch_jmp_attr_t *attr = get_sparc_switch_jmp_attr_const(node);

	sparc_emitf(node, "jmp %S0");
	fill_delay_slot(node);

	be_emit_jump_table(node, attr->table, attr->table_entity, get_jump_target);
}

static void emit_fmov(const ir_node *node, const arch_register_t *src_reg,
                      const arch_register_t *dst_reg)
{
	sparc_emitf(node, "fmovs %R, %R", src_reg, dst_reg);
}

static const arch_register_t *get_next_fp_reg(const arch_register_t *reg)
{
	unsigned idx = reg->global_index;
	assert(reg == &sparc_registers[idx]);
	idx++;
	assert(idx - REG_F0 < N_sparc_fp_REGS);
	return &sparc_registers[idx];
}

static void emit_be_Copy(const ir_node *node)
{
	ir_mode               *mode    = get_irn_mode(node);
	const arch_register_t *src_reg = arch_get_irn_register_in(node, 0);
	const arch_register_t *dst_reg = arch_get_irn_register_out(node, 0);

	if (src_reg == dst_reg)
		return;

	if (mode_is_float(mode)) {
		unsigned bits = get_mode_size_bits(mode);
		int      n    = bits > 32 ? bits > 64 ? 3 : 1 : 0;
		emit_fmov(node, src_reg, dst_reg);
		for (int i = 0; i < n; ++i) {
			src_reg = get_next_fp_reg(src_reg);
			dst_reg = get_next_fp_reg(dst_reg);
			emit_fmov(node, src_reg, dst_reg);
		}
	} else if (mode_is_data(mode)) {
		sparc_emitf(node, "mov %S0, %D0");
	} else {
		panic("invalid mode");
	}
}

/**
 * Enters the emitter functions for handled nodes into the generic
 * pointer of an opcode.
 */
static void sparc_register_emitters(void)
{
	/* first clear the generic function pointer for all ops */
	ir_clear_opcodes_generic_func();
	/* register all emitter functions defined in spec */
	sparc_register_spec_emitters();

	/* custom emitter */
	be_set_emitter(op_be_Copy,         emit_be_Copy);
	be_set_emitter(op_be_CopyKeep,     emit_be_Copy);
	be_set_emitter(op_be_IncSP,        emit_be_IncSP);
	be_set_emitter(op_be_MemPerm,      emit_be_MemPerm);
	be_set_emitter(op_be_Perm,         emit_be_Perm);
	be_set_emitter(op_sparc_ASM,       emit_sparc_ASM);
	be_set_emitter(op_sparc_Ba,        emit_sparc_Ba);
	be_set_emitter(op_sparc_Bicc,      emit_sparc_Bicc);
	be_set_emitter(op_sparc_Call,      emit_sparc_Call);
	be_set_emitter(op_sparc_Cas,       emit_sparc_Cas);
	be_set_emitter(op_sparc_FrameAddr, emit_sparc_FrameAddr);
	be_set_emitter(op_sparc_Restore,   emit_sparc_Restore);
	be_set_emitter(op_sparc_Return,    emit_sparc_Return);
	be_set_emitter(op_sparc_SDiv,      emit_sparc_SDiv);
	be_set_emitter(op_sparc_SubSP,     emit_sparc_SubSP);
	be_set_emitter(op_sparc_SwitchJmp, emit_sparc_SwitchJmp);
	be_set_emitter(op_sparc_UDiv,      emit_sparc_UDiv);
	be_set_emitter(op_sparc_fbfcc,     emit_sparc_fbfcc);

	/* no need to emit anything for the following nodes */
	be_set_emitter(op_Phi,         be_emit_nothing);
	be_set_emitter(op_be_Keep,     be_emit_nothing);
	be_set_emitter(op_sparc_Start, be_emit_nothing);
}

static bool block_needs_label(const ir_node *block, const ir_node *sched_prev)
{
	if (get_Block_entity(block) != NULL)
		return true;

	int n_cfgpreds = get_Block_n_cfgpreds(block);
	if (n_cfgpreds == 0) {
		return false;
	} else if (n_cfgpreds > 1) {
		return true;
	} else {
		ir_node *cfgpred       = get_Block_cfgpred(block, 0);
		ir_node *cfgpred_block = get_nodes_block(cfgpred);
		if (is_Proj(cfgpred) && is_sparc_SwitchJmp(get_Proj_pred(cfgpred)))
			return true;
		return sched_prev != cfgpred_block || get_jump_target(cfgpred) != block;
	}
}

/**
 * Walks over the nodes in a block connected by scheduling edges
 * and emits code for each node.
 */
static void sparc_emit_block(ir_node *block, ir_node *prev)
{
	bool needs_label = block_needs_label(block, prev);
	be_gas_begin_block(block, needs_label);

	sched_foreach(block, node) {
		if (rbitset_is_set(delay_slot_fillers, get_irn_idx(node)))
			continue;
		be_emit_node(node);
	}
}

/**
 * Emits code for function start.
 */
static void sparc_emit_func_prolog(ir_graph *irg)
{
	ir_entity *entity = get_irg_entity(irg);
	be_gas_emit_function_prolog(entity, 4, NULL);
}

/**
 * Emits code for function end
 */
static void sparc_emit_func_epilog(ir_graph *irg)
{
	ir_entity *entity = get_irg_entity(irg);
	be_gas_emit_function_epilog(entity);
}

static void init_jump_links(ir_node *block, void *env)
{
	(void) env;

	int n = get_Block_n_cfgpreds(block);
	for (n--; n >= 0; n--) {
		ir_node *pred = get_Block_cfgpred(block, n);
		set_jump_target(pred, block);
	}
}

static int cmp_block_execfreqs(const void *d1, const void *d2)
{
	ir_node **p1 = (ir_node**)d1;
	ir_node **p2 = (ir_node**)d2;
	double freq1 = get_block_execfreq(*p1);
	double freq2 = get_block_execfreq(*p2);
	if (freq1 < freq2)
		return -1;
	if (freq1 > freq2)
		return 1;
	return get_irn_node_nr(*p2)-get_irn_node_nr(*p1);
}

static void pick_delay_slots(size_t n_blocks, ir_node **blocks)
{
	/* create blocklist sorted by execution frequency */
	ir_node **sorted_blocks = XMALLOCN(ir_node*, n_blocks);
	memcpy(sorted_blocks, blocks, n_blocks*sizeof(sorted_blocks[0]));
	qsort(sorted_blocks, n_blocks, sizeof(sorted_blocks[0]),
	      cmp_block_execfreqs);

	for (size_t i = 0; i < n_blocks; ++i) {
		sched_foreach(sorted_blocks[i], node) {
			if (!has_delay_slot(node))
				continue;
			ir_node *filler = pick_delay_slot_for(node);
			if (filler == NULL)
				continue;
			rbitset_set(delay_slot_fillers, get_irn_idx(filler));
			pmap_insert(delay_slots, node, filler);
		}
	}
}

void sparc_emit_routine(ir_graph *irg)
{
	heights            = heights_new(irg);
	delay_slot_fillers = rbitset_malloc(get_irg_last_idx(irg));
	delay_slots        = pmap_create();

	/* register all emitter functions */
	sparc_register_emitters();

	/* create the block schedule. For now, we don't need it earlier. */
	ir_node **block_schedule = be_create_block_schedule(irg);

	sparc_emit_func_prolog(irg);
	irg_block_walk_graph(irg, init_jump_links, NULL, NULL);

	/* inject block scheduling links & emit code of each block */
	size_t n_blocks = ARR_LEN(block_schedule);
	for (size_t i = 0; i < n_blocks; ++i) {
		ir_node *block      = block_schedule[i];
		ir_node *next_block = i+1 < n_blocks ? block_schedule[i+1] : NULL;
		set_irn_link(block, next_block);
	}

	pick_delay_slots(n_blocks, block_schedule);

	for (size_t i = 0; i < n_blocks; ++i) {
		ir_node *block = block_schedule[i];
		ir_node *prev  = i>=1 ? block_schedule[i-1] : NULL;
		if (block == get_irg_end_block(irg))
			continue;
		sparc_emit_block(block, prev);
	}

	/* emit function epilog */
	sparc_emit_func_epilog(irg);

	pmap_destroy(delay_slots);
	free(delay_slot_fillers);
	heights_free(heights);
}

void sparc_init_emitter(void)
{
	FIRM_DBG_REGISTER(dbg, "firm.be.sparc.emit");
}
