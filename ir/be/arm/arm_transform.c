/*
 * Copyright (C) 1995-2007 University of Karlsruhe.  All right reserved.
 *
 * This file is part of libFirm.
 *
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 *
 * Licensees holding valid libFirm Professional Edition licenses may use
 * this file in accordance with the libFirm Commercial License.
 * Agreement provided with the Software.
 *
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE.
 */

/**
 * @file
 * @brief   The codegenerator (transform FIRM into arm FIRM)
 * @author  Oliver Richter, Tobias Gneist, Michael Beck
 * @version $Id$
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "irnode_t.h"
#include "irgraph_t.h"
#include "irmode_t.h"
#include "irgmod.h"
#include "iredges.h"
#include "irvrfy.h"
#include "ircons.h"
#include "irprintf.h"
#include "dbginfo.h"
#include "iropt_t.h"
#include "debug.h"
#include "error.h"

#include "../benode_t.h"
#include "../beirg_t.h"
#include "../betranshlp.h"
#include "bearch_arm_t.h"

#include "arm_nodes_attr.h"
#include "archop.h"
#include "arm_transform.h"
#include "arm_new_nodes.h"
#include "arm_map_regs.h"

#include "gen_arm_regalloc_if.h"

#include <limits.h>


/** hold the current code generator during transformation */
static arm_code_gen_t *env_cg;

extern ir_op *get_op_Mulh(void);


/****************************************************************************************************
 *                  _        _                        __                           _   _
 *                 | |      | |                      / _|                         | | (_)
 *  _ __   ___   __| | ___  | |_ _ __ __ _ _ __  ___| |_ ___  _ __ _ __ ___   __ _| |_ _  ___  _ __
 * | '_ \ / _ \ / _` |/ _ \ | __| '__/ _` | '_ \/ __|  _/ _ \| '__| '_ ` _ \ / _` | __| |/ _ \| '_ \
 * | | | | (_) | (_| |  __/ | |_| | | (_| | | | \__ \ || (_) | |  | | | | | | (_| | |_| | (_) | | | |
 * |_| |_|\___/ \__,_|\___|  \__|_|  \__,_|_| |_|___/_| \___/|_|  |_| |_| |_|\__,_|\__|_|\___/|_| |_|
 *
 ****************************************************************************************************/

static INLINE int mode_needs_gp_reg(ir_mode *mode) {
	return mode_is_int(mode) || mode_is_character(mode) || mode_is_reference(mode);
}

typedef struct vals_ {
	int ops;
	unsigned char values[4];
	unsigned char shifts[4];
} vals;

/** Execute ROL. */
static unsigned do_rol(unsigned v, unsigned rol) {
	return (v << rol) | (v >> (32 - rol));
}

/**
 * construct 8bit values and rot amounts for a value
 */
static void gen_vals_from_word(unsigned int value, vals *result)
{
	int initial = 0;

	memset(result, 0, sizeof(*result));

	/* special case: we prefer shift amount 0 */
	if (value < 0x100) {
		result->values[0] = value;
		result->ops       = 1;
		return;
	}

	while (value != 0) {
		if (value & 0xFF) {
			unsigned v = do_rol(value, 8) & 0xFFFFFF;
			int shf = 0;
			for (;;) {
				if ((v & 3) != 0)
					break;
				shf += 2;
				v >>= 2;
			}
			v  &= 0xFF;
			shf = (initial + shf - 8) & 0x1F;
			result->values[result->ops] = v;
			result->shifts[result->ops] = shf;
			++result->ops;

			value ^= do_rol(v, shf) >> initial;
		}
		else {
			value >>= 8;
			initial += 8;
		}
	}
}

/**
 * Creates a arm_Const node.
 */
static ir_node *create_const_node(be_abi_irg_t *abi, ir_node *irn, ir_node *block, long value) {
	tarval   *tv   = new_tarval_from_long(value, mode_Iu);
	dbg_info *dbg  = get_irn_dbg_info(irn);
	ir_mode  *mode = get_irn_mode(irn);
	ir_node *res;

	if (mode_needs_gp_reg(mode))
		mode = mode_Iu;
	res = new_rd_arm_Mov_i(dbg, current_ir_graph, block, mode, tv);
	/* ensure the const is schedules AFTER the barrier */
	add_irn_dep(res, be_abi_get_start_barrier(abi));
	return res;
}

/**
 * Creates a arm_Const_Neg node.
 */
static ir_node *create_const_neg_node(be_abi_irg_t *abi, ir_node *irn, ir_node *block, long value) {
	tarval   *tv   = new_tarval_from_long(value, mode_Iu);
	dbg_info *dbg  = get_irn_dbg_info(irn);
	ir_mode  *mode = get_irn_mode(irn);
	ir_node *res;

	if (mode_needs_gp_reg(mode))
		mode = mode_Iu;
	res = new_rd_arm_Mvn_i(dbg, current_ir_graph, block, mode, tv);
	add_irn_dep(res, be_abi_get_start_barrier(abi));
	/* ensure the const is schedules AFTER the barrier */
	return res;
}

#define NEW_BINOP_NODE(opname, env, op1, op2) new_rd_arm_##opname(env->dbg, current_ir_graph, env->block, op1, op2, env->mode)

/**
 * Encodes an immediate with shifter operand
 */
static unsigned int arm_encode_imm_w_shift(unsigned int shift, unsigned int immediate) {
	return immediate | ((shift>>1)<<8);
}

/**
 * Decode an immediate with shifter operand
 */
unsigned int arm_decode_imm_w_shift(tarval *tv) {
	unsigned l = get_tarval_long(tv);
	unsigned rol = (l & ~0xFF) >> 7;

	return do_rol(l & 0xFF, rol);
}

/**
 * Creates a possible DAG for an constant.
 */
static ir_node *create_const_graph_value(be_abi_irg_t *abi, ir_node *irn, ir_node *block, unsigned int value) {
	ir_node *result;
	vals v, vn;
	int cnt;
	ir_mode *mode = get_irn_mode(irn);
	dbg_info *dbg = get_irn_dbg_info(irn);

	gen_vals_from_word(value, &v);
	gen_vals_from_word(~value, &vn);

	if (mode_needs_gp_reg(mode))
		mode = mode_Iu;

	if (vn.ops < v.ops) {
		/* remove bits */
		result = create_const_neg_node(abi, irn, block, arm_encode_imm_w_shift(vn.shifts[0], vn.values[0]));

		for (cnt = 1; cnt < vn.ops; ++cnt) {
			tarval *tv = new_tarval_from_long(arm_encode_imm_w_shift(vn.shifts[cnt], vn.values[cnt]), mode_Iu);
			ir_node *bic_i_node = new_rd_arm_Bic_i(dbg, current_ir_graph, block, result, mode, tv);
			result = bic_i_node;
		}
	}
	else {
		/* add bits */
		result = create_const_node(abi, irn, block, arm_encode_imm_w_shift(v.shifts[0], v.values[0]));

		for (cnt = 1; cnt < v.ops; ++cnt) {
			tarval *tv = new_tarval_from_long(arm_encode_imm_w_shift(v.shifts[cnt], v.values[cnt]), mode_Iu);
			ir_node *orr_i_node = new_rd_arm_Or_i(dbg, current_ir_graph, block, result, mode, tv);
			result = orr_i_node;
		}
	}
	return result;
}

/**
 * Create a DAG constructing a given Const.
 *
 * @param irn  a Firm const
 */
static ir_node *create_const_graph(be_abi_irg_t *abi, ir_node *irn, ir_node *block) {
	tarval  *tv = get_Const_tarval(irn);
	ir_mode *mode = get_tarval_mode(tv);
	int     value;

	if (mode_is_reference(mode)) {
		/* ARM is 32bit, so we can safely convert a reference tarval into Iu */
		assert(get_mode_size_bits(mode) == get_mode_size_bits(mode_Iu));
		tv = tarval_convert_to(tv, mode_Iu);
	}
	value = get_tarval_long(tv);
	return create_const_graph_value(abi, irn, block, value);
}

static ir_node *gen_mask(be_abi_irg_t *abi, ir_node *irn, ir_node *op, int result_bits) {
	ir_node *block = get_nodes_block(irn);
	unsigned mask_bits = (1 << result_bits) - 1;
	ir_node *mask_node = create_const_graph_value(abi, irn, block, mask_bits);
	dbg_info *dbg = get_irn_dbg_info(irn);
	return new_rd_arm_And(dbg, current_ir_graph, block, op, mask_node, get_irn_mode(irn), ARM_SHF_NONE, NULL);
}

static ir_node *gen_sign_extension(be_abi_irg_t *abi, ir_node *irn, ir_node *op, int result_bits) {
	ir_node *block = get_nodes_block(irn);
	int shift_width = 32 - result_bits;
	ir_graph *irg = current_ir_graph;
	ir_node *shift_const_node = create_const_graph_value(abi, irn, block, shift_width);
	dbg_info *dbg = get_irn_dbg_info(irn);
	ir_node *lshift_node = new_rd_arm_Shl(dbg, irg, block, op, shift_const_node, get_irn_mode(op));
	ir_node *rshift_node = new_rd_arm_Shrs(dbg, irg, block, lshift_node, shift_const_node, get_irn_mode(irn));
	return rshift_node;
}

/**
 * Transforms a Conv node.
 *
 * @return The created ia32 Conv node
 */
static ir_node *gen_Conv(ir_node *node) {
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *op       = get_Conv_op(node);
	ir_node  *new_op   = be_transform_node(op);
	ir_graph *irg      = current_ir_graph;
	ir_mode  *src_mode = get_irn_mode(op);
	ir_mode  *dst_mode = get_irn_mode(node);
	dbg_info *dbg      = get_irn_dbg_info(node);

	if (mode_needs_gp_reg(dst_mode))
		dst_mode = mode_Iu;

	if (src_mode == dst_mode)
		return new_op;

	if (mode_is_float(src_mode) || mode_is_float(dst_mode)) {
		env_cg->have_fp_insn = 1;

		if (USE_FPA(env_cg->isa)) {
			if (mode_is_float(src_mode)) {
				if (mode_is_float(dst_mode)) {
					/* from float to float */
					return new_rd_arm_fpaMov(dbg, irg, block, new_op, dst_mode);
				}
				else {
					/* from float to int */
					return new_rd_arm_fpaFix(dbg, irg, block, new_op, dst_mode);
				}
			}
			else {
				/* from int to float */
				return new_rd_arm_fpaFlt(dbg, irg, block, new_op, dst_mode);
			}
		}
		else if (USE_VFP(env_cg->isa)) {
			panic("VFP not supported yet\n");
			return NULL;
		}
		else {
			panic("Softfloat not supported yet\n");
			return NULL;
		}
	}
	else { /* complete in gp registers */
		int src_bits = get_mode_size_bits(src_mode);
		int dst_bits = get_mode_size_bits(dst_mode);
		int src_sign = get_mode_sign(src_mode);
		int dst_sign = get_mode_sign(dst_mode);

		if (src_bits == dst_bits) {
			/* kill 32 -> 32 convs */
			if (src_bits == 32) {
				return new_op;
			} else if (dst_bits < 32) {
			// 16 -> 16
				// unsigned -> unsigned
					// NOP
				// unsigned -> signed
					// sign extension (31:16)=(15)
				// signed -> unsigned
					// zero extension (31:16)=0
				// signed -> signed
					// NOP
				if (src_sign && !dst_sign) {
					return gen_mask(env_cg->birg->abi, node, new_op, dst_bits);
				} else {
					return gen_sign_extension(env_cg->birg->abi, node, new_op, dst_bits);
				}
			} else {
				panic("Cannot handle mode %+F with %d bits\n", dst_mode, dst_bits);
				return NULL;
			}
		}
		else if (src_bits < dst_bits) {
			// 16 -> 32
				// unsigned -> unsigned
					// NOP
				// unsigned -> signed
					// NOP
				// signed -> unsigned
					// sign extension (31:16)=(15)
				// signed -> signed
					// sign extension (31:16)=(15)
			if (src_sign) {
				return gen_sign_extension(env_cg->birg->abi, node, new_op, dst_bits);
			} else {
				return new_op;
			}
		}
		else {
			// 32 -> 16
				// unsigned -> unsigned
					// maskieren (31:16)=0
				// unsigned -> signed
					// maskieren (31:16)=0
				// signed -> unsigned
					// maskieren (31:16)=0
				// signed -> signed
					// sign extension (erledigt auch maskieren) (31:16)=(15)
			if (src_sign && dst_sign) {
				return gen_sign_extension(env_cg->birg->abi, node, new_op, dst_bits);
			} else {
				return gen_mask(env_cg->birg->abi, node, new_op, dst_bits);
			}
		}
	}
}

/**
 * Return true if an operand is a shifter operand
 */
static int is_shifter_operand(ir_node *n, arm_shift_modifier *pmod) {
	arm_shift_modifier mod = ARM_SHF_NONE;

	if (is_arm_Mov(n))
		mod = get_arm_shift_modifier(n);

	*pmod = mod;
	if (mod != ARM_SHF_NONE) {
		long v = get_tarval_long(get_arm_value(n));
		if (v < 32)
			return (int)v;
	}
	return 0;
}

/**
 * Creates an ARM Add.
 *
 * @return the created arm Add node
 */
static ir_node *gen_Add(ir_node *node) {
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *op1     = get_Add_left(node);
	ir_node  *new_op1 = be_transform_node(op1);
	ir_node  *op2     = get_Add_right(node);
	ir_node  *new_op2 = be_transform_node(op2);
	ir_mode  *mode    = get_irn_mode(node);
	ir_graph *irg     = current_ir_graph;
	ir_node  *new_op3;
	int v;
	arm_shift_modifier mod;
	dbg_info *dbg = get_irn_dbg_info(node);

	if (mode_is_float(mode)) {
		env_cg->have_fp_insn = 1;
		if (USE_FPA(env_cg->isa))
			return new_rd_arm_fpaAdd(dbg, irg, block, new_op1, new_op2, mode);
		else if (USE_VFP(env_cg->isa)) {
			assert(mode != mode_E && "IEEE Extended FP not supported");
			panic("VFP not supported yet\n");
			return NULL;
		}
		else {
			panic("Softfloat not supported yet\n");
			return NULL;
		}
	} else {
		assert(mode_is_numP(mode));

		if (is_arm_Mov_i(new_op1))
			return new_rd_arm_Add_i(dbg, irg, block, new_op2, mode, get_arm_value(new_op1));
		if (is_arm_Mov_i(new_op2))
			return new_rd_arm_Add_i(dbg, irg, block, new_op1, mode, get_arm_value(new_op2));

		/* check for MLA */
		if (is_arm_Mul(new_op1) && get_irn_n_edges(new_op1) == 1) {
			new_op3 = new_op2;
			op2 = get_irn_n(new_op1, 1);
			op1 = get_irn_n(new_op1, 0);

			return new_rd_arm_Mla(dbg, irg, block, new_op1, new_op2, new_op3, mode);
		}
		if (is_arm_Mul(new_op2) && get_irn_n_edges(new_op2) == 1) {
			new_op3 = new_op1;
			new_op1 = get_irn_n(new_op2, 0);
			new_op2 = get_irn_n(new_op2, 1);

			return new_rd_arm_Mla(dbg, irg, block, new_op1, new_op2, new_op3, mode);
		}

		/* is the first a shifter */
		v = is_shifter_operand(new_op1, &mod);
		if (v) {
			new_op1 = get_irn_n(new_op1, 0);
			return new_rd_arm_Add(dbg, irg, block, new_op2, new_op1, mode, mod, new_tarval_from_long(v, mode_Iu));
		}
		/* is the second a shifter */
		v = is_shifter_operand(new_op2, &mod);
		if (v) {
			new_op2 = get_irn_n(new_op2, 0);
			return new_rd_arm_Add(dbg, irg, block, new_op1, new_op2, mode, mod, new_tarval_from_long(v, mode_Iu));
		}

		/* normal ADD */
		return new_rd_arm_Add(dbg, irg, block, new_op1, new_op2, mode, ARM_SHF_NONE, NULL);
	}
}

/**
 * Creates an ARM Mul.
 *
 * @return the created arm Mul node
 */
static ir_node *gen_Mul(ir_node *node) {
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *op1     = get_Mul_left(node);
	ir_node  *new_op1 = be_transform_node(op1);
	ir_node  *op2     = get_Mul_right(node);
	ir_node  *new_op2 = be_transform_node(op2);
	ir_mode  *mode    = get_irn_mode(node);
	ir_graph *irg     = current_ir_graph;
	dbg_info *dbg     = get_irn_dbg_info(node);

	if (mode_is_float(mode)) {
		env_cg->have_fp_insn = 1;
		if (USE_FPA(env_cg->isa))
			return new_rd_arm_fpaMul(dbg, irg, block, new_op1, new_op2, mode);
		else if (USE_VFP(env_cg->isa)) {
			assert(mode != mode_E && "IEEE Extended FP not supported");
			panic("VFP not supported yet\n");
		}
		else {
			panic("Softfloat not supported yet\n");
			return NULL;
		}
	}
	return new_rd_arm_Mul(dbg, irg, block, new_op1, new_op2, mode);
}

/**
 * Creates an ARM floating point Div.
 *
 * @param env   The transformation environment
 * @return the created arm fDiv node
 */
static ir_node *gen_Quot(ir_node *node) {
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *op1     = get_Quot_left(node);
	ir_node  *new_op1 = be_transform_node(op1);
	ir_node  *op2     = get_Quot_right(node);
	ir_node  *new_op2 = be_transform_node(op2);
	ir_mode  *mode    = get_irn_mode(node);
	dbg_info *dbg     = get_irn_dbg_info(node);

	assert(mode != mode_E && "IEEE Extended FP not supported");

	env_cg->have_fp_insn = 1;
	if (USE_FPA(env_cg->isa))
		return new_rd_arm_fpaDiv(dbg, current_ir_graph, block, new_op1, new_op2, mode);
	else if (USE_VFP(env_cg->isa)) {
		assert(mode != mode_E && "IEEE Extended FP not supported");
		panic("VFP not supported yet\n");
	}
	else {
		panic("Softfloat not supported yet\n");
		return NULL;
	}
}

#define GEN_INT_OP(op) \
	ir_node  *block   = be_transform_node(get_nodes_block(node)); \
	ir_node  *op1     = get_ ## op ## _left(node); \
	ir_node  *new_op1 = be_transform_node(op1); \
	ir_node  *op2     = get_ ## op ## _right(node); \
	ir_node  *new_op2 = be_transform_node(op2); \
	ir_graph *irg     = current_ir_graph; \
	ir_mode  *mode    = get_irn_mode(node); \
	dbg_info *dbg     = get_irn_dbg_info(node); \
	int      v; \
	arm_shift_modifier mod; \
 \
	if (is_arm_Mov_i(new_op1)) \
		return new_rd_arm_ ## op ## _i(dbg, irg, block, new_op2, mode, get_arm_value(new_op1)); \
	if (is_arm_Mov_i(new_op2)) \
		return new_rd_arm_ ## op ## _i(dbg, irg, block, new_op1, mode, get_arm_value(new_op2)); \
	/* is the first a shifter */ \
	v = is_shifter_operand(new_op1, &mod); \
	if (v) { \
		new_op1 = get_irn_n(new_op1, 0); \
		return new_rd_arm_ ## op(dbg, irg, block, new_op2, new_op1, mode, mod, new_tarval_from_long(v, mode_Iu)); \
	} \
	/* is the second a shifter */ \
	v = is_shifter_operand(new_op2, &mod); \
	if (v) { \
		new_op2 = get_irn_n(new_op2, 0); \
		return new_rd_arm_ ## op(dbg, irg, block, new_op1, new_op2, mode, mod, new_tarval_from_long(v, mode_Iu)); \
	} \
	/* Normal op */ \
	return new_rd_arm_ ## op(dbg, irg, block, new_op1, new_op2, mode, ARM_SHF_NONE, NULL) \

/**
 * Creates an ARM And.
 *
 * @return the created arm And node
 */
static ir_node *gen_And(ir_node *node) {
	GEN_INT_OP(And);
}

/**
 * Creates an ARM Orr.
 *
 * @param env   The transformation environment
 * @return the created arm Or node
 */
static ir_node *gen_Or(ir_node *node) {
	GEN_INT_OP(Or);
}

/**
 * Creates an ARM Eor.
 *
 * @return the created arm Eor node
 */
static ir_node *gen_Eor(ir_node *node) {
	GEN_INT_OP(Eor);
}

/**
 * Creates an ARM Sub.
 *
 * @return the created arm Sub node
 */
static ir_node *gen_Sub(ir_node *node) {
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *op1     = get_Sub_left(node);
	ir_node  *new_op1 = be_transform_node(op1);
	ir_node  *op2     = get_Sub_right(node);
	ir_node  *new_op2 = be_transform_node(op2);
	ir_mode  *mode    = get_irn_mode(node);
	ir_graph *irg     = current_ir_graph;
	dbg_info *dbg     = get_irn_dbg_info(node);
	int      v;
	arm_shift_modifier mod;

	if (mode_is_float(mode)) {
		env_cg->have_fp_insn = 1;
		if (USE_FPA(env_cg->isa))
			return new_rd_arm_fpaSub(dbg, irg, block, new_op1, new_op2, mode);
		else if (USE_VFP(env_cg->isa)) {
			assert(mode != mode_E && "IEEE Extended FP not supported");
			panic("VFP not supported yet\n");
			return NULL;
		}
		else {
			panic("Softfloat not supported yet\n");
			return NULL;
		}
	}
	else {
		assert(mode_is_numP(mode) && "unknown mode for Sub");

		if (is_arm_Mov_i(new_op1))
			return new_rd_arm_Rsb_i(dbg, irg, block, new_op2, mode, get_arm_value(new_op1));
		if (is_arm_Mov_i(new_op2))
			return new_rd_arm_Sub_i(dbg, irg, block, new_op1, mode, get_arm_value(new_op2));

		/* is the first a shifter */
		v = is_shifter_operand(new_op1, &mod);
		if (v) {
			new_op1 = get_irn_n(new_op1, 0);
			return new_rd_arm_Rsb(dbg, irg, block, new_op2, new_op1, mode, mod, new_tarval_from_long(v, mode_Iu));
		}
		/* is the second a shifter */
		v = is_shifter_operand(new_op2, &mod);
		if (v) {
			new_op2 = get_irn_n(new_op2, 0);
			return new_rd_arm_Sub(dbg, irg, block, new_op1, new_op2, mode, mod, new_tarval_from_long(v, mode_Iu));
		}
		/* normal sub */
		return new_rd_arm_Sub(dbg, irg, block, new_op1, new_op2, mode, ARM_SHF_NONE, NULL);
	}
}

/**
 * Creates an ARM Shl.
 *
 * @return the created ARM Shl node
 */
static ir_node *gen_Shl(ir_node *node) {
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *op1     = get_Shl_left(node);
	ir_node  *new_op1 = be_transform_node(op1);
	ir_node  *op2     = get_Shl_right(node);
	ir_node  *new_op2 = be_transform_node(op2);
	ir_mode  *mode    = get_irn_mode(node);
	dbg_info *dbg     = get_irn_dbg_info(node);

	if (is_arm_Mov_i(new_op2)) {
		return new_rd_arm_Mov(dbg, current_ir_graph, block, new_op1, mode, ARM_SHF_LSL, get_arm_value(new_op2));
	}
	return new_rd_arm_Shl(dbg, current_ir_graph, block, new_op1, new_op2, mode);
}

/**
 * Creates an ARM Shr.
 *
 * @return the created ARM Shr node
 */
static ir_node *gen_Shr(ir_node *node) {
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *op1     = get_Shr_left(node);
	ir_node  *new_op1 = be_transform_node(op1);
	ir_node  *op2     = get_Shr_right(node);
	ir_node  *new_op2 = be_transform_node(op2);
	ir_mode  *mode    = get_irn_mode(node);
	dbg_info *dbg     = get_irn_dbg_info(node);

	if (is_arm_Mov_i(new_op2)) {
		return new_rd_arm_Mov(dbg, current_ir_graph, block, new_op1, mode, ARM_SHF_LSR, get_arm_value(new_op2));
	}
	return new_rd_arm_Shr(dbg, current_ir_graph, block, new_op1, new_op2, mode);
}

/**
 * Creates an ARM Shrs.
 *
 * @return the created ARM Shrs node
 */
static ir_node *gen_Shrs(ir_node *node) {
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *op1     = get_Shrs_left(node);
	ir_node  *new_op1 = be_transform_node(op1);
	ir_node  *op2     = get_Shrs_right(node);
	ir_node  *new_op2 = be_transform_node(op2);
	ir_mode  *mode    = get_irn_mode(node);
	dbg_info *dbg     = get_irn_dbg_info(node);

	if (is_arm_Mov_i(new_op2)) {
		return new_rd_arm_Mov(dbg, current_ir_graph, block, new_op1, mode, ARM_SHF_ASR, get_arm_value(op2));
	}
	return new_rd_arm_Shrs(dbg, current_ir_graph, block, new_op1, new_op2, mode);
}

/**
 * Transforms a Not node.
 *
 * @return the created ARM Not node
 */
static ir_node *gen_Not(ir_node *node) {
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *op      = get_Not_op(node);
	ir_node  *new_op  = be_transform_node(op);
	dbg_info *dbg     = get_irn_dbg_info(node);
	tarval   *tv      = NULL;
	arm_shift_modifier mod = ARM_SHF_NONE;
	int      v        = is_shifter_operand(new_op, &mod);

	if (v) {
		new_op = get_irn_n(new_op, 0);
		tv = new_tarval_from_long(v, mode_Iu);
	}
	return new_rd_arm_Mvn(dbg, current_ir_graph, block, new_op, get_irn_mode(node), mod, tv);
}

/**
 * Transforms an Abs node.
 *
 * @param env   The transformation environment
 * @return the created ARM Abs node
 */
static ir_node *gen_Abs(ir_node *node) {
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *op      = get_Not_op(node);
	ir_node  *new_op  = be_transform_node(op);
	dbg_info *dbg     = get_irn_dbg_info(node);
	ir_mode  *mode    = get_irn_mode(node);

	if (mode_is_float(mode)) {
		env_cg->have_fp_insn = 1;
		if (USE_FPA(env_cg->isa))
			return new_rd_arm_fpaAbs(dbg, current_ir_graph, block, new_op, mode);
		else if (USE_VFP(env_cg->isa)) {
			assert(mode != mode_E && "IEEE Extended FP not supported");
			panic("VFP not supported yet\n");
		}
		else {
			panic("Softfloat not supported yet\n");
		}
	}
	return new_rd_arm_Abs(dbg, current_ir_graph, block, new_op, mode);
}

/**
 * Transforms a Minus node.
 *
 * @return the created ARM Minus node
 */
static ir_node *gen_Minus(ir_node *node) {
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *op      = get_Minus_op(node);
	ir_node  *new_op  = be_transform_node(op);
	dbg_info *dbg     = get_irn_dbg_info(node);
	ir_mode  *mode    = get_irn_mode(node);

	if (mode_is_float(mode)) {
		env_cg->have_fp_insn = 1;
		if (USE_FPA(env_cg->isa))
			return new_rd_arm_fpaMnv(dbg, current_ir_graph, block, op, mode);
		else if (USE_VFP(env_cg->isa)) {
			assert(mode != mode_E && "IEEE Extended FP not supported");
			panic("VFP not supported yet\n");
		}
		else {
			panic("Softfloat not supported yet\n");
		}
	}
	return new_rd_arm_Rsb_i(dbg, current_ir_graph, block, new_op, mode, get_mode_null(mode));
}

/**
 * Transforms a Load.
 *
 * @return the created ARM Load node
 */
static ir_node *gen_Load(ir_node *node) {
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *ptr      = get_Load_ptr(node);
	ir_node  *new_ptr  = be_transform_node(ptr);
	ir_node  *mem      = get_Load_mem(node);
	ir_node  *new_mem  = be_transform_node(mem);
	ir_mode  *mode     = get_Load_mode(node);
	ir_graph *irg      = current_ir_graph;
	dbg_info *dbg      = get_irn_dbg_info(node);
	ir_node  *new_load = NULL;

	if (mode_is_float(mode)) {
		env_cg->have_fp_insn = 1;
		if (USE_FPA(env_cg->isa))
			new_load = new_rd_arm_fpaLdf(dbg, irg, block, new_ptr, new_mem, mode);
		else if (USE_VFP(env_cg->isa)) {
			assert(mode != mode_E && "IEEE Extended FP not supported");
			panic("VFP not supported yet\n");
		}
		else {
			panic("Softfloat not supported yet\n");
		}
	}
	else {
		assert(mode_is_numP(mode) && "unsupported mode for Load");

		if (mode_is_signed(mode)) {
			/* sign extended loads */
			switch (get_mode_size_bits(mode)) {
			case 8:
				new_load = new_rd_arm_Loadbs(dbg, irg, block, new_ptr, new_mem);
			case 16:
				new_load = new_rd_arm_Loadhs(dbg, irg, block, new_ptr, new_mem);
			case 32:
				break;
			default:
				panic("mode size not supported\n");
			}
		} else {
			/* zero extended loads */
			switch (get_mode_size_bits(mode)) {
			case 8:
				new_load = new_rd_arm_Loadb(dbg, irg, block, new_ptr, new_mem);
			case 16:
				new_load = new_rd_arm_Loadh(dbg, irg, block, new_ptr, new_mem);
			case 32:
				break;
			default:
				panic("mode size not supported\n");
			}
		}
		new_load = new_rd_arm_Load(dbg, irg, block, new_ptr, new_mem);
	}
	set_irn_pinned(new_load, get_irn_pinned(node));
	return new_load;
}

/**
 * Transforms a Store.
 *
 * @return the created ARM Store node
 */
static ir_node *gen_Store(ir_node *node) {
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *ptr      = get_Store_ptr(node);
	ir_node  *new_ptr  = be_transform_node(ptr);
	ir_node  *mem      = get_Store_mem(node);
	ir_node  *new_mem  = be_transform_node(mem);
	ir_node  *val      = get_Store_value(node);
	ir_node  *new_val  = be_transform_node(val);
	ir_mode  *mode     = get_irn_mode(val);
	ir_graph *irg      = current_ir_graph;
	dbg_info *dbg      = get_irn_dbg_info(node);
	ir_node *new_store = NULL;

	if (mode_is_float(mode)) {
		env_cg->have_fp_insn = 1;
		if (USE_FPA(env_cg->isa))
			new_store = new_rd_arm_fpaStf(dbg, irg, block, new_ptr, new_val, new_mem, mode);
		else if (USE_VFP(env_cg->isa)) {
			assert(mode != mode_E && "IEEE Extended FP not supported");
			panic("VFP not supported yet\n");
		} else {
			panic("Softfloat not supported yet\n");
		}
	} else {
		assert(mode_is_numP(mode) && "unsupported mode for Store");
		switch (get_mode_size_bits(mode)) {
		case 8:
			new_store = new_rd_arm_Storeb(dbg, irg, block, new_ptr, new_val, new_mem);
		case 16:
			new_store = new_rd_arm_Storeh(dbg, irg, block, new_ptr, new_val, new_mem);
		default:
			new_store = new_rd_arm_Store(dbg, irg, block, new_ptr, new_val, new_mem);
		}
	}
	set_irn_pinned(new_store, get_irn_pinned(node));
	return new_store;
}

/**
 * Transforms a Cond.
 *
 * @return the created ARM Cond node
 */
static ir_node *gen_Cond(ir_node *node) {
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *selector = get_Cond_selector(node);
	ir_graph *irg      = current_ir_graph;
	dbg_info *dbg      = get_irn_dbg_info(node);
	ir_mode  *mode     = get_irn_mode(selector);

	if (mode == mode_b) {
		/* CondJmp */
		ir_node *cmp_node = get_Proj_pred(selector);
		ir_node *op1      = get_Cmp_left(cmp_node);
		ir_node *new_op1  = be_transform_node(op1);
		ir_node *op2      = get_Cmp_right(cmp_node);
		ir_node *new_op2  = be_transform_node(op2);

		return new_rd_arm_CondJmp(dbg, irg, block, new_op1, new_op2, get_Proj_proj(selector));
	} else {
		/* SwitchJmp */
		ir_node *new_op = be_transform_node(selector);
		ir_node *const_graph;
		ir_node *sub;

		ir_node *proj;
		const ir_edge_t *edge;
		int min = INT_MAX;
		int max = INT_MIN;
		int translation;
		int pn;
		int n_projs;

		foreach_out_edge(node, edge) {
			proj = get_edge_src_irn(edge);
			assert(is_Proj(proj) && "Only proj allowed at SwitchJmp");

			pn = get_Proj_proj(proj);

			min = pn<min ? pn : min;
			max = pn>max ? pn : max;
		}
		translation = min;
		n_projs = max - translation + 1;

		foreach_out_edge(node, edge) {
			proj = get_edge_src_irn(edge);
			assert(is_Proj(proj) && "Only proj allowed at SwitchJmp");

			pn = get_Proj_proj(proj) - translation;
			set_Proj_proj(proj, pn);
		}


		const_graph = create_const_graph_value(env_cg->birg->abi, node, block, translation);
		sub = new_rd_arm_Sub(dbg, irg, block, new_op, const_graph, mode, ARM_SHF_NONE, NULL);
		return new_rd_arm_SwitchJmp(dbg, irg, block, sub, n_projs, get_Cond_defaultProj(node) - translation);
	}
}

/**
 * Returns the name of a SymConst.
 * @param symc  the SymConst
 * @return name of the SymConst
 */
static ident *get_sc_ident(ir_node *symc) {
	ir_entity *ent;

	switch (get_SymConst_kind(symc)) {
		case symconst_addr_name:
			return get_SymConst_name(symc);

		case symconst_addr_ent:
			ent = get_SymConst_entity(symc);
			mark_entity_visited(ent);
			return get_entity_ld_ident(ent);

		default:
			assert(0 && "Unsupported SymConst");
	}

	return NULL;
}

/**
 * Transforms a Const node.
 *
 * @return The transformed ARM node.
 */
static ir_node *gen_Const(ir_node *node) {
	ir_node  *block = be_transform_node(get_nodes_block(node));
	ir_graph *irg = current_ir_graph;
	ir_mode *mode = get_irn_mode(node);
	dbg_info *dbg = get_irn_dbg_info(node);

	if (mode_is_float(mode)) {
		env_cg->have_fp_insn = 1;
		if (USE_FPA(env_cg->isa)) {
			node = new_rd_arm_fpaConst(dbg, irg, block, get_Const_tarval(node));
			/* ensure the const is schedules AFTER the barrier */
			add_irn_dep(node, be_abi_get_start_barrier(env_cg->birg->abi));
			return node;
		}
		else if (USE_VFP(env_cg->isa)) {
			assert(mode != mode_E && "IEEE Extended FP not supported");
			panic("VFP not supported yet\n");
		}
		else {
			panic("Softfloat not supported yet\n");
		}
	}
	return create_const_graph(env_cg->birg->abi, node, block);
}

/**
 * Transforms a SymConst node.
 *
 * @return The transformed ARM node.
 */
static ir_node *gen_SymConst(ir_node *node) {
	ir_node  *block = be_transform_node(get_nodes_block(node));
	ir_mode  *mode  = get_irn_mode(node);
	dbg_info *dbg   = get_irn_dbg_info(node);
	ir_node  *res;

	res = new_rd_arm_SymConst(dbg, current_ir_graph, block, mode, get_sc_ident(node));
	add_irn_dep(res, be_abi_get_start_barrier(env_cg->birg->abi));
	/* ensure the const is schedules AFTER the barrier */
	return res;
}

/**
 * Transforms a CopyB node.
 *
 * @return The transformed ARM node.
 */
static ir_node *gen_CopyB(ir_node *node) {
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *src      = get_CopyB_src(node);
	ir_node  *new_src  = be_transform_node(src);
	ir_node  *dst      = get_CopyB_dst(node);
	ir_node  *new_dst  = be_transform_node(dst);
	ir_node  *mem      = get_CopyB_mem(node);
	ir_node  *new_mem  = be_transform_node(mem);
	ir_graph *irg      = current_ir_graph;
	dbg_info *dbg      = get_irn_dbg_info(node);
	int      size      = get_type_size_bytes(get_CopyB_type(node));
	ir_node  *src_copy;
	ir_node  *dst_copy;

	src_copy = be_new_Copy(&arm_reg_classes[CLASS_arm_gp], irg, block, new_src);
	dst_copy = be_new_Copy(&arm_reg_classes[CLASS_arm_gp], irg, block, new_dst);

 	return new_rd_arm_CopyB(dbg, irg, block, dst_copy, src_copy,
			new_rd_arm_EmptyReg(dbg, irg, block, mode_Iu),
			new_rd_arm_EmptyReg(dbg, irg, block, mode_Iu),
			new_rd_arm_EmptyReg(dbg, irg, block, mode_Iu),
			new_mem, new_tarval_from_long(size, mode_Iu));
}


/********************************************
 *  _                          _
 * | |                        | |
 * | |__   ___ _ __   ___   __| | ___  ___
 * | '_ \ / _ \ '_ \ / _ \ / _` |/ _ \/ __|
 * | |_) |  __/ | | | (_) | (_| |  __/\__ \
 * |_.__/ \___|_| |_|\___/ \__,_|\___||___/
 *
 ********************************************/

/**
 * Return an expanding stack offset.
 * Note that function is called in the transform phase
 * where the stack offsets are still relative regarding
 * the first (frame allocating) IncSP.
 * However this is exactly what we want because frame
 * access must be done relative the the fist IncSP ...
 */
static int get_sp_expand_offset(ir_node *inc_sp) {
	int offset = be_get_IncSP_offset(inc_sp);

	if (offset == BE_STACK_FRAME_SIZE_EXPAND)
		return 0;

	return offset;
}

#if 0
static ir_node *gen_StackParam(ir_node *irn) {
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node   *new_op = NULL;
	ir_node   *noreg  = ia32_new_NoReg_gp(env->cg);
	ir_node   *mem    = new_rd_NoMem(env->irg);
	ir_node   *ptr    = get_irn_n(irn, 0);
	ir_entity *ent    = be_get_frame_entity(irn);
	ir_mode   *mode   = env->mode;

//	/* If the StackParam has only one user ->     */
//	/* put it in the Block where the user resides */
//	if (get_irn_n_edges(node) == 1) {
//		env->block = get_nodes_block(get_edge_src_irn(get_irn_out_edge_first(node)));
//	}

	if (mode_is_float(mode)) {
		if (USE_SSE2(env->cg))
			new_op = new_rd_ia32_fLoad(env->dbg, env->irg, block, ptr, noreg, mem, mode_T);
		else {
			env->cg->used_x87 = 1;
			new_op = new_rd_ia32_vfld(env->dbg, env->irg, block, ptr, noreg, mem, mode_T);
		}
	}
	else {
		new_op = new_rd_ia32_Load(env->dbg, env->irg, block, ptr, noreg, mem, mode_T);
	}

	set_ia32_frame_ent(new_op, ent);
	set_ia32_use_frame(new_op);

	set_ia32_am_support(new_op, ia32_am_Source);
	set_ia32_op_type(new_op, ia32_AddrModeS);
	set_ia32_am_flavour(new_op, ia32_B);
	set_ia32_ls_mode(new_op, mode);

	SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env->cg, env->irn));

	return new_rd_Proj(env->dbg, env->irg, block, new_op, mode, 0);
}
#endif

/**
 * Transforms a FrameAddr into an ARM Add.
 */
static ir_node *gen_be_FrameAddr(ir_node *node) {
	ir_node   *block  = be_transform_node(get_nodes_block(node));
	ir_entity *ent    = be_get_frame_entity(node);
	int       offset  = get_entity_offset(ent);
	ir_node   *op     = be_get_FrameAddr_frame(node);
	ir_node   *new_op = be_transform_node(op);
	ir_mode   *mode   = get_irn_mode(node);
	dbg_info  *dbg    = get_irn_dbg_info(node);
	ir_node   *cnst;

	if (be_is_IncSP(op)) {
		/* BEWARE: we get an offset which is absolute from an offset that
		   is relative. Both must be merged */
		offset += get_sp_expand_offset(op);
	}
	cnst = create_const_graph_value(env_cg->birg->abi, node, block, (unsigned)offset);
	if (is_arm_Mov_i(cnst))
		return new_rd_arm_Add_i(dbg, current_ir_graph, block, new_op, mode, get_arm_value(cnst));
	return new_rd_arm_Add(dbg, current_ir_graph, block, new_op, cnst, mode, ARM_SHF_NONE, NULL);
}

#if 0
/**
 * Transforms a FrameLoad into an ARM Load.
 */
static ir_node *gen_FrameLoad(ir_node *irn) {
	ir_node   *new_op = NULL;
	ir_node   *noreg  = ia32_new_NoReg_gp(env->cg);
	ir_node   *mem    = get_irn_n(irn, 0);
	ir_node   *ptr    = get_irn_n(irn, 1);
	ir_entity *ent    = be_get_frame_entity(irn);
	ir_mode   *mode   = get_type_mode(get_entity_type(ent));

	if (mode_is_float(mode)) {
		if (USE_SSE2(env->cg))
			new_op = new_rd_ia32_fLoad(env->dbg, current_ir_graph, env->block, ptr, noreg, mem, mode_T);
		else {
			env->cg->used_x87 = 1;
			new_op = new_rd_ia32_vfld(env->dbg, current_ir_graph, env->block, ptr, noreg, mem, mode_T);
		}
	}
	else {
		new_op = new_rd_ia32_Load(env->dbg, current_ir_graph, env->block, ptr, noreg, mem, mode_T);
	}

	set_ia32_frame_ent(new_op, ent);
	set_ia32_use_frame(new_op);

	set_ia32_am_support(new_op, ia32_am_Source);
	set_ia32_op_type(new_op, ia32_AddrModeS);
	set_ia32_am_flavour(new_op, ia32_B);
	set_ia32_ls_mode(new_op, mode);

	SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(env->cg, env->irn));

	return new_op;
}
#endif

/**
 * Transform a be_AddSP into an arm_AddSP. Eat up const sizes.
 */
static ir_node *gen_be_AddSP(ir_node *node) {
	ir_node  *block  = be_transform_node(get_nodes_block(node));
	ir_node  *sz     = get_irn_n(node, be_pos_AddSP_size);
	ir_node  *new_sz = be_transform_node(sz);
	ir_node  *sp     = get_irn_n(node, be_pos_AddSP_old_sp);
	ir_node  *new_sp = be_transform_node(sp);
	ir_graph *irg    = current_ir_graph;
	dbg_info *dbgi   = get_irn_dbg_info(node);
	ir_node  *nomem  = new_NoMem();
	ir_node  *new_op;

	/* ARM stack grows in reverse direction, make a SubSP */
	new_op = new_rd_arm_SubSP(dbgi, irg, block, new_sp, new_sz, nomem);

	return new_op;
}

/**
 * Transform a be_SubSP into an arm_SubSP. Eat up const sizes.
 */
static ir_node *gen_be_SubSP(ir_node *node) {
	ir_node  *block  = be_transform_node(get_nodes_block(node));
	ir_node  *sz     = get_irn_n(node, be_pos_SubSP_size);
	ir_node  *new_sz = be_transform_node(sz);
	ir_node  *sp     = get_irn_n(node, be_pos_SubSP_old_sp);
	ir_node  *new_sp = be_transform_node(sp);
	ir_graph *irg    = current_ir_graph;
	dbg_info *dbgi   = get_irn_dbg_info(node);
	ir_node  *nomem  = new_NoMem();
	ir_node  *new_op;

	/* ARM stack grows in reverse direction, make an AddSP */
	new_op = new_rd_arm_AddSP(dbgi, irg, block, new_sp, new_sz, nomem);

	return new_op;
}

/**
 * Transform a be_Copy.
 */
static ir_node *gen_be_Copy(ir_node *node) {
	ir_node *result = be_duplicate_node(node);
	ir_mode *mode   = get_irn_mode(result);

	if (mode_needs_gp_reg(mode)) {
		set_irn_mode(node, mode_Iu);
	}

	return result;
}

/**
 * Transform a Proj from a Load.
 */
static ir_node *gen_Proj_Load(ir_node *node) {
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *load     = get_Proj_pred(node);
	ir_node  *new_load = be_transform_node(load);
	ir_graph *irg      = current_ir_graph;
	dbg_info *dbgi     = get_irn_dbg_info(node);
	long     proj      = get_Proj_proj(node);

	/* renumber the proj */
	switch (get_arm_irn_opcode(new_load)) {
	case iro_arm_Load:
	case iro_arm_Loadb:
	case iro_arm_Loadbs:
	case iro_arm_Loadh:
	case iro_arm_Loadhs:
		/* handle all gp loads equal: they have the same proj numbers. */
		if (proj == pn_Load_res) {
			return new_rd_Proj(dbgi, irg, block, new_load, mode_Iu, pn_arm_Load_res);
		} else if (proj == pn_Load_M) {
			return new_rd_Proj(dbgi, irg, block, new_load, mode_M, pn_arm_Load_M);
		}
		break;
	case iro_arm_fpaLdf:
		if (proj == pn_Load_res) {
			ir_mode *mode = get_Load_mode(load);
			return new_rd_Proj(dbgi, irg, block, new_load, mode, pn_arm_fpaLdf_res);
		} else if (proj == pn_Load_M) {
			return new_rd_Proj(dbgi, irg, block, new_load, mode_M, pn_arm_fpaLdf_M);
		}
		break;
	default:
		break;
	}
	assert(0);
	return new_rd_Unknown(irg, get_irn_mode(node));
}

/**
 * Transform and renumber the Projs from a CopyB.
 */
static ir_node *gen_Proj_CopyB(ir_node *node) {
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *pred     = get_Proj_pred(node);
	ir_node  *new_pred = be_transform_node(pred);
	ir_graph *irg      = current_ir_graph;
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_mode  *mode     = get_irn_mode(node);
	long     proj      = get_Proj_proj(node);

	switch(proj) {
	case pn_CopyB_M_regular:
		if (is_arm_CopyB(new_pred)) {
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_M, pn_arm_CopyB_M);
		}
		break;
	default:
		break;
	}
	assert(0);
	return new_rd_Unknown(irg, mode);
}

/**
 * Transform and renumber the Projs from a Quot.
 */
static ir_node *gen_Proj_Quot(ir_node *node) {
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *pred     = get_Proj_pred(node);
	ir_node  *new_pred = be_transform_node(pred);
	ir_graph *irg      = current_ir_graph;
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_mode  *mode     = get_irn_mode(node);
	long     proj      = get_Proj_proj(node);

	switch (proj) {
	case pn_Quot_M:
		if (is_arm_fpaDiv(new_pred)) {
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_M, pn_arm_fpaDiv_M);
		} else if (is_arm_fpaRdv(new_pred)) {
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_M, pn_arm_fpaRdv_M);
		} else if (is_arm_fpaFDiv(new_pred)) {
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_M, pn_arm_fpaFDiv_M);
		} else if (is_arm_fpaFRdv(new_pred)) {
			return new_rd_Proj(dbgi, irg, block, new_pred, mode_M, pn_arm_fpaFRdv_M);
		}
		break;
	case pn_Quot_res:
		if (is_arm_fpaDiv(new_pred)) {
			return new_rd_Proj(dbgi, irg, block, new_pred, mode, pn_arm_fpaDiv_res);
		} else if (is_arm_fpaFDiv(new_pred)) {
			return new_rd_Proj(dbgi, irg, block, new_pred, mode, pn_arm_fpaRdv_res);
		} else if (is_arm_fpaFDiv(new_pred)) {
			return new_rd_Proj(dbgi, irg, block, new_pred, mode, pn_arm_fpaFDiv_res);
		} else if (is_arm_fpaFDiv(new_pred)) {
			return new_rd_Proj(dbgi, irg, block, new_pred, mode, pn_arm_fpaFRdv_res);
		}
		break;
	default:
		break;
	}
	assert(0);
	return new_rd_Unknown(irg, mode);
}

/**
 * Transform the Projs of an AddSP.
 */
static ir_node *gen_Proj_be_AddSP(ir_node *node) {
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *pred     = get_Proj_pred(node);
	ir_node  *new_pred = be_transform_node(pred);
	ir_graph *irg      = current_ir_graph;
	dbg_info *dbgi     = get_irn_dbg_info(node);
	long     proj      = get_Proj_proj(node);

	if (proj == pn_be_AddSP_res) {
		ir_node *res = new_rd_Proj(dbgi, irg, block, new_pred, mode_Iu, pn_arm_AddSP_stack);
		arch_set_irn_register(env_cg->arch_env, res, &arm_gp_regs[REG_SP]);
		return res;
	} else if (proj == pn_be_AddSP_M) {
		return new_rd_Proj(dbgi, irg, block, new_pred, mode_M, pn_arm_AddSP_M);
	}

	assert(0);
	return new_rd_Unknown(irg, get_irn_mode(node));
}

/**
 * Transform the Projs of a SubSP.
 */
static ir_node *gen_Proj_be_SubSP(ir_node *node) {
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *pred     = get_Proj_pred(node);
	ir_node  *new_pred = be_transform_node(pred);
	ir_graph *irg      = current_ir_graph;
	dbg_info *dbgi     = get_irn_dbg_info(node);
	long     proj      = get_Proj_proj(node);

	if (proj == pn_be_SubSP_res) {
		ir_node *res = new_rd_Proj(dbgi, irg, block, new_pred, mode_Iu, pn_arm_SubSP_stack);
		arch_set_irn_register(env_cg->arch_env, res, &arm_gp_regs[REG_SP]);
		return res;
	} else if (proj == pn_be_SubSP_M) {
		return new_rd_Proj(dbgi, irg, block, new_pred, mode_M, pn_arm_SubSP_M);
	}

	assert(0);
	return new_rd_Unknown(irg, get_irn_mode(node));
}

/**
 * Transform the Projs from a Cmp.
 */
static ir_node *gen_Proj_Cmp(ir_node *node) {
	panic("Psi NYI\n");
}


/**
 * Transform the Thread Local Storage Proj.
 */
static ir_node *gen_Proj_tls(ir_node *node) {
	ir_node  *block = be_transform_node(get_nodes_block(node));
	ir_graph *irg   = current_ir_graph;
	dbg_info *dbgi  = NULL;

	return new_rd_arm_LdTls(dbgi, irg, block, mode_Iu);
}

/**
 * Transform a Proj node.
 */
static ir_node *gen_Proj(ir_node *node) {
	ir_graph *irg  = current_ir_graph;
	dbg_info *dbgi = get_irn_dbg_info(node);
	ir_node  *pred = get_Proj_pred(node);
	long     proj  = get_Proj_proj(node);

	if (is_Store(pred) || be_is_FrameStore(pred)) {
		if (proj == pn_Store_M) {
			return be_transform_node(pred);
		} else {
			assert(0);
			return new_r_Bad(irg);
		}
	} else if (is_Load(pred) || be_is_FrameLoad(pred)) {
		return gen_Proj_Load(node);
	} else if (is_CopyB(pred)) {
		return gen_Proj_CopyB(node);
	} else if (is_Quot(pred)) {
		return gen_Proj_Quot(node);
	} else if (be_is_SubSP(pred)) {
		return gen_Proj_be_SubSP(node);
	} else if (be_is_AddSP(pred)) {
		return gen_Proj_be_AddSP(node);
	} else if (is_Cmp(pred)) {
		return gen_Proj_Cmp(node);
	} else if (get_irn_op(pred) == op_Start) {
		if (proj == pn_Start_X_initial_exec) {
			ir_node *block = get_nodes_block(pred);
			ir_node *jump;

			/* we exchange the ProjX with a jump */
			block = be_transform_node(block);
			jump  = new_rd_Jmp(dbgi, irg, block);
			ir_fprintf(stderr, "created jump: %+F\n", jump);
			return jump;
		}
		if (node == be_get_old_anchor(anchor_tls)) {
			return gen_Proj_tls(node);
		}
	} else {
		ir_node *new_pred = be_transform_node(pred);
		ir_mode *mode     = get_irn_mode(node);
		if (mode_needs_gp_reg(mode)) {
			ir_node *block    = be_transform_node(get_nodes_block(node));
			ir_node *new_proj = new_r_Proj(irg, block, new_pred, mode_Iu,
			                               get_Proj_proj(node));
#ifdef DEBUG_libfirm
			new_proj->node_nr = node->node_nr;
#endif
			return new_proj;
		}
	}

	return be_duplicate_node(node);
}

/**
 * This function just sets the register for the Unknown node
 * as this is not done during register allocation because Unknown
 * is an "ignore" node.
 */
static ir_node *gen_Unknown(ir_node *node) {
	ir_mode *mode = get_irn_mode(node);
/*
	if (mode_is_float(mode)) {
		if (USE_FPA(env_cg->isa))
			return arm_new_Unknown_fpa(env_cg);
		else if (USE_VFP(env_cg->isa))
			return arm_new_Unknown_vfp(env_cg);
		else
			panic("Softfloat not supported yet");
	} else if (mode_needs_gp_reg(mode)) {
		return ia32_new_Unknown_gp(env_cg);
	} else {
		assert(0 && "unsupported Unknown-Mode");
	}
*/
	panic("Unknown NYI\n");
	return NULL;
}

/**
 * Change some phi modes
 */
static ir_node *gen_Phi(ir_node *node) {
	ir_node  *block = be_transform_node(get_nodes_block(node));
	ir_graph *irg   = current_ir_graph;
	dbg_info *dbgi  = get_irn_dbg_info(node);
	ir_mode  *mode  = get_irn_mode(node);
	ir_node  *phi;

	if (mode_needs_gp_reg(mode)) {
		/* we shouldn't have any 64bit stuff around anymore */
		assert(get_mode_size_bits(mode) <= 32);
		/* all integer operations are on 32bit registers now */
		mode = mode_Iu;
	}

	/* phi nodes allow loops, so we use the old arguments for now
	 * and fix this later */
	phi = new_ir_node(dbgi, irg, block, op_Phi, mode, get_irn_arity(node), get_irn_in(node) + 1);
	copy_node_attr(node, phi);
	be_duplicate_deps(node, phi);

	be_set_transformed_node(node, phi);
	be_enqueue_preds(node);

	return phi;
}

/*********************************************************
 *                  _             _      _
 *                 (_)           | |    (_)
 *  _ __ ___   __ _ _ _ __     __| |_ __ ___   _____ _ __
 * | '_ ` _ \ / _` | | '_ \   / _` | '__| \ \ / / _ \ '__|
 * | | | | | | (_| | | | | | | (_| | |  | |\ V /  __/ |
 * |_| |_| |_|\__,_|_|_| |_|  \__,_|_|  |_| \_/ \___|_|
 *
 *********************************************************/

/**
 * the BAD transformer.
 */
static ir_node *bad_transform(ir_node *irn) {
	panic("ARM backend: Not implemented: %+F\n", irn);
	return irn;
}

/**
 * Set a node emitter. Make it a bit more type safe.
 */
static INLINE void set_transformer(ir_op *op, be_transform_func arm_transform_func) {
	op->ops.generic = (op_func)arm_transform_func;
}

/**
 * Enters all transform functions into the generic pointer
 */
static void arm_register_transformers(void) {
	ir_op *op_Max, *op_Min, *op_Mulh;

	/* first clear the generic function pointer for all ops */
	clear_irp_opcodes_generic_func();

#define GEN(a)     set_transformer(op_##a, gen_##a)
#define BAD(a)     set_transformer(op_##a, bad_transform)
#define IGN(a)

	GEN(Add);
	GEN(Sub);
	GEN(Mul);
	GEN(And);
	GEN(Or);
	GEN(Eor);

	GEN(Shl);
	GEN(Shr);
	GEN(Shrs);
	BAD(Rot);	/* unsupported yet */

	GEN(Quot);

	/* should be lowered */
	BAD(Div);
	BAD(Mod);
	BAD(DivMod);

	GEN(Minus);
	GEN(Conv);
	GEN(Abs);
	GEN(Not);

	GEN(Load);
	GEN(Store);
	GEN(Cond);

	BAD(ASM);	/* unsupported yet */
	GEN(CopyB);
	BAD(Mux);
	BAD(Psi);	/* unsupported yet */
	GEN(Proj);
	GEN(Phi);

	IGN(Block);
	IGN(End);

	GEN(Const);
	GEN(SymConst);

	/* we should never see these nodes */
	BAD(Raise);
	BAD(Sel);
	BAD(InstOf);
	BAD(Cast);
	BAD(Free);
	BAD(Tuple);
	BAD(Id);
	//BAD(Bad);
	BAD(Confirm);
	BAD(Filter);
	BAD(CallBegin);
	BAD(EndReg);
	BAD(EndExcept);

	/* handle generic backend nodes */
	GEN(be_FrameAddr);
	//GEN(be_Call);
	//GEN(be_Return);
	BAD(be_FrameLoad);	/* unsupported yet */
	BAD(be_FrameStore);	/* unsupported yet */
	BAD(be_StackParam);	/* unsupported yet */
	GEN(be_AddSP);
	GEN(be_SubSP);
	GEN(be_Copy);

	/* set the register for all Unknown nodes */
	GEN(Unknown);

	op_Max = get_op_Max();
	if (op_Max)
		BAD(Max);	/* unsupported yet */
	op_Min = get_op_Min();
	if (op_Min)
		BAD(Min);	/* unsupported yet */
	op_Mulh = get_op_Mulh();
	if (op_Mulh)
		BAD(Mulh);	/* unsupported yet */

#undef IGN
#undef GEN
#undef BAD
}

/**
 * Pre-transform all unknown and noreg nodes.
 */
static void arm_pretransform_node(void *arch_cg) {
	arm_code_gen_t *cg = arch_cg;
}

/**
 * Transform a Firm graph into an ARM graph.
 */
void arm_transform_graph(arm_code_gen_t *cg) {
	arm_register_transformers();
	env_cg = cg;
	be_transform_graph(cg->birg, /*arm_pretransform_node*/ NULL, cg);
}

void arm_init_transform(void) {
	// FIRM_DBG_REGISTER(dbg, "firm.be.arm.transform");
}
