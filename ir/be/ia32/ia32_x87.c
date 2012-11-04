/*
 * Copyright (C) 1995-2010 University of Karlsruhe.  All right reserved.
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
 * @brief       This file implements the x87 support and virtual to stack
 *              register translation for the ia32 backend.
 * @author      Michael Beck
 */
#include "config.h"

#include <assert.h>

#include "irnode_t.h"
#include "irop_t.h"
#include "irprog.h"
#include "iredges_t.h"
#include "irgmod.h"
#include "ircons.h"
#include "irgwalk.h"
#include "obst.h"
#include "pmap.h"
#include "array_t.h"
#include "pdeq.h"
#include "irprintf.h"
#include "debug.h"
#include "error.h"

#include "belive_t.h"
#include "besched.h"
#include "benode.h"
#include "bearch_ia32_t.h"
#include "ia32_new_nodes.h"
#include "gen_ia32_new_nodes.h"
#include "gen_ia32_regalloc_if.h"
#include "ia32_x87.h"
#include "ia32_architecture.h"

/** the debug handle */
DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

/* Forward declaration. */
typedef struct x87_simulator x87_simulator;

/**
 * An exchange template.
 * Note that our virtual functions have the same inputs
 * and attributes as the real ones, so we can simple exchange
 * their opcodes!
 * Further, x87 supports inverse instructions, so we can handle them.
 */
typedef struct exchange_tmpl {
	ir_op *normal_op;       /**< the normal one */
	ir_op *reverse_op;      /**< the reverse one if exists */
	ir_op *normal_pop_op;   /**< the normal one with tos pop */
	ir_op *reverse_pop_op;  /**< the reverse one with tos pop */
} exchange_tmpl;

/**
 * An entry on the simulated x87 stack.
 */
typedef struct st_entry {
	int     reg_idx;        /**< the virtual register index of this stack value */
	ir_node *node;          /**< the node that produced this value */
} st_entry;

/**
 * The x87 state.
 */
typedef struct x87_state {
	st_entry st[N_ia32_st_REGS]; /**< the register stack */
	int depth;                   /**< the current stack depth */
	x87_simulator *sim;          /**< The simulator. */
} x87_state;

/** An empty state, used for blocks without fp instructions. */
static x87_state empty = { { {0, NULL}, }, 0, NULL };

/**
 * Return values of the instruction simulator functions.
 */
enum {
	NO_NODE_ADDED = 0,  /**< No node that needs simulation was added. */
	NODE_ADDED    = 1   /**< A node that must be simulated was added by the simulator
	                         in the schedule AFTER the current node. */
};

/**
 * The type of an instruction simulator function.
 *
 * @param state  the x87 state
 * @param n      the node to be simulated
 *
 * @return NODE_ADDED    if a node was added AFTER n in schedule that MUST be
 *                       simulated further
 *         NO_NODE_ADDED otherwise
 */
typedef int (*sim_func)(x87_state *state, ir_node *n);

/**
 * A block state: Every block has a x87 state at the beginning and at the end.
 */
typedef struct blk_state {
	x87_state *begin;   /**< state at the begin or NULL if not assigned */
	x87_state *end;     /**< state at the end or NULL if not assigned */
} blk_state;

/** liveness bitset for vfp registers. */
typedef unsigned char vfp_liveness;

/**
 * The x87 simulator.
 */
struct x87_simulator {
	struct obstack obst;        /**< An obstack for fast allocating. */
	pmap *blk_states;           /**< Map blocks to states. */
	be_lv_t *lv;                /**< intrablock liveness. */
	vfp_liveness *live;         /**< Liveness information. */
	unsigned n_idx;             /**< The cached get_irg_last_idx() result. */
	waitq *worklist;            /**< Worklist of blocks that must be processed. */
};

/**
 * Returns the current stack depth.
 *
 * @param state  the x87 state
 *
 * @return the x87 stack depth
 */
static int x87_get_depth(const x87_state *state)
{
	return state->depth;
}

static st_entry *x87_get_entry(x87_state *const state, int const pos)
{
	assert(0 <= pos && pos < state->depth);
	return &state->st[N_ia32_st_REGS - state->depth + pos];
}

/**
 * Return the virtual register index at st(pos).
 *
 * @param state  the x87 state
 * @param pos    a stack position
 *
 * @return the vfp register index that produced the value at st(pos)
 */
static int x87_get_st_reg(const x87_state *state, int pos)
{
	return x87_get_entry((x87_state*)state, pos)->reg_idx;
}

#ifdef DEBUG_libfirm
/**
 * Dump the stack for debugging.
 *
 * @param state  the x87 state
 */
static void x87_dump_stack(const x87_state *state)
{
	for (int i = state->depth; i-- != 0;) {
		st_entry const *const entry = x87_get_entry((x87_state*)state, i);
		DB((dbg, LEVEL_2, "vf%d(%+F) ", entry->reg_idx, entry->node));
	}
	DB((dbg, LEVEL_2, "<-- TOS\n"));
}
#endif /* DEBUG_libfirm */

/**
 * Set a virtual register to st(pos).
 *
 * @param state    the x87 state
 * @param reg_idx  the vfp register index that should be set
 * @param node     the IR node that produces the value of the vfp register
 * @param pos      the stack position where the new value should be entered
 */
static void x87_set_st(x87_state *state, int reg_idx, ir_node *node, int pos)
{
	st_entry *const entry = x87_get_entry(state, pos);
	entry->reg_idx = reg_idx;
	entry->node    = node;

	DB((dbg, LEVEL_2, "After SET_REG: "));
	DEBUG_ONLY(x87_dump_stack(state);)
}

/**
 * Set the tos virtual register.
 *
 * @param state    the x87 state
 * @param reg_idx  the vfp register index that should be set
 * @param node     the IR node that produces the value of the vfp register
 */
static void x87_set_tos(x87_state *state, int reg_idx, ir_node *node)
{
	x87_set_st(state, reg_idx, node, 0);
}

/**
 * Swap st(0) with st(pos).
 *
 * @param state    the x87 state
 * @param pos      the stack position to change the tos with
 */
static void x87_fxch(x87_state *state, int pos)
{
	st_entry *const a = x87_get_entry(state, pos);
	st_entry *const b = x87_get_entry(state, 0);
	st_entry  const t = *a;
	*a = *b;
	*b = t;

	DB((dbg, LEVEL_2, "After FXCH: "));
	DEBUG_ONLY(x87_dump_stack(state);)
}

/**
 * Convert a virtual register to the stack index.
 *
 * @param state    the x87 state
 * @param reg_idx  the register vfp index
 *
 * @return the stack position where the register is stacked
 *         or -1 if the virtual register was not found
 */
static int x87_on_stack(const x87_state *state, int reg_idx)
{
	for (int i = 0; i < state->depth; ++i) {
		if (x87_get_st_reg(state, i) == reg_idx)
			return i;
	}
	return -1;
}

/**
 * Push a virtual Register onto the stack, double pushed allowed.
 *
 * @param state     the x87 state
 * @param reg_idx   the register vfp index
 * @param node      the node that produces the value of the vfp register
 */
static void x87_push_dbl(x87_state *state, int reg_idx, ir_node *node)
{
	assert(state->depth < N_ia32_st_REGS && "stack overrun");

	++state->depth;
	st_entry *const entry = x87_get_entry(state, 0);
	entry->reg_idx = reg_idx;
	entry->node    = node;

	DB((dbg, LEVEL_2, "After PUSH: ")); DEBUG_ONLY(x87_dump_stack(state);)
}

/**
 * Push a virtual Register onto the stack, double pushes are NOT allowed.
 *
 * @param state     the x87 state
 * @param reg_idx   the register vfp index
 * @param node      the node that produces the value of the vfp register
 */
static void x87_push(x87_state *state, int reg_idx, ir_node *node)
{
	assert(x87_on_stack(state, reg_idx) == -1 && "double push");

	x87_push_dbl(state, reg_idx, node);
}

/**
 * Pop a virtual Register from the stack.
 *
 * @param state     the x87 state
 */
static void x87_pop(x87_state *state)
{
	assert(state->depth > 0 && "stack underrun");

	--state->depth;

	DB((dbg, LEVEL_2, "After POP: ")); DEBUG_ONLY(x87_dump_stack(state);)
}

/**
 * Empty the fpu stack
 *
 * @param state     the x87 state
 */
static void x87_emms(x87_state *state)
{
	state->depth = 0;
}

/**
 * Returns the block state of a block.
 *
 * @param sim    the x87 simulator handle
 * @param block  the current block
 *
 * @return the block state
 */
static blk_state *x87_get_bl_state(x87_simulator *sim, ir_node *block)
{
	blk_state *res = pmap_get(blk_state, sim->blk_states, block);

	if (res == NULL) {
		res = OALLOC(&sim->obst, blk_state);
		res->begin = NULL;
		res->end   = NULL;

		pmap_insert(sim->blk_states, block, res);
	}

	return res;
}

/**
 * Clone a x87 state.
 *
 * @param sim    the x87 simulator handle
 * @param src    the x87 state that will be cloned
 *
 * @return a cloned copy of the src state
 */
static x87_state *x87_clone_state(x87_simulator *sim, const x87_state *src)
{
	x87_state *const res = OALLOC(&sim->obst, x87_state);
	*res = *src;
	return res;
}

/**
 * Patch a virtual instruction into a x87 one and return
 * the node representing the result value.
 *
 * @param n   the IR node to patch
 * @param op  the x87 opcode to patch in
 */
static ir_node *x87_patch_insn(ir_node *n, ir_op *op)
{
	ir_mode *mode = get_irn_mode(n);
	ir_node *res = n;

	set_irn_op(n, op);

	if (mode == mode_T) {
		/* patch all Proj's */
		foreach_out_edge(n, edge) {
			ir_node *proj = get_edge_src_irn(edge);
			if (is_Proj(proj)) {
				mode = get_irn_mode(proj);
				if (mode_is_float(mode)) {
					res = proj;
					set_irn_mode(proj, ia32_reg_classes[CLASS_ia32_st].mode);
				}
			}
		}
	} else if (mode_is_float(mode))
		set_irn_mode(n, ia32_reg_classes[CLASS_ia32_st].mode);
	return res;
}

/**
 * Returns the first Proj of a mode_T node having a given mode.
 *
 * @param n  the mode_T node
 * @param m  the desired mode of the Proj
 * @return The first Proj of mode @p m found or NULL.
 */
static ir_node *get_irn_Proj_for_mode(ir_node *n, ir_mode *m)
{
	assert(get_irn_mode(n) == mode_T && "Need mode_T node");

	foreach_out_edge(n, edge) {
		ir_node *proj = get_edge_src_irn(edge);
		if (get_irn_mode(proj) == m)
			return proj;
	}

	return NULL;
}

/**
 * Wrap the arch_* function here so we can check for errors.
 */
static inline const arch_register_t *x87_get_irn_register(const ir_node *irn)
{
	const arch_register_t *res = arch_get_irn_register(irn);

	assert(res->reg_class == &ia32_reg_classes[CLASS_ia32_vfp]);
	return res;
}

static inline const arch_register_t *x87_irn_get_register(const ir_node *irn,
                                                          int pos)
{
	const arch_register_t *res = arch_get_irn_register_out(irn, pos);

	assert(res->reg_class == &ia32_reg_classes[CLASS_ia32_vfp]);
	return res;
}

static inline const arch_register_t *get_st_reg(int index)
{
	return &ia32_registers[REG_ST0 + index];
}

/* -------------- x87 perm --------------- */

/**
 * Creates a fxch for shuffle.
 *
 * @param state     the x87 state
 * @param pos       parameter for fxch
 * @param block     the block were fxch is inserted
 *
 * Creates a new fxch node and reroute the user of the old node
 * to the fxch.
 *
 * @return the fxch node
 */
static ir_node *x87_fxch_shuffle(x87_state *state, int pos, ir_node *block)
{
	ir_node         *fxch;
	ia32_x87_attr_t *attr;

	fxch = new_bd_ia32_fxch(NULL, block);
	attr = get_ia32_x87_attr(fxch);
	attr->x87[0] = get_st_reg(pos);
	attr->x87[2] = get_st_reg(0);

	keep_alive(fxch);

	x87_fxch(state, pos);
	return fxch;
}

/**
 * Calculate the necessary permutations to reach dst_state.
 *
 * These permutations are done with fxch instructions and placed
 * at the end of the block.
 *
 * Note that critical edges are removed here, so we need only
 * a shuffle if the current block has only one successor.
 *
 * @param block      the current block
 * @param state      the current x87 stack state, might be modified
 * @param dst_state  destination state
 *
 * @return state
 */
static x87_state *x87_shuffle(ir_node *block, x87_state *state, const x87_state *dst_state)
{
	int      i, n_cycles, k, ri;
	unsigned cycles[4], all_mask;
	char     cycle_idx[4][8];
	ir_node  *fxch, *before, *after;

	assert(state->depth == dst_state->depth);

	/* Some mathematics here:
	   If we have a cycle of length n that includes the tos,
	   we need n-1 exchange operations.
	   We can always add the tos and restore it, so we need
	   n+1 exchange operations for a cycle not containing the tos.
	   So, the maximum of needed operations is for a cycle of 7
	   not including the tos == 8.
	   This is the same number of ops we would need for using stores,
	   so exchange is cheaper (we save the loads).
	   On the other hand, we might need an additional exchange
	   in the next block to bring one operand on top, so the
	   number of ops in the first case is identical.
	   Further, no more than 4 cycles can exists (4 x 2).
	*/
	all_mask = (1 << (state->depth)) - 1;

	for (n_cycles = 0; all_mask; ++n_cycles) {
		int src_idx, dst_idx;

		/* find the first free slot */
		for (i = 0; i < state->depth; ++i) {
			if (all_mask & (1 << i)) {
				all_mask &= ~(1 << i);

				/* check if there are differences here */
				if (x87_get_st_reg(state, i) != x87_get_st_reg(dst_state, i))
					break;
			}
		}

		if (! all_mask) {
			/* no more cycles found */
			break;
		}

		k = 0;
		cycles[n_cycles] = (1 << i);
		cycle_idx[n_cycles][k++] = i;
		for (src_idx = i; ; src_idx = dst_idx) {
			dst_idx = x87_on_stack(dst_state, x87_get_st_reg(state, src_idx));

			if ((all_mask & (1 << dst_idx)) == 0)
				break;

			cycle_idx[n_cycles][k++] = dst_idx;
			cycles[n_cycles] |=  (1 << dst_idx);
			all_mask       &= ~(1 << dst_idx);
		}
		cycle_idx[n_cycles][k] = -1;
	}

	if (n_cycles <= 0) {
		/* no permutation needed */
		return state;
	}

	/* Hmm: permutation needed */
	DB((dbg, LEVEL_2, "\n%+F needs permutation: from\n", block));
	DEBUG_ONLY(x87_dump_stack(state);)
	DB((dbg, LEVEL_2, "                  to\n"));
	DEBUG_ONLY(x87_dump_stack(dst_state);)


#ifdef DEBUG_libfirm
	DB((dbg, LEVEL_2, "Need %d cycles\n", n_cycles));
	for (ri = 0; ri < n_cycles; ++ri) {
		DB((dbg, LEVEL_2, " Ring %d:\n ", ri));
		for (k = 0; cycle_idx[ri][k] != -1; ++k)
			DB((dbg, LEVEL_2, " st%d ->", cycle_idx[ri][k]));
		DB((dbg, LEVEL_2, "\n"));
	}
#endif

	after = NULL;

	/*
	 * Find the place node must be insert.
	 * We have only one successor block, so the last instruction should
	 * be a jump.
	 */
	before = sched_last(block);
	assert(is_cfop(before));

	/* now do the permutations */
	for (ri = 0; ri < n_cycles; ++ri) {
		if ((cycles[ri] & 1) == 0) {
			/* this cycle does not include the tos */
			fxch = x87_fxch_shuffle(state, cycle_idx[ri][0], block);
			if (after)
				sched_add_after(after, fxch);
			else
				sched_add_before(before, fxch);
			after = fxch;
		}
		for (k = 1; cycle_idx[ri][k] != -1; ++k) {
			fxch = x87_fxch_shuffle(state, cycle_idx[ri][k], block);
			if (after)
				sched_add_after(after, fxch);
			else
				sched_add_before(before, fxch);
			after = fxch;
		}
		if ((cycles[ri] & 1) == 0) {
			/* this cycle does not include the tos */
			fxch = x87_fxch_shuffle(state, cycle_idx[ri][0], block);
			sched_add_after(after, fxch);
		}
	}
	return state;
}

/**
 * Create a fxch node before another node.
 *
 * @param state   the x87 state
 * @param n       the node after the fxch
 * @param pos     exchange st(pos) with st(0)
 *
 * @return the fxch
 */
static ir_node *x87_create_fxch(x87_state *state, ir_node *n, int pos)
{
	ir_node         *fxch;
	ia32_x87_attr_t *attr;
	ir_node         *block = get_nodes_block(n);

	x87_fxch(state, pos);

	fxch = new_bd_ia32_fxch(NULL, block);
	attr = get_ia32_x87_attr(fxch);
	attr->x87[0] = get_st_reg(pos);
	attr->x87[2] = get_st_reg(0);

	keep_alive(fxch);

	sched_add_before(n, fxch);
	DB((dbg, LEVEL_1, "<<< %s %s, %s\n", get_irn_opname(fxch), attr->x87[0]->name, attr->x87[2]->name));
	return fxch;
}

/**
 * Create a fpush before node n.
 *
 * @param state     the x87 state
 * @param n         the node after the fpush
 * @param pos       push st(pos) on stack
 * @param op_idx    replace input op_idx of n with the fpush result
 */
static void x87_create_fpush(x87_state *state, ir_node *n, int pos, int op_idx)
{
	ir_node               *fpush, *pred = get_irn_n(n, op_idx);
	ia32_x87_attr_t       *attr;
	const arch_register_t *out = x87_get_irn_register(pred);

	x87_push_dbl(state, arch_register_get_index(out), pred);

	fpush = new_bd_ia32_fpush(NULL, get_nodes_block(n));
	attr  = get_ia32_x87_attr(fpush);
	attr->x87[0] = get_st_reg(pos);
	attr->x87[2] = get_st_reg(0);

	keep_alive(fpush);
	sched_add_before(n, fpush);

	DB((dbg, LEVEL_1, "<<< %s %s, %s\n", get_irn_opname(fpush), attr->x87[0]->name, attr->x87[2]->name));
}

/**
 * Create a fpop before node n.
 *
 * @param state   the x87 state
 * @param n       the node after the fpop
 * @param num     pop 1 or 2 values
 *
 * @return the fpop node
 */
static ir_node *x87_create_fpop(x87_state *state, ir_node *n, int num)
{
	ir_node         *fpop = NULL;
	ia32_x87_attr_t *attr;

	assert(num > 0);
	do {
		x87_pop(state);
		if (ia32_cg_config.use_ffreep)
			fpop = new_bd_ia32_ffreep(NULL, get_nodes_block(n));
		else
			fpop = new_bd_ia32_fpop(NULL, get_nodes_block(n));
		attr = get_ia32_x87_attr(fpop);
		attr->x87[0] = get_st_reg(0);
		attr->x87[1] = get_st_reg(0);
		attr->x87[2] = get_st_reg(0);

		keep_alive(fpop);
		sched_add_before(n, fpop);
		DB((dbg, LEVEL_1, "<<< %s %s\n", get_irn_opname(fpop), attr->x87[0]->name));
	} while (--num > 0);
	return fpop;
}

/* --------------------------------- liveness ------------------------------------------ */

/**
 * The liveness transfer function.
 * Updates a live set over a single step from a given node to its predecessor.
 * Everything defined at the node is removed from the set, the uses of the node get inserted.
 *
 * @param irn      The node at which liveness should be computed.
 * @param live     The bitset of registers live before @p irn. This set gets modified by updating it to
 *                 the registers live after irn.
 *
 * @return The live bitset.
 */
static vfp_liveness vfp_liveness_transfer(ir_node *irn, vfp_liveness live)
{
	int i, n;
	const arch_register_class_t *cls = &ia32_reg_classes[CLASS_ia32_vfp];

	if (get_irn_mode(irn) == mode_T) {
		foreach_out_edge(irn, edge) {
			ir_node *proj = get_edge_src_irn(edge);

			if (arch_irn_consider_in_reg_alloc(cls, proj)) {
				const arch_register_t *reg = x87_get_irn_register(proj);
				live &= ~(1 << arch_register_get_index(reg));
			}
		}
	} else if (arch_irn_consider_in_reg_alloc(cls, irn)) {
		const arch_register_t *reg = x87_get_irn_register(irn);
		live &= ~(1 << arch_register_get_index(reg));
	}

	for (i = 0, n = get_irn_arity(irn); i < n; ++i) {
		ir_node *op = get_irn_n(irn, i);

		if (mode_is_float(get_irn_mode(op)) &&
				arch_irn_consider_in_reg_alloc(cls, op)) {
			const arch_register_t *reg = x87_get_irn_register(op);
			live |= 1 << arch_register_get_index(reg);
		}
	}
	return live;
}

/**
 * Put all live virtual registers at the end of a block into a bitset.
 *
 * @param sim      the simulator handle
 * @param bl       the block
 *
 * @return The live bitset at the end of this block
 */
static vfp_liveness vfp_liveness_end_of_block(x87_simulator *sim, const ir_node *block)
{
	vfp_liveness live = 0;
	const arch_register_class_t *cls = &ia32_reg_classes[CLASS_ia32_vfp];
	const be_lv_t *lv = sim->lv;

	be_lv_foreach(lv, block, be_lv_state_end, node) {
		const arch_register_t *reg;
		if (!arch_irn_consider_in_reg_alloc(cls, node))
			continue;

		reg = x87_get_irn_register(node);
		live |= 1 << arch_register_get_index(reg);
	}

	return live;
}

/** get the register mask from an arch_register */
#define REGMASK(reg)    (1 << (arch_register_get_index(reg)))

/**
 * Return a bitset of argument registers which are live at the end of a node.
 *
 * @param sim    the simulator handle
 * @param pos    the node
 * @param kill   kill mask for the output registers
 *
 * @return The live bitset.
 */
static unsigned vfp_live_args_after(x87_simulator *sim, const ir_node *pos, unsigned kill)
{
	unsigned idx = get_irn_idx(pos);

	assert(idx < sim->n_idx);
	return sim->live[idx] & ~kill;
}

/**
 * Calculate the liveness for a whole block and cache it.
 *
 * @param sim   the simulator handle
 * @param block the block
 */
static void update_liveness(x87_simulator *sim, ir_node *block)
{
	vfp_liveness live = vfp_liveness_end_of_block(sim, block);
	unsigned idx;

	/* now iterate through the block backward and cache the results */
	sched_foreach_reverse(block, irn) {
		/* stop at the first Phi: this produces the live-in */
		if (is_Phi(irn))
			break;

		idx = get_irn_idx(irn);
		sim->live[idx] = live;

		live = vfp_liveness_transfer(irn, live);
	}
	idx = get_irn_idx(block);
	sim->live[idx] = live;
}

/**
 * Returns true if a register is live in a set.
 *
 * @param reg_idx  the vfp register index
 * @param live     a live bitset
 */
#define is_vfp_live(reg_idx, live) ((live) & (1 << (reg_idx)))

#ifdef DEBUG_libfirm
/**
 * Dump liveness info.
 *
 * @param live  the live bitset
 */
static void vfp_dump_live(vfp_liveness live)
{
	int i;

	DB((dbg, LEVEL_2, "Live after: "));
	for (i = 0; i < 8; ++i) {
		if (live & (1 << i)) {
			DB((dbg, LEVEL_2, "vf%d ", i));
		}
	}
	DB((dbg, LEVEL_2, "\n"));
}
#endif /* DEBUG_libfirm */

/* --------------------------------- simulators ---------------------------------------- */

/**
 * Simulate a virtual binop.
 *
 * @param state  the x87 state
 * @param n      the node that should be simulated (and patched)
 * @param tmpl   the template containing the 4 possible x87 opcodes
 *
 * @return NO_NODE_ADDED
 */
static int sim_binop(x87_state *state, ir_node *n, const exchange_tmpl *tmpl)
{
	int op2_idx = 0, op1_idx;
	int out_idx, do_pop = 0;
	ia32_x87_attr_t *attr;
	int permuted;
	ir_node *patched_insn;
	ir_op *dst;
	x87_simulator         *sim     = state->sim;
	ir_node               *op1     = get_irn_n(n, n_ia32_binary_left);
	ir_node               *op2     = get_irn_n(n, n_ia32_binary_right);
	const arch_register_t *op1_reg = x87_get_irn_register(op1);
	const arch_register_t *op2_reg = x87_get_irn_register(op2);
	const arch_register_t *out     = x87_irn_get_register(n, pn_ia32_res);
	int reg_index_1                = arch_register_get_index(op1_reg);
	int reg_index_2                = arch_register_get_index(op2_reg);
	vfp_liveness           live    = vfp_live_args_after(sim, n, REGMASK(out));
	int                    op1_live_after;
	int                    op2_live_after;

	DB((dbg, LEVEL_1, ">>> %+F %s, %s -> %s\n", n,
		arch_register_get_name(op1_reg), arch_register_get_name(op2_reg),
		arch_register_get_name(out)));
	DEBUG_ONLY(vfp_dump_live(live);)
	DB((dbg, LEVEL_1, "Stack before: "));
	DEBUG_ONLY(x87_dump_stack(state);)

	op1_idx = x87_on_stack(state, reg_index_1);
	assert(op1_idx >= 0);
	op1_live_after = is_vfp_live(reg_index_1, live);

	attr     = get_ia32_x87_attr(n);
	permuted = attr->attr.data.ins_permuted;

	if (reg_index_2 != REG_VFP_VFP_NOREG) {
		assert(!permuted);

		/* second operand is a vfp register */
		op2_idx = x87_on_stack(state, reg_index_2);
		assert(op2_idx >= 0);
		op2_live_after = is_vfp_live(reg_index_2, live);

		if (op2_live_after) {
			/* Second operand is live. */

			if (op1_live_after) {
				/* Both operands are live: push the first one.
				   This works even for op1 == op2. */
				x87_create_fpush(state, n, op1_idx, n_ia32_binary_right);
				/* now do fxxx (tos=tos X op) */
				op1_idx = 0;
				op2_idx += 1;
				out_idx = 0;
				dst = tmpl->normal_op;
			} else {
				/* Second live, first operand is dead here, bring it to tos. */
				if (op1_idx != 0) {
					x87_create_fxch(state, n, op1_idx);
					if (op2_idx == 0)
						op2_idx = op1_idx;
					op1_idx = 0;
				}
				/* now do fxxx (tos=tos X op) */
				out_idx = 0;
				dst = tmpl->normal_op;
			}
		} else {
			/* Second operand is dead. */
			if (op1_live_after) {
				/* First operand is live: bring second to tos. */
				if (op2_idx != 0) {
					x87_create_fxch(state, n, op2_idx);
					if (op1_idx == 0)
						op1_idx = op2_idx;
					op2_idx = 0;
				}
				/* now do fxxxr (tos = op X tos) */
				out_idx = 0;
				dst = tmpl->reverse_op;
			} else {
				/* Both operands are dead here, pop them from the stack. */
				if (op2_idx == 0) {
					if (op1_idx == 0) {
						/* Both are identically and on tos, no pop needed. */
						/* here fxxx (tos = tos X tos) */
						dst = tmpl->normal_op;
						out_idx = 0;
					} else {
						/* now do fxxxp (op = op X tos, pop) */
						dst = tmpl->normal_pop_op;
						do_pop = 1;
						out_idx = op1_idx;
					}
				} else if (op1_idx == 0) {
					assert(op1_idx != op2_idx);
					/* now do fxxxrp (op = tos X op, pop) */
					dst = tmpl->reverse_pop_op;
					do_pop = 1;
					out_idx = op2_idx;
				} else {
					/* Bring the second on top. */
					x87_create_fxch(state, n, op2_idx);
					if (op1_idx == op2_idx) {
						/* Both are identically and on tos now, no pop needed. */
						op1_idx = 0;
						op2_idx = 0;
						/* use fxxx (tos = tos X tos) */
						dst = tmpl->normal_op;
						out_idx = 0;
					} else {
						/* op2 is on tos now */
						op2_idx = 0;
						/* use fxxxp (op = op X tos, pop) */
						dst = tmpl->normal_pop_op;
						out_idx = op1_idx;
						do_pop = 1;
					}
				}
			}
		}
	} else {
		/* second operand is an address mode */
		if (op1_live_after) {
			/* first operand is live: push it here */
			x87_create_fpush(state, n, op1_idx, n_ia32_binary_left);
			op1_idx = 0;
		} else {
			/* first operand is dead: bring it to tos */
			if (op1_idx != 0) {
				x87_create_fxch(state, n, op1_idx);
				op1_idx = 0;
			}
		}

		/* use fxxx (tos = tos X mem) */
		dst = permuted ? tmpl->reverse_op : tmpl->normal_op;
		out_idx = 0;
	}

	patched_insn = x87_patch_insn(n, dst);
	x87_set_st(state, arch_register_get_index(out), patched_insn, out_idx);
	if (do_pop) {
		x87_pop(state);
	}

	/* patch the operation */
	attr->x87[0] = op1_reg = get_st_reg(op1_idx);
	if (reg_index_2 != REG_VFP_VFP_NOREG) {
		attr->x87[1] = op2_reg = get_st_reg(op2_idx);
	}
	attr->x87[2] = out = get_st_reg(out_idx);

	if (reg_index_2 != REG_VFP_VFP_NOREG) {
		DB((dbg, LEVEL_1, "<<< %s %s, %s -> %s\n", get_irn_opname(n),
			arch_register_get_name(op1_reg), arch_register_get_name(op2_reg),
			arch_register_get_name(out)));
	} else {
		DB((dbg, LEVEL_1, "<<< %s %s, [AM] -> %s\n", get_irn_opname(n),
			arch_register_get_name(op1_reg),
			arch_register_get_name(out)));
	}

	return NO_NODE_ADDED;
}

/**
 * Simulate a virtual Unop.
 *
 * @param state  the x87 state
 * @param n      the node that should be simulated (and patched)
 * @param op     the x87 opcode that will replace n's opcode
 *
 * @return NO_NODE_ADDED
 */
static int sim_unop(x87_state *state, ir_node *n, ir_op *op)
{
	x87_simulator         *sim = state->sim;
	const arch_register_t *op1 = x87_get_irn_register(get_irn_n(n, 0));
	const arch_register_t *out = x87_get_irn_register(n);
	ia32_x87_attr_t *attr;
	unsigned live = vfp_live_args_after(sim, n, REGMASK(out));

	DB((dbg, LEVEL_1, ">>> %+F -> %s\n", n, out->name));
	DEBUG_ONLY(vfp_dump_live(live);)

	int op1_idx = x87_on_stack(state, arch_register_get_index(op1));

	if (is_vfp_live(arch_register_get_index(op1), live)) {
		/* push the operand here */
		x87_create_fpush(state, n, op1_idx, 0);
		op1_idx = 0;
	} else {
		/* operand is dead, bring it to tos */
		if (op1_idx != 0) {
			x87_create_fxch(state, n, op1_idx);
		}
	}

	x87_set_tos(state, arch_register_get_index(out), x87_patch_insn(n, op));
	attr = get_ia32_x87_attr(n);
	attr->x87[0] = op1 = get_st_reg(0);
	attr->x87[2] = out = get_st_reg(0);
	DB((dbg, LEVEL_1, "<<< %s -> %s\n", get_irn_opname(n), out->name));

	return NO_NODE_ADDED;
}

/**
 * Simulate a virtual Load instruction.
 *
 * @param state  the x87 state
 * @param n      the node that should be simulated (and patched)
 * @param op     the x87 opcode that will replace n's opcode
 *
 * @return NO_NODE_ADDED
 */
static int sim_load(x87_state *state, ir_node *n, ir_op *op, int res_pos)
{
	const arch_register_t *out = x87_irn_get_register(n, res_pos);
	ia32_x87_attr_t *attr;

	DB((dbg, LEVEL_1, ">>> %+F -> %s\n", n, arch_register_get_name(out)));
	x87_push(state, arch_register_get_index(out), x87_patch_insn(n, op));
	assert(out == x87_irn_get_register(n, res_pos));
	attr = get_ia32_x87_attr(n);
	attr->x87[2] = out = get_st_reg(0);
	DB((dbg, LEVEL_1, "<<< %s -> %s\n", get_irn_opname(n), arch_register_get_name(out)));

	return NO_NODE_ADDED;
}

/**
 * Rewire all users of @p old_val to @new_val iff they are scheduled after @p store.
 *
 * @param store   The store
 * @param old_val The former value
 * @param new_val The new value
 */
static void collect_and_rewire_users(ir_node *store, ir_node *old_val, ir_node *new_val)
{
	foreach_out_edge_safe(old_val, edge) {
		ir_node *user = get_edge_src_irn(edge);

		if (! user || user == store)
			continue;

		/* if the user is scheduled after the store: rewire */
		if (sched_is_scheduled(user) && sched_comes_after(store, user)) {
			int i;
			/* find the input of the user pointing to the old value */
			for (i = get_irn_arity(user) - 1; i >= 0; i--) {
				if (get_irn_n(user, i) == old_val)
					set_irn_n(user, i, new_val);
			}
		}
	}
}

/**
 * Simulate a virtual Store.
 *
 * @param state  the x87 state
 * @param n      the node that should be simulated (and patched)
 * @param op     the x87 store opcode
 * @param op_p   the x87 store and pop opcode
 */
static int sim_store(x87_state *state, ir_node *n, ir_op *op, ir_op *op_p)
{
	ir_node               *val = get_irn_n(n, n_ia32_vfst_val);
	const arch_register_t *op2 = x87_get_irn_register(val);
	unsigned              live = vfp_live_args_after(state->sim, n, 0);
	int                   insn = NO_NODE_ADDED;
	ia32_x87_attr_t *attr;
	int op2_reg_idx, op2_idx, depth;
	int live_after_node;
	ir_mode *mode;

	op2_reg_idx = arch_register_get_index(op2);
	op2_idx = x87_on_stack(state, op2_reg_idx);
	live_after_node = is_vfp_live(arch_register_get_index(op2), live);
	DB((dbg, LEVEL_1, ">>> %+F %s ->\n", n, arch_register_get_name(op2)));
	assert(op2_idx >= 0);

	mode  = get_ia32_ls_mode(n);
	depth = x87_get_depth(state);

	if (live_after_node) {
		/*
			Problem: fst doesn't support 96bit modes (spills), only fstp does
			         fist doesn't support 64bit mode, only fistp
			Solution:
				- stack not full: push value and fstp
				- stack full: fstp value and load again
			Note that we cannot test on mode_E, because floats might be 96bit ...
		*/
		if (get_mode_size_bits(mode) > 64 || (mode_is_int(mode) && get_mode_size_bits(mode) > 32)) {
			if (depth < N_ia32_st_REGS) {
				/* ok, we have a free register: push + fstp */
				x87_create_fpush(state, n, op2_idx, n_ia32_vfst_val);
				x87_pop(state);
				x87_patch_insn(n, op_p);
			} else {
				ir_node  *vfld, *mem, *block, *rproj, *mproj;
				ir_graph *irg   = get_irn_irg(n);
				ir_node  *nomem = get_irg_no_mem(irg);

				/* stack full here: need fstp + load */
				x87_pop(state);
				x87_patch_insn(n, op_p);

				block = get_nodes_block(n);
				vfld  = new_bd_ia32_vfld(NULL, block, get_irn_n(n, 0), get_irn_n(n, 1), nomem, get_ia32_ls_mode(n));

				/* copy all attributes */
				set_ia32_frame_ent(vfld, get_ia32_frame_ent(n));
				if (is_ia32_use_frame(n))
					set_ia32_use_frame(vfld);
				set_ia32_op_type(vfld, ia32_AddrModeS);
				add_ia32_am_offs_int(vfld, get_ia32_am_offs_int(n));
				set_ia32_am_sc(vfld, get_ia32_am_sc(n));
				set_ia32_ls_mode(vfld, get_ia32_ls_mode(n));

				rproj = new_r_Proj(vfld, get_ia32_ls_mode(vfld), pn_ia32_vfld_res);
				mproj = new_r_Proj(vfld, mode_M, pn_ia32_vfld_M);
				mem   = get_irn_Proj_for_mode(n, mode_M);

				assert(mem && "Store memory not found");

				arch_set_irn_register(rproj, op2);

				/* reroute all former users of the store memory to the load memory */
				edges_reroute(mem, mproj);
				/* set the memory input of the load to the store memory */
				set_irn_n(vfld, n_ia32_vfld_mem, mem);

				sched_add_after(n, vfld);
				sched_add_after(vfld, rproj);

				/* rewire all users, scheduled after the store, to the loaded value */
				collect_and_rewire_users(n, val, rproj);

				insn = NODE_ADDED;
			}
		} else {
			/* we can only store the tos to memory */
			if (op2_idx != 0)
				x87_create_fxch(state, n, op2_idx);

			/* mode size 64 or smaller -> use normal fst */
			x87_patch_insn(n, op);
		}
	} else {
		/* we can only store the tos to memory */
		if (op2_idx != 0)
			x87_create_fxch(state, n, op2_idx);

		x87_pop(state);
		x87_patch_insn(n, op_p);
	}

	attr = get_ia32_x87_attr(n);
	attr->x87[1] = op2 = get_st_reg(0);
	DB((dbg, LEVEL_1, "<<< %s %s ->\n", get_irn_opname(n), arch_register_get_name(op2)));

	return insn;
}

#define _GEN_BINOP(op, rev) \
static int sim_##op(x87_state *state, ir_node *n) { \
	exchange_tmpl tmpl = { op_ia32_##op, op_ia32_##rev, op_ia32_##op##p, op_ia32_##rev##p }; \
	return sim_binop(state, n, &tmpl); \
}

#define GEN_BINOP(op)   _GEN_BINOP(op, op)
#define GEN_BINOPR(op)  _GEN_BINOP(op, op##r)

#define GEN_LOAD(op)                                              \
static int sim_##op(x87_state *state, ir_node *n) {               \
	return sim_load(state, n, op_ia32_##op, pn_ia32_v##op##_res); \
}

#define GEN_UNOP(op) \
static int sim_##op(x87_state *state, ir_node *n) { \
	return sim_unop(state, n, op_ia32_##op); \
}

#define GEN_STORE(op) \
static int sim_##op(x87_state *state, ir_node *n) { \
	return sim_store(state, n, op_ia32_##op, op_ia32_##op##p); \
}

/* all stubs */
GEN_BINOP(fadd)
GEN_BINOPR(fsub)
GEN_BINOP(fmul)
GEN_BINOPR(fdiv)
GEN_BINOP(fprem)

GEN_UNOP(fabs)
GEN_UNOP(fchs)

GEN_LOAD(fld)
GEN_LOAD(fild)
GEN_LOAD(fldz)
GEN_LOAD(fld1)

GEN_STORE(fst)
GEN_STORE(fist)

/**
 * Simulate a virtual fisttp.
 *
 * @param state  the x87 state
 * @param n      the node that should be simulated (and patched)
 *
 * @return NO_NODE_ADDED
 */
static int sim_fisttp(x87_state *state, ir_node *n)
{
	ir_node               *val = get_irn_n(n, n_ia32_vfst_val);
	const arch_register_t *op2 = x87_get_irn_register(val);
	ia32_x87_attr_t *attr;
	int op2_reg_idx, op2_idx;

	op2_reg_idx = arch_register_get_index(op2);
	op2_idx     = x87_on_stack(state, op2_reg_idx);
	DB((dbg, LEVEL_1, ">>> %+F %s ->\n", n, arch_register_get_name(op2)));
	assert(op2_idx >= 0);

	/* Note: although the value is still live here, it is destroyed because
	   of the pop. The register allocator is aware of that and introduced a copy
	   if the value must be alive. */

	/* we can only store the tos to memory */
	if (op2_idx != 0)
		x87_create_fxch(state, n, op2_idx);

	x87_pop(state);
	x87_patch_insn(n, op_ia32_fisttp);

	attr = get_ia32_x87_attr(n);
	attr->x87[1] = op2 = get_st_reg(0);
	DB((dbg, LEVEL_1, "<<< %s %s ->\n", get_irn_opname(n), arch_register_get_name(op2)));

	return NO_NODE_ADDED;
}

/**
 * Simulate a virtual FtstFnstsw.
 *
 * @param state  the x87 state
 * @param n      the node that should be simulated (and patched)
 *
 * @return NO_NODE_ADDED
 */
static int sim_FtstFnstsw(x87_state *state, ir_node *n)
{
	x87_simulator         *sim         = state->sim;
	ia32_x87_attr_t       *attr        = get_ia32_x87_attr(n);
	ir_node               *op1_node    = get_irn_n(n, n_ia32_vFtstFnstsw_left);
	const arch_register_t *reg1        = x87_get_irn_register(op1_node);
	int                    reg_index_1 = arch_register_get_index(reg1);
	int                    op1_idx     = x87_on_stack(state, reg_index_1);
	unsigned               live        = vfp_live_args_after(sim, n, 0);

	DB((dbg, LEVEL_1, ">>> %+F %s\n", n, arch_register_get_name(reg1)));
	DEBUG_ONLY(vfp_dump_live(live);)
	DB((dbg, LEVEL_1, "Stack before: "));
	DEBUG_ONLY(x87_dump_stack(state);)
	assert(op1_idx >= 0);

	if (op1_idx != 0) {
		/* bring the value to tos */
		x87_create_fxch(state, n, op1_idx);
		op1_idx = 0;
	}

	/* patch the operation */
	x87_patch_insn(n, op_ia32_FtstFnstsw);
	reg1 = get_st_reg(op1_idx);
	attr->x87[0] = reg1;
	attr->x87[1] = NULL;
	attr->x87[2] = NULL;

	if (!is_vfp_live(reg_index_1, live))
		x87_create_fpop(state, sched_next(n), 1);

	return NO_NODE_ADDED;
}

/**
 * Simulate a Fucom
 *
 * @param state  the x87 state
 * @param n      the node that should be simulated (and patched)
 *
 * @return NO_NODE_ADDED
 */
static int sim_Fucom(x87_state *state, ir_node *n)
{
	int op1_idx;
	int op2_idx = -1;
	ia32_x87_attr_t *attr = get_ia32_x87_attr(n);
	ir_op *dst;
	x87_simulator         *sim        = state->sim;
	ir_node               *op1_node   = get_irn_n(n, n_ia32_vFucomFnstsw_left);
	ir_node               *op2_node   = get_irn_n(n, n_ia32_vFucomFnstsw_right);
	const arch_register_t *op1        = x87_get_irn_register(op1_node);
	const arch_register_t *op2        = x87_get_irn_register(op2_node);
	int reg_index_1 = arch_register_get_index(op1);
	int                    reg_index_2 = arch_register_get_index(op2);
	unsigned               live       = vfp_live_args_after(sim, n, 0);
	bool                   permuted   = attr->attr.data.ins_permuted;
	bool                   xchg       = false;
	int                    pops       = 0;

	DB((dbg, LEVEL_1, ">>> %+F %s, %s\n", n,
		arch_register_get_name(op1), arch_register_get_name(op2)));
	DEBUG_ONLY(vfp_dump_live(live);)
	DB((dbg, LEVEL_1, "Stack before: "));
	DEBUG_ONLY(x87_dump_stack(state);)

	op1_idx = x87_on_stack(state, reg_index_1);
	assert(op1_idx >= 0);

	/* BEWARE: check for comp a,a cases, they might happen */
	if (reg_index_2 != REG_VFP_VFP_NOREG) {
		/* second operand is a vfp register */
		op2_idx = x87_on_stack(state, reg_index_2);
		assert(op2_idx >= 0);

		if (is_vfp_live(reg_index_2, live)) {
			/* second operand is live */

			if (is_vfp_live(reg_index_1, live)) {
				/* both operands are live */

				if (op1_idx == 0) {
					/* res = tos X op */
				} else if (op2_idx == 0) {
					/* res = op X tos */
					permuted = !permuted;
					xchg     = true;
				} else {
					/* bring the first one to tos */
					x87_create_fxch(state, n, op1_idx);
					if (op1_idx == op2_idx) {
						op2_idx = 0;
					} else if (op2_idx == 0) {
						op2_idx = op1_idx;
					}
					op1_idx = 0;
					/* res = tos X op */
				}
			} else {
				/* second live, first operand is dead here, bring it to tos.
				   This means further, op1_idx != op2_idx. */
				assert(op1_idx != op2_idx);
				if (op1_idx != 0) {
					x87_create_fxch(state, n, op1_idx);
					if (op2_idx == 0)
						op2_idx = op1_idx;
					op1_idx = 0;
				}
				/* res = tos X op, pop */
				pops = 1;
			}
		} else {
			/* second operand is dead */
			if (is_vfp_live(reg_index_1, live)) {
				/* first operand is live: bring second to tos.
				   This means further, op1_idx != op2_idx. */
				assert(op1_idx != op2_idx);
				if (op2_idx != 0) {
					x87_create_fxch(state, n, op2_idx);
					if (op1_idx == 0)
						op1_idx = op2_idx;
					op2_idx = 0;
				}
				/* res = op X tos, pop */
				pops     = 1;
				permuted = !permuted;
				xchg     = true;
			} else {
				/* both operands are dead here, check first for identity. */
				if (op1_idx == op2_idx) {
					/* identically, one pop needed */
					if (op1_idx != 0) {
						x87_create_fxch(state, n, op1_idx);
						op1_idx = 0;
						op2_idx = 0;
					}
					/* res = tos X op, pop */
					pops    = 1;
				}
				/* different, move them to st and st(1) and pop both.
				   The tricky part is to get one into st(1).*/
				else if (op2_idx == 1) {
					/* good, second operand is already in the right place, move the first */
					if (op1_idx != 0) {
						/* bring the first on top */
						x87_create_fxch(state, n, op1_idx);
						assert(op2_idx != 0);
						op1_idx = 0;
					}
					/* res = tos X op, pop, pop */
					pops = 2;
				} else if (op1_idx == 1) {
					/* good, first operand is already in the right place, move the second */
					if (op2_idx != 0) {
						/* bring the first on top */
						x87_create_fxch(state, n, op2_idx);
						assert(op1_idx != 0);
						op2_idx = 0;
					}
					/* res = op X tos, pop, pop */
					permuted = !permuted;
					xchg     = true;
					pops     = 2;
				} else {
					/* if one is already the TOS, we need two fxch */
					if (op1_idx == 0) {
						/* first one is TOS, move to st(1) */
						x87_create_fxch(state, n, 1);
						assert(op2_idx != 1);
						op1_idx = 1;
						x87_create_fxch(state, n, op2_idx);
						op2_idx = 0;
						/* res = op X tos, pop, pop */
						pops     = 2;
						permuted = !permuted;
						xchg     = true;
					} else if (op2_idx == 0) {
						/* second one is TOS, move to st(1) */
						x87_create_fxch(state, n, 1);
						assert(op1_idx != 1);
						op2_idx = 1;
						x87_create_fxch(state, n, op1_idx);
						op1_idx = 0;
						/* res = tos X op, pop, pop */
						pops    = 2;
					} else {
						/* none of them is either TOS or st(1), 3 fxch needed */
						x87_create_fxch(state, n, op2_idx);
						assert(op1_idx != 0);
						x87_create_fxch(state, n, 1);
						op2_idx = 1;
						x87_create_fxch(state, n, op1_idx);
						op1_idx = 0;
						/* res = tos X op, pop, pop */
						pops    = 2;
					}
				}
			}
		}
	} else {
		/* second operand is an address mode */
		if (is_vfp_live(reg_index_1, live)) {
			/* first operand is live: bring it to TOS */
			if (op1_idx != 0) {
				x87_create_fxch(state, n, op1_idx);
				op1_idx = 0;
			}
		} else {
			/* first operand is dead: bring it to tos */
			if (op1_idx != 0) {
				x87_create_fxch(state, n, op1_idx);
				op1_idx = 0;
			}
			pops = 1;
		}
	}

	/* patch the operation */
	if (is_ia32_vFucomFnstsw(n)) {
		int i;

		switch (pops) {
		case 0: dst = op_ia32_FucomFnstsw;   break;
		case 1: dst = op_ia32_FucompFnstsw;  break;
		case 2: dst = op_ia32_FucomppFnstsw; break;
		default: panic("invalid popcount");
		}

		for (i = 0; i < pops; ++i) {
			x87_pop(state);
		}
	} else if (is_ia32_vFucomi(n)) {
		switch (pops) {
		case 0: dst = op_ia32_Fucomi;                  break;
		case 1: dst = op_ia32_Fucompi; x87_pop(state); break;
		case 2:
			dst = op_ia32_Fucompi;
			x87_pop(state);
			x87_create_fpop(state, sched_next(n), 1);
			break;
		default: panic("invalid popcount");
		}
	} else {
		panic("invalid operation %+F", n);
	}

	x87_patch_insn(n, dst);
	if (xchg) {
		int tmp = op1_idx;
		op1_idx = op2_idx;
		op2_idx = tmp;
	}

	op1 = get_st_reg(op1_idx);
	attr->x87[0] = op1;
	if (op2_idx >= 0) {
		op2 = get_st_reg(op2_idx);
		attr->x87[1] = op2;
	}
	attr->x87[2] = NULL;
	attr->attr.data.ins_permuted = permuted;

	if (op2_idx >= 0) {
		DB((dbg, LEVEL_1, "<<< %s %s, %s\n", get_irn_opname(n),
			arch_register_get_name(op1), arch_register_get_name(op2)));
	} else {
		DB((dbg, LEVEL_1, "<<< %s %s, [AM]\n", get_irn_opname(n),
			arch_register_get_name(op1)));
	}

	return NO_NODE_ADDED;
}

/**
 * Simulate a Keep.
 *
 * @param state  the x87 state
 * @param n      the node that should be simulated (and patched)
 *
 * @return NO_NODE_ADDED
 */
static int sim_Keep(x87_state *state, ir_node *node)
{
	const ir_node         *op;
	const arch_register_t *op_reg;
	int                    reg_id;
	int                    op_stack_idx;
	unsigned               live;
	int                    i, arity;

	DB((dbg, LEVEL_1, ">>> %+F\n", node));

	arity = get_irn_arity(node);
	for (i = 0; i < arity; ++i) {
		op      = get_irn_n(node, i);
		op_reg  = arch_get_irn_register(op);
		if (arch_register_get_class(op_reg) != &ia32_reg_classes[CLASS_ia32_vfp])
			continue;

		reg_id = arch_register_get_index(op_reg);
		live   = vfp_live_args_after(state->sim, node, 0);

		op_stack_idx = x87_on_stack(state, reg_id);
		if (op_stack_idx >= 0 && !is_vfp_live(reg_id, live))
			x87_create_fpop(state, sched_next(node), 1);
	}

	DB((dbg, LEVEL_1, "Stack after: "));
	DEBUG_ONLY(x87_dump_stack(state);)

	return NO_NODE_ADDED;
}

/**
 * Keep the given node alive by adding a be_Keep.
 *
 * @param node  the node to kept alive
 */
static void keep_float_node_alive(ir_node *node)
{
	ir_node *block = get_nodes_block(node);
	ir_node *keep  = be_new_Keep(block, 1, &node);

	assert(sched_is_scheduled(node));
	sched_add_after(node, keep);
}

/**
 * Create a copy of a node. Recreate the node if it's a constant.
 *
 * @param state  the x87 state
 * @param n      the node to be copied
 *
 * @return the copy of n
 */
static ir_node *create_Copy(x87_state *state, ir_node *n)
{
	dbg_info *n_dbg = get_irn_dbg_info(n);
	ir_mode *mode = get_irn_mode(n);
	ir_node *block = get_nodes_block(n);
	ir_node *pred = get_irn_n(n, 0);
	ir_node *(*cnstr)(dbg_info *, ir_node *, ir_mode *) = NULL;
	ir_node *res;
	const arch_register_t *out;
	const arch_register_t *op1;
	ia32_x87_attr_t *attr;

	/* Do not copy constants, recreate them. */
	switch (get_ia32_irn_opcode(pred)) {
	case iro_ia32_fldz:
		cnstr = new_bd_ia32_fldz;
		break;
	case iro_ia32_fld1:
		cnstr = new_bd_ia32_fld1;
		break;
	case iro_ia32_fldpi:
		cnstr = new_bd_ia32_fldpi;
		break;
	case iro_ia32_fldl2e:
		cnstr = new_bd_ia32_fldl2e;
		break;
	case iro_ia32_fldl2t:
		cnstr = new_bd_ia32_fldl2t;
		break;
	case iro_ia32_fldlg2:
		cnstr = new_bd_ia32_fldlg2;
		break;
	case iro_ia32_fldln2:
		cnstr = new_bd_ia32_fldln2;
		break;
	default:
		break;
	}

	out = x87_get_irn_register(n);
	op1 = x87_get_irn_register(pred);

	if (cnstr != NULL) {
		/* copy a constant */
		res = (*cnstr)(n_dbg, block, mode);

		x87_push(state, arch_register_get_index(out), res);

		attr = get_ia32_x87_attr(res);
		attr->x87[2] = get_st_reg(0);
	} else {
		int op1_idx = x87_on_stack(state, arch_register_get_index(op1));

		res = new_bd_ia32_fpushCopy(n_dbg, block, pred, mode);

		x87_push(state, arch_register_get_index(out), res);

		attr = get_ia32_x87_attr(res);
		attr->x87[0] = get_st_reg(op1_idx);
		attr->x87[2] = get_st_reg(0);
	}
	arch_set_irn_register(res, out);

	return res;
}

/**
 * Simulate a be_Copy.
 *
 * @param state  the x87 state
 * @param n      the node that should be simulated (and patched)
 *
 * @return NO_NODE_ADDED
 */
static int sim_Copy(x87_state *state, ir_node *n)
{
	ir_node                     *pred;
	const arch_register_t       *out;
	const arch_register_t       *op1;
	const arch_register_class_t *cls;
	ir_node                     *node, *next;
	int                         op1_idx, out_idx;
	unsigned                    live;

	cls = arch_get_irn_reg_class(n);
	if (cls != &ia32_reg_classes[CLASS_ia32_vfp])
		return 0;

	pred = be_get_Copy_op(n);
	out  = x87_get_irn_register(n);
	op1  = x87_get_irn_register(pred);
	live = vfp_live_args_after(state->sim, n, REGMASK(out));

	DB((dbg, LEVEL_1, ">>> %+F %s -> %s\n", n,
		arch_register_get_name(op1), arch_register_get_name(out)));
	DEBUG_ONLY(vfp_dump_live(live);)

	op1_idx = x87_on_stack(state, arch_register_get_index(op1));

	if (is_vfp_live(arch_register_get_index(op1), live)) {
		/* Operand is still live, a real copy. We need here an fpush that can
		   hold a a register, so use the fpushCopy or recreate constants */
		node = create_Copy(state, n);

		/* We have to make sure the old value doesn't go dead (which can happen
		 * when we recreate constants). As the simulator expected that value in
		 * the pred blocks. This is unfortunate as removing it would save us 1
		 * instruction, but we would have to rerun all the simulation to get
		 * this correct...
		 */
		next = sched_next(n);
		sched_remove(n);
		exchange(n, node);
		sched_add_before(next, node);

		if (get_irn_n_edges(pred) == 0) {
			keep_float_node_alive(pred);
		}

		DB((dbg, LEVEL_1, "<<< %+F %s -> ?\n", node, op1->name));
	} else {
		out_idx = x87_on_stack(state, arch_register_get_index(out));

		if (out_idx >= 0 && out_idx != op1_idx) {
			/* Matze: out already on stack? how can this happen? */
			panic("invalid stack state");

#if 0
			/* op1 must be killed and placed where out is */
			if (out_idx == 0) {
				ia32_x87_attr_t *attr;
				/* best case, simple remove and rename */
				x87_patch_insn(n, op_ia32_Pop);
				attr = get_ia32_x87_attr(n);
				attr->x87[0] = op1 = get_st_reg(0);

				x87_pop(state);
				x87_set_st(state, arch_register_get_index(out), n, op1_idx - 1);
			} else {
				ia32_x87_attr_t *attr;
				/* move op1 to tos, store and pop it */
				if (op1_idx != 0) {
					x87_create_fxch(state, n, op1_idx);
					op1_idx = 0;
				}
				x87_patch_insn(n, op_ia32_Pop);
				attr = get_ia32_x87_attr(n);
				attr->x87[0] = op1 = get_st_reg(out_idx);

				x87_pop(state);
				x87_set_st(state, arch_register_get_index(out), n, out_idx - 1);
			}
			DB((dbg, LEVEL_1, "<<< %+F %s\n", n, op1->name));
#endif
		} else {
			/* just a virtual copy */
			x87_set_st(state, arch_register_get_index(out), pred, op1_idx);
			/* don't remove the node to keep the verifier quiet :),
			   the emitter won't emit any code for the node */
#if 0
			sched_remove(n);
			DB((dbg, LEVEL_1, "<<< KILLED %s\n", get_irn_opname(n)));
			exchange(n, pred);
#endif
		}
	}
	return NO_NODE_ADDED;
}

/**
 * Returns the vf0 result Proj of a Call.
 *
 * @para call  the Call node
 */
static ir_node *get_call_result_proj(ir_node *call)
{
	/* search the result proj */
	foreach_out_edge(call, edge) {
		ir_node *proj = get_edge_src_irn(edge);
		long pn = get_Proj_proj(proj);

		if (pn == pn_ia32_Call_vf0)
			return proj;
	}

	panic("result Proj missing");
}

static int sim_Asm(x87_state *const state, ir_node *const n)
{
	(void)state;

	for (size_t i = get_irn_arity(n); i-- != 0;) {
		arch_register_req_t const *const req = arch_get_irn_register_req_in(n, i);
		if (req->cls == &ia32_reg_classes[CLASS_ia32_vfp])
			panic("cannot handle %+F with x87 constraints", n);
	}

	for (size_t i = arch_get_irn_n_outs(n); i-- != 0;) {
		arch_register_req_t const *const req = arch_get_irn_register_req_out(n, i);
		if (req->cls == &ia32_reg_classes[CLASS_ia32_vfp])
			panic("cannot handle %+F with x87 constraints", n);
	}

	return NO_NODE_ADDED;
}

/**
 * Simulate a ia32_Call.
 *
 * @param state      the x87 state
 * @param n          the node that should be simulated (and patched)
 *
 * @return NO_NODE_ADDED
 */
static int sim_Call(x87_state *state, ir_node *n)
{
	ir_type *call_tp = get_ia32_call_attr_const(n)->call_tp;
	ir_type *res_type;
	ir_mode *mode;
	ir_node *resproj;
	const arch_register_t *reg;

	DB((dbg, LEVEL_1, ">>> %+F\n", n));

	/* at the begin of a call the x87 state should be empty */
	assert(state->depth == 0 && "stack not empty before call");

	if (get_method_n_ress(call_tp) <= 0)
		goto end_call;

	/*
	 * If the called function returns a float, it is returned in st(0).
	 * This even happens if the return value is NOT used.
	 * Moreover, only one return result is supported.
	 */
	res_type = get_method_res_type(call_tp, 0);
	mode     = get_type_mode(res_type);

	if (mode == NULL || !mode_is_float(mode))
		goto end_call;

	resproj = get_call_result_proj(n);

	reg = x87_get_irn_register(resproj);
	x87_push(state, arch_register_get_index(reg), resproj);

end_call:
	DB((dbg, LEVEL_1, "Stack after: "));
	DEBUG_ONLY(x87_dump_stack(state);)

	return NO_NODE_ADDED;
}

/**
 * Simulate a be_Return.
 *
 * @param state  the x87 state
 * @param n      the node that should be simulated (and patched)
 *
 * @return NO_NODE_ADDED
 */
static int sim_Return(x87_state *state, ir_node *n)
{
#ifdef DEBUG_libfirm
	/* only floating point return values must reside on stack */
	int       n_float_res = 0;
	int const n_res       = be_Return_get_n_rets(n);
	for (int i = 0; i < n_res; ++i) {
		ir_node *const res = get_irn_n(n, n_be_Return_val + i);
		if (mode_is_float(get_irn_mode(res)))
			++n_float_res;
	}
	assert(x87_get_depth(state) == n_float_res);
#endif

	/* pop them virtually */
	x87_emms(state);
	return NO_NODE_ADDED;
}

/**
 * Simulate a be_Perm.
 *
 * @param state  the x87 state
 * @param irn    the node that should be simulated (and patched)
 *
 * @return NO_NODE_ADDED
 */
static int sim_Perm(x87_state *state, ir_node *irn)
{
	int      i, n;
	ir_node *pred = get_irn_n(irn, 0);
	int     *stack_pos;

	/* handle only floating point Perms */
	if (! mode_is_float(get_irn_mode(pred)))
		return NO_NODE_ADDED;

	DB((dbg, LEVEL_1, ">>> %+F\n", irn));

	/* Perm is a pure virtual instruction on x87.
	   All inputs must be on the FPU stack and are pairwise
	   different from each other.
	   So, all we need to do is to permutate the stack state. */
	n = get_irn_arity(irn);
	NEW_ARR_A(int, stack_pos, n);

	/* collect old stack positions */
	for (i = 0; i < n; ++i) {
		const arch_register_t *inreg = x87_get_irn_register(get_irn_n(irn, i));
		int idx = x87_on_stack(state, arch_register_get_index(inreg));

		assert(idx >= 0 && "Perm argument not on x87 stack");

		stack_pos[i] = idx;
	}
	/* now do the permutation */
	foreach_out_edge(irn, edge) {
		ir_node               *proj = get_edge_src_irn(edge);
		const arch_register_t *out  = x87_get_irn_register(proj);
		long                  num   = get_Proj_proj(proj);

		assert(0 <= num && num < n && "More Proj's than Perm inputs");
		x87_set_st(state, arch_register_get_index(out), proj, stack_pos[(unsigned)num]);
	}
	DB((dbg, LEVEL_1, "<<< %+F\n", irn));

	return NO_NODE_ADDED;
}

/**
 * Kill any dead registers at block start by popping them from the stack.
 *
 * @param sim    the simulator handle
 * @param block  the current block
 * @param state  the x87 state at the begin of the block
 */
static void x87_kill_deads(x87_simulator *const sim, ir_node *const block, x87_state *const state)
{
	ir_node *first_insn = sched_first(block);
	ir_node *keep = NULL;
	unsigned live = vfp_live_args_after(sim, block, 0);
	unsigned kill_mask;
	int i, depth, num_pop;

	kill_mask = 0;
	depth = x87_get_depth(state);
	for (i = depth - 1; i >= 0; --i) {
		int reg = x87_get_st_reg(state, i);

		if (! is_vfp_live(reg, live))
			kill_mask |= (1 << i);
	}

	if (kill_mask) {
		DB((dbg, LEVEL_1, "Killing deads:\n"));
		DEBUG_ONLY(vfp_dump_live(live);)
		DEBUG_ONLY(x87_dump_stack(state);)

		if (kill_mask != 0 && live == 0) {
			/* special case: kill all registers */
			if (ia32_cg_config.use_femms || ia32_cg_config.use_emms) {
				if (ia32_cg_config.use_femms) {
					/* use FEMMS on AMD processors to clear all */
					keep = new_bd_ia32_femms(NULL, block);
				} else {
					/* use EMMS to clear all */
					keep = new_bd_ia32_emms(NULL, block);
				}
				sched_add_before(first_insn, keep);
				keep_alive(keep);
				x87_emms(state);
				return;
			}
		}
		/* now kill registers */
		while (kill_mask) {
			/* we can only kill from TOS, so bring them up */
			if (! (kill_mask & 1)) {
				/* search from behind, because we can to a double-pop */
				for (i = depth - 1; i >= 0; --i) {
					if (kill_mask & (1 << i)) {
						kill_mask &= ~(1 << i);
						kill_mask |= 1;
						break;
					}
				}

				if (keep)
					x87_set_st(state, -1, keep, i);
				x87_create_fxch(state, first_insn, i);
			}

			if ((kill_mask & 3) == 3) {
				/* we can do a double-pop */
				num_pop = 2;
			}
			else {
				/* only a single pop */
				num_pop = 1;
			}

			depth -= num_pop;
			kill_mask >>= num_pop;
			keep = x87_create_fpop(state, first_insn, num_pop);
		}
		keep_alive(keep);
	}
}

/**
 * Run a simulation and fix all virtual instructions for a block.
 *
 * @param sim          the simulator handle
 * @param block        the current block
 */
static void x87_simulate_block(x87_simulator *sim, ir_node *block)
{
	ir_node *n, *next;
	blk_state *bl_state = x87_get_bl_state(sim, block);
	x87_state *state = bl_state->begin;
	ir_node *start_block;

	assert(state != NULL);
	/* already processed? */
	if (bl_state->end != NULL)
		return;

	DB((dbg, LEVEL_1, "Simulate %+F\n", block));
	DB((dbg, LEVEL_2, "State at Block begin:\n "));
	DEBUG_ONLY(x87_dump_stack(state);)

	/* create a new state, will be changed */
	state = x87_clone_state(sim, state);
	/* at block begin, kill all dead registers */
	x87_kill_deads(sim, block, state);

	/* beware, n might change */
	for (n = sched_first(block); !sched_is_end(n); n = next) {
		int node_inserted;
		sim_func func;
		ir_op *op = get_irn_op(n);

		/*
		 * get the next node to be simulated here.
		 * n might be completely removed from the schedule-
		 */
		next = sched_next(n);
		if (op->ops.generic != NULL) {
			func = (sim_func)op->ops.generic;

			/* simulate it */
			node_inserted = (*func)(state, n);

			/*
			 * sim_func might have added an additional node after n,
			 * so update next node
			 * beware: n must not be changed by sim_func
			 * (i.e. removed from schedule) in this case
			 */
			if (node_inserted != NO_NODE_ADDED)
				next = sched_next(n);
		}
	}

	start_block = get_irg_start_block(get_irn_irg(block));

	DB((dbg, LEVEL_2, "State at Block end:\n ")); DEBUG_ONLY(x87_dump_stack(state);)

	/* check if the state must be shuffled */
	foreach_block_succ(block, edge) {
		ir_node *succ = get_edge_src_irn(edge);
		blk_state *succ_state;

		if (succ == start_block)
			continue;

		succ_state = x87_get_bl_state(sim, succ);

		if (succ_state->begin == NULL) {
			DB((dbg, LEVEL_2, "Set begin state for succ %+F:\n", succ));
			DEBUG_ONLY(x87_dump_stack(state);)
			succ_state->begin = state;

			waitq_put(sim->worklist, succ);
		} else {
			DB((dbg, LEVEL_2, "succ %+F already has a state, shuffling\n", succ));
			/* There is already a begin state for the successor, bad.
			   Do the necessary permutations.
			   Note that critical edges are removed, so this is always possible:
			   If the successor has more than one possible input, then it must
			   be the only one.
			 */
			x87_shuffle(block, state, succ_state->begin);
		}
	}
	bl_state->end = state;
}

/**
 * Register a simulator function.
 *
 * @param op    the opcode to simulate
 * @param func  the simulator function for the opcode
 */
static void register_sim(ir_op *op, sim_func func)
{
	assert(op->ops.generic == NULL);
	op->ops.generic = (op_func) func;
}

/**
 * Create a new x87 simulator.
 *
 * @param sim       a simulator handle, will be initialized
 * @param irg       the current graph
 */
static void x87_init_simulator(x87_simulator *sim, ir_graph *irg)
{
	obstack_init(&sim->obst);
	sim->blk_states = pmap_create();
	sim->n_idx      = get_irg_last_idx(irg);
	sim->live       = OALLOCN(&sim->obst, vfp_liveness, sim->n_idx);

	DB((dbg, LEVEL_1, "--------------------------------\n"
		"x87 Simulator started for %+F\n", irg));

	/* set the generic function pointer of instruction we must simulate */
	ir_clear_opcodes_generic_func();

	register_sim(op_ia32_Asm,          sim_Asm);
	register_sim(op_ia32_Call,         sim_Call);
	register_sim(op_ia32_vfld,         sim_fld);
	register_sim(op_ia32_vfild,        sim_fild);
	register_sim(op_ia32_vfld1,        sim_fld1);
	register_sim(op_ia32_vfldz,        sim_fldz);
	register_sim(op_ia32_vfadd,        sim_fadd);
	register_sim(op_ia32_vfsub,        sim_fsub);
	register_sim(op_ia32_vfmul,        sim_fmul);
	register_sim(op_ia32_vfdiv,        sim_fdiv);
	register_sim(op_ia32_vfprem,       sim_fprem);
	register_sim(op_ia32_vfabs,        sim_fabs);
	register_sim(op_ia32_vfchs,        sim_fchs);
	register_sim(op_ia32_vfist,        sim_fist);
	register_sim(op_ia32_vfisttp,      sim_fisttp);
	register_sim(op_ia32_vfst,         sim_fst);
	register_sim(op_ia32_vFtstFnstsw,  sim_FtstFnstsw);
	register_sim(op_ia32_vFucomFnstsw, sim_Fucom);
	register_sim(op_ia32_vFucomi,      sim_Fucom);
	register_sim(op_be_Copy,           sim_Copy);
	register_sim(op_be_Return,         sim_Return);
	register_sim(op_be_Perm,           sim_Perm);
	register_sim(op_be_Keep,           sim_Keep);
}

/**
 * Destroy a x87 simulator.
 *
 * @param sim  the simulator handle
 */
static void x87_destroy_simulator(x87_simulator *sim)
{
	pmap_destroy(sim->blk_states);
	obstack_free(&sim->obst, NULL);
	DB((dbg, LEVEL_1, "x87 Simulator stopped\n\n"));
}

/**
 * Pre-block walker: calculate the liveness information for the block
 * and store it into the sim->live cache.
 */
static void update_liveness_walker(ir_node *block, void *data)
{
	x87_simulator *sim = (x87_simulator*)data;
	update_liveness(sim, block);
}

/*
 * Run a simulation and fix all virtual instructions for a graph.
 * Replaces all virtual floating point instructions and registers
 * by real ones.
 */
void ia32_x87_simulate_graph(ir_graph *irg)
{
	/* TODO improve code quality (less executed fxch) by using execfreqs */

	ir_node       *block, *start_block;
	blk_state     *bl_state;
	x87_simulator sim;

	/* create the simulator */
	x87_init_simulator(&sim, irg);

	start_block = get_irg_start_block(irg);
	bl_state    = x87_get_bl_state(&sim, start_block);

	/* start with the empty state */
	empty.sim       = &sim;
	bl_state->begin = &empty;

	sim.worklist = new_waitq();
	waitq_put(sim.worklist, start_block);

	be_assure_live_sets(irg);
	sim.lv = be_get_irg_liveness(irg);

	/* Calculate the liveness for all nodes. We must precalculate this info,
	 * because the simulator adds new nodes (possible before Phi nodes) which
	 * would let a lazy calculation fail.
	 * On the other hand we reduce the computation amount due to
	 * precaching from O(n^2) to O(n) at the expense of O(n) cache memory.
	 */
	irg_block_walk_graph(irg, update_liveness_walker, NULL, &sim);

	/* iterate */
	do {
		block = (ir_node*)waitq_get(sim.worklist);
		x87_simulate_block(&sim, block);
	} while (! waitq_empty(sim.worklist));

	/* kill it */
	del_waitq(sim.worklist);
	x87_destroy_simulator(&sim);
}

/* Initializes the x87 simulator. */
void ia32_init_x87(void)
{
	FIRM_DBG_REGISTER(dbg, "firm.be.ia32.x87");
}
