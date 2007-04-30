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
 * @brief       Implements several optimizations for IA32.
 * @author      Christian Wuerdig
 * @version     $Id$
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "irnode.h"
#include "irprog_t.h"
#include "ircons.h"
#include "firm_types.h"
#include "iredges.h"
#include "tv.h"
#include "irgmod.h"
#include "irgwalk.h"
#include "height.h"
#include "irbitset.h"

#include "../be_t.h"
#include "../beabi.h"
#include "../benode_t.h"
#include "../besched_t.h"

#include "ia32_new_nodes.h"
#include "bearch_ia32_t.h"
#include "gen_ia32_regalloc_if.h"     /* the generated interface (register type and class defenitions) */
#include "ia32_transform.h"
#include "ia32_dbg_stat.h"
#include "ia32_util.h"

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

#define AGGRESSIVE_AM

typedef enum {
	IA32_AM_CAND_NONE  = 0,  /**< no addressmode possible with irn inputs */
	IA32_AM_CAND_LEFT  = 1,  /**< addressmode possible with left input */
	IA32_AM_CAND_RIGHT = 2,  /**< addressmode possible with right input */
	IA32_AM_CAND_BOTH  = 3   /**< addressmode possible with both inputs */
} ia32_am_cand_t;

typedef int is_op_func_t(const ir_node *n);
typedef ir_node *load_func_t(dbg_info *db, ir_graph *irg, ir_node *block, ir_node *base, ir_node *index, ir_node *mem);

/**
 * checks if a node represents the NOREG value
 */
static INLINE int be_is_NoReg(ia32_code_gen_t *cg, const ir_node *irn) {
	return irn == cg->noreg_gp || irn == cg->noreg_xmm || irn == cg->noreg_vfp;
}

void ia32_pre_transform_phase(ia32_code_gen_t *cg) {
	/*
		We need to transform the consts twice:
		- the psi condition tree transformer needs existing constants to be ia32 constants
		- the psi condition tree transformer inserts new firm constants which need to be transformed
	*/
	//ia32_transform_all_firm_consts(cg);
	irg_walk_graph(cg->irg, NULL, ia32_transform_psi_cond_tree, cg);
	//ia32_transform_all_firm_consts(cg);
}

/********************************************************************************************************
 *  _____                _           _         ____        _   _           _          _   _
 * |  __ \              | |         | |       / __ \      | | (_)         (_)        | | (_)
 * | |__) |__  ___ _ __ | |__   ___ | | ___  | |  | |_ __ | |_ _ _ __ ___  _ ______ _| |_ _  ___  _ __
 * |  ___/ _ \/ _ \ '_ \| '_ \ / _ \| |/ _ \ | |  | | '_ \| __| | '_ ` _ \| |_  / _` | __| |/ _ \| '_ \
 * | |  |  __/  __/ |_) | | | | (_) | |  __/ | |__| | |_) | |_| | | | | | | |/ / (_| | |_| | (_) | | | |
 * |_|   \___|\___| .__/|_| |_|\___/|_|\___|  \____/| .__/ \__|_|_| |_| |_|_/___\__,_|\__|_|\___/|_| |_|
 *                | |                               | |
 *                |_|                               |_|
 ********************************************************************************************************/

/**
 * NOTE: THESE PEEPHOLE OPTIMIZATIONS MUST BE CALLED AFTER SCHEDULING AND REGISTER ALLOCATION.
 */

static int ia32_const_equal(const ir_node *n1, const ir_node *n2) {
	if(get_ia32_immop_type(n1) != get_ia32_immop_type(n2))
		return 0;

	if(get_ia32_immop_type(n1) == ia32_ImmConst) {
		return get_ia32_Immop_tarval(n1) == get_ia32_Immop_tarval(n2);
	} else if(get_ia32_immop_type(n1) == ia32_ImmSymConst) {
		return get_ia32_Immop_symconst(n1) == get_ia32_Immop_symconst(n2);
	}

	assert(get_ia32_immop_type(n1) == ia32_ImmNone);
	return 1;
}

/**
 * Checks for potential CJmp/CJmpAM optimization candidates.
 */
static ir_node *ia32_determine_cjmp_cand(ir_node *irn, is_op_func_t *is_op_func) {
	ir_node *cand = NULL;
	ir_node *prev = sched_prev(irn);

	if (is_Block(prev)) {
		if (get_Block_n_cfgpreds(prev) == 1)
			prev = get_Block_cfgpred(prev, 0);
		else
			prev = NULL;
	}

	/* The predecessor must be a ProjX. */
	if (prev && is_Proj(prev) && get_irn_mode(prev) == mode_X) {
		prev = get_Proj_pred(prev);

		if (is_op_func(prev))
			cand = prev;
	}

	return cand;
}

static int is_TestJmp_cand(const ir_node *irn) {
	return is_ia32_TestJmp(irn) || is_ia32_And(irn);
}

/**
 * Checks if two consecutive arguments of cand matches
 * the two arguments of irn (TestJmp).
 */
static int is_TestJmp_replacement(ir_node *cand, ir_node *irn) {
	ir_node *in1       = get_irn_n(irn, 0);
	ir_node *in2       = get_irn_n(irn, 1);
	int      i, n      = get_irn_arity(cand);
	int      same_args = 0;

	for (i = 0; i < n - 1; i++) {
		if (get_irn_n(cand, i)     == in1 &&
			get_irn_n(cand, i + 1) == in2)
		{
			same_args = 1;
			break;
		}
	}

	if (!same_args)
		return 0;

	return ia32_const_equal(cand, irn);
}

/**
 * Tries to replace a TestJmp by a CJmp or CJmpAM (in case of And)
 */
static void ia32_optimize_TestJmp(ir_node *irn, ia32_code_gen_t *cg) {
	ir_node *cand    = ia32_determine_cjmp_cand(irn, is_TestJmp_cand);
	int      replace = 0;

	/* we found a possible candidate */
	replace = cand ? is_TestJmp_replacement(cand, irn) : 0;

	if (replace) {
		DBG((dbg, LEVEL_1, "replacing %+F by ", irn));

		if (is_ia32_And(cand))
			set_irn_op(irn, op_ia32_CJmpAM);
		else
			set_irn_op(irn, op_ia32_CJmp);

		DB((dbg, LEVEL_1, "%+F\n", irn));
	}
}

static int is_CondJmp_cand(const ir_node *irn) {
	return is_ia32_CondJmp(irn) || is_ia32_Sub(irn);
}

/**
 * Checks if the arguments of cand are the same of irn.
 */
static int is_CondJmp_replacement(ir_node *cand, ir_node *irn) {
	int i, arity;

	arity = get_irn_arity(cand);
	for (i = 0; i < arity; i++) {
		if (get_irn_n(cand, i) != get_irn_n(irn, i)) {
			return 0;
		}
	}

	return ia32_const_equal(cand, irn);
}

/**
 * Tries to replace a CondJmp by a CJmpAM
 */
static void ia32_optimize_CondJmp(ir_node *irn, ia32_code_gen_t *cg) {
	ir_node *cand    = ia32_determine_cjmp_cand(irn, is_CondJmp_cand);
	int      replace = 0;

	/* we found a possible candidate */
	replace = cand ? is_CondJmp_replacement(cand, irn) : 0;

	if (replace) {
		DBG((dbg, LEVEL_1, "replacing %+F by ", irn));
		DBG_OPT_CJMP(irn);

		set_irn_op(irn, op_ia32_CJmpAM);

		DB((dbg, LEVEL_1, "%+F\n", irn));
	}
}

// only optimize up to 48 stores behind IncSPs
#define MAXPUSH_OPTIMIZE	48

/**
 * Tries to create pushs from IncSP,Store combinations
 */
static void ia32_create_Pushs(ir_node *irn, ia32_code_gen_t *cg) {
	int i;
	int offset;
	ir_node *node;
	ir_node *stores[MAXPUSH_OPTIMIZE];
	ir_node *block = get_nodes_block(irn);
	ir_graph *irg = cg->irg;
	ir_node *curr_sp;
	ir_mode *spmode = get_irn_mode(irn);

	memset(stores, 0, sizeof(stores));

	assert(be_is_IncSP(irn));

	offset = be_get_IncSP_offset(irn);
	if(offset < 4)
		return;

	/*
	 * We first walk the schedule after the IncSP node as long as we find
	 * suitable stores that could be transformed to a push.
	 * We save them into the stores array which is sorted by the frame offset/4
	 * attached to the node
	 */
	for(node = sched_next(irn); !sched_is_end(node); node = sched_next(node)) {
		ir_node *mem;
		int offset;
		int storeslot;

		// it has to be a store
		if(!is_ia32_Store(node))
			break;

		// it has to use our sp value
		if(get_irn_n(node, 0) != irn)
			continue;
		// store has to be attached to NoMem
		mem = get_irn_n(node, 3);
		if(!is_NoMem(mem)) {
			continue;
		}

		if( (get_ia32_am_flavour(node) & ia32_am_IS) != 0)
			break;

		offset = get_ia32_am_offs_int(node);

		storeslot = offset / 4;
		if(storeslot >= MAXPUSH_OPTIMIZE)
			continue;

		// storing into the same slot twice is bad (and shouldn't happen...)
		if(stores[storeslot] != NULL)
			break;

		// storing at half-slots is bad
		if(offset % 4 != 0)
			break;

		stores[storeslot] = node;
	}

	curr_sp = get_irn_n(irn, 0);

	// walk the stores in inverse order and create pushs for them
	i = (offset / 4) - 1;
	if(i >= MAXPUSH_OPTIMIZE) {
		i = MAXPUSH_OPTIMIZE - 1;
	}

	for( ; i >= 0; --i) {
		const arch_register_t *spreg;
		ir_node *push;
		ir_node *val, *mem, *mem_proj;
		ir_node *store = stores[i];
		ir_node *noreg = ia32_new_NoReg_gp(cg);

		if(store == NULL || is_Bad(store))
			break;

		val = get_irn_n(store, 2);
		mem = get_irn_n(store, 3);
		spreg = arch_get_irn_register(cg->arch_env, curr_sp);

		// create a push
		push = new_rd_ia32_Push(NULL, irg, block, noreg, noreg, val, curr_sp, mem);

		set_ia32_am_support(push, ia32_am_Source);
		copy_ia32_Immop_attr(push, store);

		sched_add_before(irn, push);

		// create stackpointer proj
		curr_sp = new_r_Proj(irg, block, push, spmode, pn_ia32_Push_stack);
		arch_set_irn_register(cg->arch_env, curr_sp, spreg);
		sched_add_before(irn, curr_sp);

		// create memory proj
		mem_proj = new_r_Proj(irg, block, push, mode_M, pn_ia32_Push_M);
		sched_add_before(irn, mem_proj);

		// use the memproj now
		exchange(store, mem_proj);

		// we can remove the store now
		sched_remove(store);

		offset -= 4;
	}

	be_set_IncSP_offset(irn, offset);

	// can we remove the IncSP now?
	if(offset == 0) {
		const ir_edge_t *edge, *next;

		foreach_out_edge_safe(irn, edge, next) {
			ir_node *arg = get_edge_src_irn(edge);
			int pos = get_edge_src_pos(edge);

			set_irn_n(arg, pos, curr_sp);
		}

		set_irn_n(irn, 0, new_Bad());
		sched_remove(irn);
	} else {
		set_irn_n(irn, 0, curr_sp);
	}
}

#if 0
/**
 * Tries to optimize two following IncSP.
 */
static void ia32_optimize_IncSP(ir_node *irn, ia32_code_gen_t *cg) {
	ir_node *prev = be_get_IncSP_pred(irn);
	int real_uses = get_irn_n_edges(prev);

	if (be_is_IncSP(prev) && real_uses == 1) {
		/* first IncSP has only one IncSP user, kill the first one */
		int prev_offs = be_get_IncSP_offset(prev);
		int curr_offs = be_get_IncSP_offset(irn);

		be_set_IncSP_offset(prev, prev_offs + curr_offs);

		/* Omit the optimized IncSP */
		be_set_IncSP_pred(irn, be_get_IncSP_pred(prev));

		set_irn_n(prev, 0, new_Bad());
		sched_remove(prev);
	}
}
#endif

/**
 * Performs Peephole Optimizations.
 */
static void ia32_peephole_optimize_node(ir_node *irn, void *env) {
	ia32_code_gen_t *cg = env;

	/* AMD CPUs want explicit compare before conditional jump  */
	if (! ARCH_AMD(cg->opt_arch)) {
		if (is_ia32_TestJmp(irn))
			ia32_optimize_TestJmp(irn, cg);
		else if (is_ia32_CondJmp(irn))
			ia32_optimize_CondJmp(irn, cg);
	}

	if (be_is_IncSP(irn)) {
		// optimize_IncSP doesn't respect dependency edges yet...
		//ia32_optimize_IncSP(irn, cg);

		if (cg->opt & IA32_OPT_PUSHARGS)
			ia32_create_Pushs(irn, cg);
	}
}

void ia32_peephole_optimization(ir_graph *irg, ia32_code_gen_t *cg) {
	irg_walk_graph(irg, ia32_peephole_optimize_node, NULL, cg);
}

/******************************************************************
 *              _     _                   __  __           _
 *     /\      | |   | |                 |  \/  |         | |
 *    /  \   __| | __| |_ __ ___  ___ ___| \  / | ___   __| | ___
 *   / /\ \ / _` |/ _` | '__/ _ \/ __/ __| |\/| |/ _ \ / _` |/ _ \
 *  / ____ \ (_| | (_| | | |  __/\__ \__ \ |  | | (_) | (_| |  __/
 * /_/    \_\__,_|\__,_|_|  \___||___/___/_|  |_|\___/ \__,_|\___|
 *
 ******************************************************************/

typedef struct {
	ia32_code_gen_t *cg;
	heights_t       *h;
} ia32_am_opt_env_t;

static int node_is_ia32_comm(const ir_node *irn) {
	return is_ia32_irn(irn) ? is_ia32_commutative(irn) : 0;
}

static int ia32_get_irn_n_edges(const ir_node *irn) {
	const ir_edge_t *edge;
	int cnt = 0;

	foreach_out_edge(irn, edge) {
		cnt++;
	}

	return cnt;
}

/**
 * Determines if pred is a Proj and if is_op_func returns true for it's predecessor.
 *
 * @param pred       The node to be checked
 * @param is_op_func The check-function
 * @return 1 if conditions are fulfilled, 0 otherwise
 */
static int pred_is_specific_node(const ir_node *pred, is_op_func_t *is_op_func) {
	return is_op_func(pred);
}

/**
 * Determines if pred is a Proj and if is_op_func returns true for it's predecessor
 * and if the predecessor is in block bl.
 *
 * @param bl         The block
 * @param pred       The node to be checked
 * @param is_op_func The check-function
 * @return 1 if conditions are fulfilled, 0 otherwise
 */
static int pred_is_specific_nodeblock(const ir_node *bl, const ir_node *pred,
	int (*is_op_func)(const ir_node *n))
{
	if (is_Proj(pred)) {
		pred = get_Proj_pred(pred);
		if ((bl == get_nodes_block(pred)) && is_op_func(pred)) {
			return 1;
		}
	}

	return 0;
}

/**
 * Checks if irn is a candidate for address calculation. We avoid transforming
 * adds to leas if they have a load as pred, because then we can use AM mode
 * for the add later.
 *
 * - none of the operand must be a Load  within the same block OR
 * - all Loads must have more than one user                    OR
 *
 * @param block   The block the Loads must/mustnot be in
 * @param irn     The irn to check
 * return 1 if irn is a candidate, 0 otherwise
 */
static int is_addr_candidate(const ir_node *irn) {
#ifndef AGGRESSIVE_AM
	const ir_node *block = get_nodes_block(irn);
	ir_node *left, *right;
	int      n;

	left  = get_irn_n(irn, 2);
	right = get_irn_n(irn, 3);

	if (pred_is_specific_nodeblock(block, left, is_ia32_Ld)) {
		n         = ia32_get_irn_n_edges(left);
		/* load with only one user: don't create LEA */
		if(n == 1)
			return 0;
	}

	if (pred_is_specific_nodeblock(block, right, is_ia32_Ld)) {
		n         = ia32_get_irn_n_edges(right);
		if(n == 1)
			return 0;
	}
#endif

	return 1;
}

/**
 * Checks if irn is a candidate for address mode.
 *
 * address mode (AM):
 * - at least one operand has to be a Load within the same block AND
 * - the load must not have other users than the irn             AND
 * - the irn must not have a frame entity set
 *
 * @param cg          The ia32 code generator
 * @param h           The height information of the irg
 * @param block       The block the Loads must/mustnot be in
 * @param irn         The irn to check
 * return 0 if irn is no candidate, 1 if left load can be used, 2 if right one, 3 for both
 */
static ia32_am_cand_t is_am_candidate(ia32_code_gen_t *cg, heights_t *h, const ir_node *block, ir_node *irn) {
	ir_node *in, *load, *other, *left, *right;
	int      is_cand = 0, cand;
	int arity;

	if (is_ia32_Ld(irn) || is_ia32_St(irn) || is_ia32_Store8Bit(irn) || is_ia32_vfild(irn) || is_ia32_vfist(irn) ||
		is_ia32_GetST0(irn) || is_ia32_SetST0(irn) || is_ia32_xStoreSimple(irn))
		return 0;

	left  = get_irn_n(irn, 2);
	arity = get_irn_arity(irn);
	assert(arity == 5 || arity == 4);
	if(arity == 5) {
		/* binary op */
		right = get_irn_n(irn, 3);
	} else {
		/* unary op */
		right = left;
	}

	in = left;

	if (pred_is_specific_nodeblock(block, in, is_ia32_Ld)) {
#ifndef AGGRESSIVE_AM
		int n;
		n         = ia32_get_irn_n_edges(in);
		is_cand   = (n == 1) ? 1 : is_cand;  /* load with more than one user: no AM */
#else
		is_cand   = 1;
#endif

		load  = get_Proj_pred(in);
		other = right;

		/* 8bit Loads are not supported (for binary ops),
		 * they cannot be used with every register */
		if (get_irn_arity(irn) != 4 && get_mode_size_bits(get_ia32_ls_mode(load)) < 16) {
			assert(get_irn_arity(irn) == 5);
			is_cand = 0;
		}

		/* If there is a data dependency of other irn from load: cannot use AM */
		if (is_cand && get_nodes_block(other) == block) {
			other   = skip_Proj(other);
			is_cand = heights_reachable_in_block(h, other, load) ? 0 : is_cand;
			/* this could happen in loops */
			is_cand = heights_reachable_in_block(h, load, irn) ? 0 : is_cand;
		}
	}

	cand    = is_cand ? IA32_AM_CAND_LEFT : IA32_AM_CAND_NONE;
	in      = right;
	is_cand = 0;

	if (pred_is_specific_nodeblock(block, in, is_ia32_Ld)) {
#ifndef AGGRESSIVE_AM
		int n;
		n         = ia32_get_irn_n_edges(in);
		is_cand   = (n == 1) ? 1 : is_cand;  /* load with more than one user: no AM */
#else
		is_cand = 1;
#endif

		load  = get_Proj_pred(in);
		other = left;

		/* 8bit Loads are not supported, they cannot be used with every register */
		if (get_mode_size_bits(get_ia32_ls_mode(load)) < 16)
			is_cand = 0;

		/* If there is a data dependency of other irn from load: cannot use load */
		if (is_cand && get_nodes_block(other) == block) {
			other   = skip_Proj(other);
			is_cand = heights_reachable_in_block(h, other, load) ? 0 : is_cand;
			/* this could happen in loops */
			is_cand = heights_reachable_in_block(h, load, irn) ? 0 : is_cand;
		}
	}

	cand = is_cand ? (cand | IA32_AM_CAND_RIGHT) : cand;

	/* if the irn has a frame entity: we do not use address mode */
	return get_ia32_frame_ent(irn) ? IA32_AM_CAND_NONE : cand;
}

/**
 * Compares the base and index addr and the load/store entities
 * and returns 1 if they are equal.
 */
static int load_store_addr_is_equal(const ir_node *load, const ir_node *store,
									const ir_node *addr_b, const ir_node *addr_i)
{
	if(get_irn_n(load, 0) != addr_b)
		return 0;
	if(get_irn_n(load, 1) != addr_i)
		return 0;

	if(get_ia32_frame_ent(load) != get_ia32_frame_ent(store))
		return 0;

	if(get_ia32_am_sc(load) != get_ia32_am_sc(store))
		return 0;
	if(is_ia32_am_sc_sign(load) != is_ia32_am_sc_sign(store))
		return 0;
	if(get_ia32_am_offs_int(load) != get_ia32_am_offs_int(store))
		return 0;
	if(get_ia32_ls_mode(load) != get_ia32_ls_mode(store))
		return 0;

	return 1;
}

typedef enum _ia32_take_lea_attr {
	IA32_LEA_ATTR_NONE  = 0,
	IA32_LEA_ATTR_BASE  = (1 << 0),
	IA32_LEA_ATTR_INDEX = (1 << 1),
	IA32_LEA_ATTR_OFFS  = (1 << 2),
	IA32_LEA_ATTR_SCALE = (1 << 3),
	IA32_LEA_ATTR_AMSC  = (1 << 4),
	IA32_LEA_ATTR_FENT  = (1 << 5)
} ia32_take_lea_attr;

/**
 * Decides if we have to keep the LEA operand or if we can assimilate it.
 */
static int do_new_lea(ir_node *irn, ir_node *base, ir_node *index, ir_node *lea,
		int have_am_sc, ia32_code_gen_t *cg)
{
	ir_entity *irn_ent  = get_ia32_frame_ent(irn);
	ir_entity *lea_ent  = get_ia32_frame_ent(lea);
	int        ret_val  = 0;
	int        is_noreg_base  = be_is_NoReg(cg, base);
	int        is_noreg_index = be_is_NoReg(cg, index);
	ia32_am_flavour_t am_flav = get_ia32_am_flavour(lea);

	/* If the Add and the LEA both have a different frame entity set: keep */
	if (irn_ent && lea_ent && (irn_ent != lea_ent))
		return IA32_LEA_ATTR_NONE;
	else if (! irn_ent && lea_ent)
		ret_val |= IA32_LEA_ATTR_FENT;

	/* If the Add and the LEA both have already an address mode symconst: keep */
	if (have_am_sc && get_ia32_am_sc(lea))
		return IA32_LEA_ATTR_NONE;
	else if (get_ia32_am_sc(lea))
		ret_val |= IA32_LEA_ATTR_AMSC;

	/* Check the different base-index combinations */

	if (! is_noreg_base && ! is_noreg_index) {
		/* Assimilate if base is the lea and the LEA is just a Base + Offset calculation */
		if ((base == lea) && ! (am_flav & ia32_I ? 1 : 0)) {
			if (am_flav & ia32_O)
				ret_val |= IA32_LEA_ATTR_OFFS;

			ret_val |= IA32_LEA_ATTR_BASE;
		}
		else
			return IA32_LEA_ATTR_NONE;
	}
	else if (! is_noreg_base && is_noreg_index) {
		/* Base is set but index not */
		if (base == lea) {
			/* Base points to LEA: assimilate everything */
			if (am_flav & ia32_O)
				ret_val |= IA32_LEA_ATTR_OFFS;
			if (am_flav & ia32_S)
				ret_val |= IA32_LEA_ATTR_SCALE;
			if (am_flav & ia32_I)
				ret_val |= IA32_LEA_ATTR_INDEX;

			ret_val |= IA32_LEA_ATTR_BASE;
		}
		else if (am_flav & ia32_B ? 0 : 1) {
			/* Base is not the LEA but the LEA is an index only calculation: assimilate */
			if (am_flav & ia32_O)
				ret_val |= IA32_LEA_ATTR_OFFS;
			if (am_flav & ia32_S)
				ret_val |= IA32_LEA_ATTR_SCALE;

			ret_val |= IA32_LEA_ATTR_INDEX;
		}
		else
			return IA32_LEA_ATTR_NONE;
	}
	else if (is_noreg_base && ! is_noreg_index) {
		/* Index is set but not base */
		if (index == lea) {
			/* Index points to LEA: assimilate everything */
			if (am_flav & ia32_O)
				ret_val |= IA32_LEA_ATTR_OFFS;
			if (am_flav & ia32_S)
				ret_val |= IA32_LEA_ATTR_SCALE;
			if (am_flav & ia32_B)
				ret_val |= IA32_LEA_ATTR_BASE;

			ret_val |= IA32_LEA_ATTR_INDEX;
		}
		else if (am_flav & ia32_I ? 0 : 1) {
			/* Index is not the LEA but the LEA is a base only calculation: assimilate */
			if (am_flav & ia32_O)
				ret_val |= IA32_LEA_ATTR_OFFS;
			if (am_flav & ia32_S)
				ret_val |= IA32_LEA_ATTR_SCALE;

			ret_val |= IA32_LEA_ATTR_BASE;
		}
		else
			return IA32_LEA_ATTR_NONE;
	}
	else {
		assert(0 && "There must have been set base or index");
	}

	return ret_val;
}

/**
 * Adds res before irn into schedule if irn was scheduled.
 * @param irn  The schedule point
 * @param res  The node to be scheduled
 */
static INLINE void try_add_to_sched(ir_node *irn, ir_node *res) {
	if (sched_is_scheduled(irn))
		sched_add_before(irn, res);
}

/**
 * Removes node from schedule if it is not used anymore. If irn is a mode_T node
 * all it's Projs are removed as well.
 * @param irn  The irn to be removed from schedule
 */
static INLINE void try_remove_from_sched(ir_node *node) {
	int i, arity;

	if(get_irn_mode(node) == mode_T) {
		const ir_edge_t *edge;
		foreach_out_edge(node, edge) {
			ir_node *proj = get_edge_src_irn(edge);
			try_remove_from_sched(proj);
		}
	}

	if(get_irn_n_edges(node) != 0)
		return;

	if (sched_is_scheduled(node)) {
		sched_remove(node);
	}

	arity = get_irn_arity(node);
	for(i = 0; i < arity; ++i) {
		set_irn_n(node, i, new_Bad());
	}
}

/**
 * Folds Add or Sub to LEA if possible
 */
static ir_node *fold_addr(ia32_code_gen_t *cg, ir_node *irn) {
	ir_graph   *irg        = get_irn_irg(irn);
	dbg_info   *dbg_info   = get_irn_dbg_info(irn);
	ir_node    *block      = get_nodes_block(irn);
	ir_node    *res        = irn;
	ir_node    *shift      = NULL;
	ir_node    *lea_o      = NULL;
	ir_node    *lea        = NULL;
	long        offs       = 0;
	long        offs_cnst  = 0;
	long        offs_lea   = 0;
	int         scale      = 0;
	int         isadd      = 0;
	int         dolea      = 0;
	int         have_am_sc = 0;
	int         am_sc_sign = 0;
	ir_entity  *am_sc      = NULL;
	ir_entity  *lea_ent    = NULL;
	ir_node    *noreg      = ia32_new_NoReg_gp(cg);
	ir_node    *left, *right, *temp;
	ir_node    *base, *index;
	int consumed_left_shift;
	ia32_am_flavour_t am_flav;

	if (is_ia32_Add(irn))
		isadd = 1;

	left  = get_irn_n(irn, 2);
	right = get_irn_n(irn, 3);

	/* "normalize" arguments in case of add with two operands */
	if  (isadd && ! be_is_NoReg(cg, right)) {
		/* put LEA == ia32_am_O as right operand */
		if (is_ia32_Lea(left) && get_ia32_am_flavour(left) == ia32_am_O) {
			set_irn_n(irn, 2, right);
			set_irn_n(irn, 3, left);
			temp  = left;
			left  = right;
			right = temp;
		}

		/* put LEA != ia32_am_O as left operand */
		if (is_ia32_Lea(right) && get_ia32_am_flavour(right) != ia32_am_O) {
			set_irn_n(irn, 2, right);
			set_irn_n(irn, 3, left);
			temp  = left;
			left  = right;
			right = temp;
		}

		/* put SHL as left operand iff left is NOT a LEA */
		if (! is_ia32_Lea(left) && pred_is_specific_node(right, is_ia32_Shl)) {
			set_irn_n(irn, 2, right);
			set_irn_n(irn, 3, left);
			temp  = left;
			left  = right;
			right = temp;
		}
	}

	base    = left;
	index   = noreg;
	offs    = 0;
	scale   = 0;
	am_flav = 0;

	/* check for operation with immediate */
	if (is_ia32_ImmConst(irn)) {
		tarval *tv = get_ia32_Immop_tarval(irn);

		DBG((dbg, LEVEL_1, "\tfound op with imm const"));

		offs_cnst = get_tarval_long(tv);
		dolea     = 1;
	}
	else if (isadd && is_ia32_ImmSymConst(irn)) {
		DBG((dbg, LEVEL_1, "\tfound op with imm symconst"));

		have_am_sc = 1;
		dolea      = 1;
		am_sc      = get_ia32_Immop_symconst(irn);
		am_sc_sign = is_ia32_am_sc_sign(irn);
	}

	/* determine the operand which needs to be checked */
	temp = be_is_NoReg(cg, right) ? left : right;

	/* check if right operand is AMConst (LEA with ia32_am_O)  */
	/* but we can only eat it up if there is no other symconst */
	/* because the linker won't accept two symconsts           */
	if (! have_am_sc && is_ia32_Lea(temp) && get_ia32_am_flavour(temp) == ia32_am_O) {
		DBG((dbg, LEVEL_1, "\tgot op with LEA am_O"));

		offs_lea   = get_ia32_am_offs_int(temp);
		am_sc      = get_ia32_am_sc(temp);
		am_sc_sign = is_ia32_am_sc_sign(temp);
		have_am_sc = 1;
		dolea      = 1;
		lea_o      = temp;

		if (temp == base)
			base = noreg;
		else if (temp == right)
			right = noreg;
	}

	if (isadd) {
		/* default for add -> make right operand to index */
		index               = right;
		dolea               = 1;
		consumed_left_shift = -1;

		DBG((dbg, LEVEL_1, "\tgot LEA candidate with index %+F\n", index));

		/* determine the operand which needs to be checked */
		temp = left;
		if (is_ia32_Lea(left)) {
			temp = right;
			consumed_left_shift = 0;
		}

		/* check for SHL 1,2,3 */
		if (pred_is_specific_node(temp, is_ia32_Shl)) {

			if (is_ia32_ImmConst(temp)) {
				long shiftval = get_tarval_long(get_ia32_Immop_tarval(temp));

				if (shiftval <= 3) {
					index               = get_irn_n(temp, 2);
					consumed_left_shift = consumed_left_shift < 0 ? 1 : 0;
					shift = temp;
					scale = shiftval;

					DBG((dbg, LEVEL_1, "\tgot scaled index %+F\n", index));
				}
			}
		}

		/* fix base */
		if (! be_is_NoReg(cg, index)) {
			/* if we have index, but left == right -> no base */
			if (left == right) {
				base = noreg;
			}
			else if (consumed_left_shift == 1) {
				/* -> base is right operand  */
				base = (right == lea_o) ? noreg : right;
			}
		}
	}

	/* Try to assimilate a LEA as left operand */
	if (is_ia32_Lea(left) && (get_ia32_am_flavour(left) != ia32_am_O)) {
		/* check if we can assimilate the LEA */
		int take_attr = do_new_lea(irn, base, index, left, have_am_sc, cg);

		if (take_attr == IA32_LEA_ATTR_NONE) {
			DBG((dbg, LEVEL_1, "\tleave old LEA, creating new one\n"));
		}
		else {
			DBG((dbg, LEVEL_1, "\tgot LEA as left operand ... assimilating\n"));
			lea = left; /* for statistics */

			if (take_attr & IA32_LEA_ATTR_OFFS)
				offs = get_ia32_am_offs_int(left);

			if (take_attr & IA32_LEA_ATTR_AMSC) {
				am_sc      = get_ia32_am_sc(left);
				have_am_sc = 1;
				am_sc_sign = is_ia32_am_sc_sign(left);
			}

			if (take_attr & IA32_LEA_ATTR_SCALE)
				scale = get_ia32_am_scale(left);

			if (take_attr & IA32_LEA_ATTR_BASE)
				base = get_irn_n(left, 0);

			if (take_attr & IA32_LEA_ATTR_INDEX)
				index = get_irn_n(left, 1);

			if (take_attr & IA32_LEA_ATTR_FENT)
				lea_ent = get_ia32_frame_ent(left);
		}
	}

	/* ok, we can create a new LEA */
	if (dolea) {
		res = new_rd_ia32_Lea(dbg_info, irg, block, base, index);

		/* add the old offset of a previous LEA */
		add_ia32_am_offs_int(res, offs);

		/* add the new offset */
		if (isadd) {
			add_ia32_am_offs_int(res, offs_cnst);
			add_ia32_am_offs_int(res, offs_lea);
		} else {
			/* either lea_O-cnst, -cnst or -lea_O  */
			if (offs_cnst != 0) {
				add_ia32_am_offs_int(res, offs_lea);
				add_ia32_am_offs_int(res, -offs_cnst);
			} else {
				add_ia32_am_offs_int(res, offs_lea);
			}
		}

		/* set the address mode symconst */
		if (have_am_sc) {
			set_ia32_am_sc(res, am_sc);
			if (am_sc_sign)
				set_ia32_am_sc_sign(res);
		}

		/* copy the frame entity (could be set in case of Add */
		/* which was a FrameAddr) */
		if (lea_ent != NULL) {
			set_ia32_frame_ent(res, lea_ent);
			set_ia32_use_frame(res);
		} else {
			set_ia32_frame_ent(res, get_ia32_frame_ent(irn));
			if(is_ia32_use_frame(irn))
				set_ia32_use_frame(res);
		}

		/* set scale */
		set_ia32_am_scale(res, scale);

		am_flav = ia32_am_N;
		/* determine new am flavour */
		if (offs || offs_cnst || offs_lea || have_am_sc) {
			am_flav |= ia32_O;
		}
		if (! be_is_NoReg(cg, base)) {
			am_flav |= ia32_B;
		}
		if (! be_is_NoReg(cg, index)) {
			am_flav |= ia32_I;
		}
		if (scale > 0) {
			am_flav |= ia32_S;
		}
		set_ia32_am_flavour(res, am_flav);

		set_ia32_op_type(res, ia32_AddrModeS);

		SET_IA32_ORIG_NODE(res, ia32_get_old_node_name(cg, irn));

		DBG((dbg, LEVEL_1, "\tLEA [%+F + %+F * %d + %d]\n", base, index, scale, get_ia32_am_offs_int(res)));

		assert(irn && "Couldn't find result proj");

		/* get the result Proj of the Add/Sub */
		try_add_to_sched(irn, res);

		/* exchange the old op with the new LEA */
		try_remove_from_sched(irn);
		exchange(irn, res);

		/* we will exchange it, report here before the Proj is created */
		if (shift && lea && lea_o) {
			try_remove_from_sched(shift);
			try_remove_from_sched(lea);
			try_remove_from_sched(lea_o);
			DBG_OPT_LEA4(irn, lea_o, lea, shift, res);
		} else if (shift && lea) {
			try_remove_from_sched(shift);
			try_remove_from_sched(lea);
			DBG_OPT_LEA3(irn, lea, shift, res);
		} else if (shift && lea_o) {
			try_remove_from_sched(shift);
			try_remove_from_sched(lea_o);
			DBG_OPT_LEA3(irn, lea_o, shift, res);
		} else if (lea && lea_o) {
			try_remove_from_sched(lea);
			try_remove_from_sched(lea_o);
			DBG_OPT_LEA3(irn, lea_o, lea, res);
		} else if (shift) {
			try_remove_from_sched(shift);
			DBG_OPT_LEA2(irn, shift, res);
		} else if (lea) {
			try_remove_from_sched(lea);
			DBG_OPT_LEA2(irn, lea, res);
		} else if (lea_o) {
			try_remove_from_sched(lea_o);
			DBG_OPT_LEA2(irn, lea_o, res);
		} else {
			DBG_OPT_LEA1(irn, res);
		}
	}

	return res;
}


/**
 * Merges a Load/Store node with a LEA.
 * @param irn The Load/Store node
 * @param lea The LEA
 */
static void merge_loadstore_lea(ir_node *irn, ir_node *lea) {
	ir_entity *irn_ent = get_ia32_frame_ent(irn);
	ir_entity *lea_ent = get_ia32_frame_ent(lea);

	/* If the irn and the LEA both have a different frame entity set: do not merge */
	if (irn_ent != NULL && lea_ent != NULL && (irn_ent != lea_ent))
		return;
	else if (irn_ent == NULL && lea_ent != NULL) {
		set_ia32_frame_ent(irn, lea_ent);
		set_ia32_use_frame(irn);
	}

	/* get the AM attributes from the LEA */
	add_ia32_am_offs_int(irn, get_ia32_am_offs_int(lea));
	set_ia32_am_scale(irn, get_ia32_am_scale(lea));
	set_ia32_am_flavour(irn, get_ia32_am_flavour(lea));

	set_ia32_am_sc(irn, get_ia32_am_sc(lea));
	if (is_ia32_am_sc_sign(lea))
		set_ia32_am_sc_sign(irn);

	set_ia32_op_type(irn, is_ia32_Ld(irn) ? ia32_AddrModeS : ia32_AddrModeD);

	/* set base and index */
	set_irn_n(irn, 0, get_irn_n(lea, 0));
	set_irn_n(irn, 1, get_irn_n(lea, 1));

	try_remove_from_sched(lea);

	/* clear remat flag */
	set_ia32_flags(irn, get_ia32_flags(irn) & ~arch_irn_flags_rematerializable);

	if (is_ia32_Ld(irn))
		DBG_OPT_LOAD_LEA(lea, irn);
	else
		DBG_OPT_STORE_LEA(lea, irn);

}

/**
 * Sets new_right index of irn to right and new_left index to left.
 * Also exchange left and right
 */
static void exchange_left_right(ir_node *irn, ir_node **left, ir_node **right, int new_left, int new_right) {
	ir_node *temp;

	set_irn_n(irn, new_right, *right);
	set_irn_n(irn, new_left, *left);

	temp   = *left;
	*left  = *right;
	*right = temp;

	/* this is only needed for Compares, but currently ALL nodes
	 * have this attribute :-) */
	set_ia32_pncode(irn, get_inversed_pnc(get_ia32_pncode(irn)));
}

/**
 * Performs address calculation optimization (create LEAs if possible)
 */
static void optimize_lea(ir_node *irn, void *env) {
	ia32_code_gen_t *cg  = env;

	if (! is_ia32_irn(irn))
		return;

	/* Following cases can occur:                                  */
	/* - Sub (l, imm) -> LEA [base - offset]                       */
	/* - Sub (l, r == LEA with ia32_am_O)   -> LEA [base - offset] */
	/* - Add (l, imm) -> LEA [base + offset]                       */
	/* - Add (l, r == LEA with ia32_am_O)  -> LEA [base + offset]  */
	/* - Add (l == LEA with ia32_am_O, r)  -> LEA [base + offset]  */
	/* - Add (l, r) -> LEA [base + index * scale]                  */
	/*              with scale > 1 iff l/r == shl (1,2,3)          */
	if (is_ia32_Sub(irn) || is_ia32_Add(irn)) {
		ir_node *res;

		if(!is_addr_candidate(irn))
			return;

		DBG((dbg, LEVEL_1, "\tfound address calculation candidate %+F ... ", irn));
		res = fold_addr(cg, irn);

		if (res != irn)
			DB((dbg, LEVEL_1, "transformed into %+F\n", res));
		else
			DB((dbg, LEVEL_1, "not transformed\n"));
	} else if (is_ia32_Ld(irn) || is_ia32_St(irn) || is_ia32_Store8Bit(irn)) {
		/* - Load  -> LEA into Load  } TODO: If the LEA is used by more than one Load/Store */
		/* - Store -> LEA into Store }       it might be better to keep the LEA             */
		ir_node *left = get_irn_n(irn, 0);

		if (is_ia32_Lea(left)) {
			const ir_edge_t *edge, *ne;
			ir_node *src;

			/* merge all Loads/Stores connected to this LEA with the LEA */
			foreach_out_edge_safe(left, edge, ne) {
				src = get_edge_src_irn(edge);

				if (src && (get_edge_src_pos(edge) == 0) && (is_ia32_Ld(src) || is_ia32_St(src) || is_ia32_Store8Bit(src))) {
					DBG((dbg, LEVEL_1, "\nmerging %+F into %+F\n", left, irn));
					if (! is_ia32_got_lea(src))
						merge_loadstore_lea(src, left);
					set_ia32_got_lea(src);
				}
			}
		}
	}
}

/**
 * Checks for address mode patterns and performs the
 * necessary transformations.
 * This function is called by a walker.
 */
static void optimize_am(ir_node *irn, void *env) {
	ia32_am_opt_env_t *am_opt_env = env;
	ia32_code_gen_t   *cg         = am_opt_env->cg;
	ir_graph          *irg        = get_irn_irg(irn);
	heights_t         *h          = am_opt_env->h;
	ir_node           *block, *left, *right;
	ir_node           *store, *load, *mem_proj;
	ir_node           *addr_b, *addr_i;
	int               need_exchange_on_fail = 0;
	ia32_am_type_t    am_support;
	ia32_am_cand_t cand;
	ia32_am_cand_t orig_cand;
	int               dest_possible;
	int               source_possible;

	if (!is_ia32_irn(irn) || is_ia32_Ld(irn) || is_ia32_St(irn) || is_ia32_Store8Bit(irn))
		return;
	if (is_ia32_Lea(irn))
		return;

	am_support = get_ia32_am_support(irn);
	block = get_nodes_block(irn);

	DBG((dbg, LEVEL_1, "checking for AM\n"));

	/* fold following patterns:                                                         */
	/* - op -> Load into AMop with am_Source                                            */
	/*   conditions:                                                                    */
	/*     - op is am_Source capable AND                                                */
	/*     - the Load is only used by this op AND                                       */
	/*     - the Load is in the same block                                              */
	/* - Store -> op -> Load  into AMop with am_Dest                                    */
	/*   conditions:                                                                    */
	/*     - op is am_Dest capable AND                                                  */
	/*     - the Store uses the same address as the Load AND                            */
	/*     - the Load is only used by this op AND                                       */
	/*     - the Load and Store are in the same block AND                               */
	/*     - nobody else uses the result of the op                                      */
	if (get_ia32_am_support(irn) == ia32_am_None)
		return;

	cand = is_am_candidate(cg, h, block, irn);
	if (cand == IA32_AM_CAND_NONE)
		return;

	orig_cand = cand;
	DBG((dbg, LEVEL_1, "\tfound address mode candidate %+F ... ", irn));

	left  = get_irn_n(irn, 2);
	if (get_irn_arity(irn) == 4) {
		/* it's an "unary" operation */
		right = left;
		assert(cand == IA32_AM_CAND_BOTH);
	} else {
		right = get_irn_n(irn, 3);
	}

	dest_possible = am_support & ia32_am_Dest ? 1 : 0;
	source_possible = am_support & ia32_am_Source ? 1 : 0;

	if (dest_possible) {
		addr_b = NULL;
		addr_i = NULL;
		store  = NULL;

		/* we should only have 1 user which is a store */
		if (ia32_get_irn_n_edges(irn) == 1) {
			ir_node *succ = get_edge_src_irn(get_irn_out_edge_first(irn));

			if (is_ia32_xStore(succ) || is_ia32_Store(succ)) {
				store  = succ;
				addr_b = get_irn_n(store, 0);
				addr_i = get_irn_n(store, 1);
			}
		}

		if (store == NULL) {
			dest_possible = 0;
		}
	}

	if (dest_possible) {
		/* normalize nodes, we need the interesting load on the left side */
		if (cand & IA32_AM_CAND_RIGHT) {
			load = get_Proj_pred(right);
			if (load_store_addr_is_equal(load, store, addr_b, addr_i)) {
				exchange_left_right(irn, &left, &right, 3, 2);
				need_exchange_on_fail ^= 1;
				if (cand == IA32_AM_CAND_RIGHT)
					cand = IA32_AM_CAND_LEFT;
			}
		}
	}

	if (dest_possible) {
		if(cand & IA32_AM_CAND_LEFT && is_Proj(left)) {
			load = get_Proj_pred(left);

#ifndef AGGRESSIVE_AM
			/* we have to be the only user of the load */
			if (get_irn_n_edges(left) > 1) {
				dest_possible = 0;
			}
#endif
		} else {
			dest_possible = 0;
		}
	}

	if (dest_possible) {
		/* the store has to use the loads memory or the same memory
		 * as the load */
		ir_node *loadmem = get_irn_n(load, 2);
		ir_node *storemem = get_irn_n(store, 3);
		assert(get_irn_mode(loadmem) == mode_M);
		assert(get_irn_mode(storemem) == mode_M);
		if(storemem != loadmem || !is_Proj(storemem)
				|| get_Proj_pred(storemem) != load) {
			dest_possible = 0;
		}
	}

	if (dest_possible) {
		/* Compare Load and Store address */
		if (!load_store_addr_is_equal(load, store, addr_b, addr_i))
			dest_possible = 0;
	}

	if (dest_possible) {
		/* all conditions fullfilled, do the transformation */
		assert(cand & IA32_AM_CAND_LEFT);

		/* set new base, index and attributes */
		set_irn_n(irn, 0, addr_b);
		set_irn_n(irn, 1, addr_i);
		add_ia32_am_offs_int(irn, get_ia32_am_offs_int(load));
		set_ia32_am_scale(irn, get_ia32_am_scale(load));
		set_ia32_am_flavour(irn, get_ia32_am_flavour(load));
		set_ia32_op_type(irn, ia32_AddrModeD);
		set_ia32_frame_ent(irn, get_ia32_frame_ent(load));
		set_ia32_ls_mode(irn, get_ia32_ls_mode(load));

		set_ia32_am_sc(irn, get_ia32_am_sc(load));
		if (is_ia32_am_sc_sign(load))
			set_ia32_am_sc_sign(irn);

		/* connect to Load memory and disconnect Load */
		if (get_irn_arity(irn) == 5) {
			/* binary AMop */
			set_irn_n(irn, 4, get_irn_n(load, 2));
			set_irn_n(irn, 2, ia32_get_admissible_noreg(cg, irn, 2));
		} else {
			/* unary AMop */
			set_irn_n(irn, 3, get_irn_n(load, 2));
			set_irn_n(irn, 2, ia32_get_admissible_noreg(cg, irn, 2));
		}

		set_irn_mode(irn, mode_M);

		/* connect the memory Proj of the Store to the op */
		mem_proj = ia32_get_proj_for_mode(store, mode_M);
		edges_reroute(mem_proj, irn, irg);

		/* clear remat flag */
		set_ia32_flags(irn, get_ia32_flags(irn) & ~arch_irn_flags_rematerializable);

		try_remove_from_sched(load);
		try_remove_from_sched(store);
		DBG_OPT_AM_D(load, store, irn);

		DB((dbg, LEVEL_1, "merged with %+F and %+F into dest AM\n", load, store));
		need_exchange_on_fail = 0;
		source_possible = 0;
	}

	if (source_possible) {
		/* normalize ops, we need the load on the right */
		if(cand == IA32_AM_CAND_LEFT) {
			if(node_is_ia32_comm(irn)) {
				exchange_left_right(irn, &left, &right, 3, 2);
				need_exchange_on_fail ^= 1;
				cand = IA32_AM_CAND_RIGHT;
			} else {
				source_possible = 0;
			}
		}
	}

	if (source_possible) {
		/* all conditions fullfilled, do transform */
		assert(cand & IA32_AM_CAND_RIGHT);
		load = get_Proj_pred(right);

		if(get_irn_n_edges(load) > 1) {
			source_possible = 0;
		}
	}

	if (source_possible) {
		addr_b = get_irn_n(load, 0);
		addr_i = get_irn_n(load, 1);

		/* set new base, index and attributes */
		set_irn_n(irn, 0, addr_b);
		set_irn_n(irn, 1, addr_i);
		add_ia32_am_offs_int(irn, get_ia32_am_offs_int(load));
		set_ia32_am_scale(irn, get_ia32_am_scale(load));
		set_ia32_am_flavour(irn, get_ia32_am_flavour(load));
		set_ia32_op_type(irn, ia32_AddrModeS);
		set_ia32_frame_ent(irn, get_ia32_frame_ent(load));
		set_ia32_ls_mode(irn, get_ia32_ls_mode(load));

		set_ia32_am_sc(irn, get_ia32_am_sc(load));
		if (is_ia32_am_sc_sign(load))
			set_ia32_am_sc_sign(irn);

		/* clear remat flag */
		set_ia32_flags(irn, get_ia32_flags(irn) & ~arch_irn_flags_rematerializable);

		if (is_ia32_use_frame(load)) {
			if(get_ia32_frame_ent(load) == NULL) {
				set_ia32_need_stackent(irn);
			}
			set_ia32_use_frame(irn);
		}

		/* connect to Load memory and disconnect Load */
		if (get_irn_arity(irn) == 5) {
			/* binary AMop */
			set_irn_n(irn, 3, ia32_get_admissible_noreg(cg, irn, 3));
			set_irn_n(irn, 4, get_irn_n(load, 2));
		} else {
			assert(get_irn_arity(irn) == 4);
			/* unary AMop */
			set_irn_n(irn, 2, ia32_get_admissible_noreg(cg, irn, 2));
			set_irn_n(irn, 3, get_irn_n(load, 2));
		}

		DBG_OPT_AM_S(load, irn);

		/* If Load has a memory Proj, connect it to the op */
		mem_proj = ia32_get_proj_for_mode(load, mode_M);
		if (mem_proj != NULL) {
			ir_node *res_proj;
			ir_mode *mode = get_irn_mode(irn);

			res_proj = new_rd_Proj(get_irn_dbg_info(irn), irg,
			                       get_nodes_block(irn), new_Unknown(mode_T),
								   mode, 0);
			set_irn_mode(irn, mode_T);
			edges_reroute(irn, res_proj, irg);
			set_Proj_pred(res_proj, irn);

			set_Proj_pred(mem_proj, irn);
			set_Proj_proj(mem_proj, 1);

			if(sched_is_scheduled(irn)) {
				sched_add_after(irn, res_proj);
				sched_add_after(irn, mem_proj);
			}
		}

		if(get_irn_n_edges(load) == 0) {
			try_remove_from_sched(load);
		}
		need_exchange_on_fail = 0;

		DB((dbg, LEVEL_1, "merged with %+F into source AM\n", load));
	}

	/* was exchanged but optimize failed: exchange back */
	if (need_exchange_on_fail) {
		exchange_left_right(irn, &left, &right, 3, 2);
	}
}

/**
 * Performs address mode optimization.
 */
void ia32_optimize_addressmode(ia32_code_gen_t *cg) {
	/* if we are supposed to do AM or LEA optimization: recalculate edges */
	if (cg->opt & (IA32_OPT_DOAM | IA32_OPT_LEA)) {
#if 0
		edges_deactivate(cg->irg);
		edges_activate(cg->irg);
#endif
	}
	else {
		/* no optimizations at all */
		return;
	}

	/* beware: we cannot optimize LEA and AM in one run because */
	/*         LEA optimization adds new nodes to the irg which */
	/*         invalidates the phase data                       */

	if (cg->opt & IA32_OPT_LEA) {
		irg_walk_blkwise_graph(cg->irg, NULL, optimize_lea, cg);
	}

	if (cg->dump)
		be_dump(cg->irg, "-lea", dump_ir_block_graph_sched);

	/* hack for now, so these don't get created during optimize, because then
	 * they will be unknown to the heights module
	 */
	ia32_new_NoReg_gp(cg);
	ia32_new_NoReg_fp(cg);
	ia32_new_NoReg_vfp(cg);

	if (cg->opt & IA32_OPT_DOAM) {
		/* we need height information for am optimization */
		heights_t *h = heights_new(cg->irg);
		ia32_am_opt_env_t env;

		env.cg = cg;
		env.h  = h;

		irg_walk_blkwise_graph(cg->irg, NULL, optimize_am, &env);

		heights_free(h);
	}
}

void ia32_init_optimize(void)
{
	FIRM_DBG_REGISTER(dbg, "firm.be.ia32.optimize");
}
