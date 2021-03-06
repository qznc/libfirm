/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief   If conversion
 * @author  Christoph Mallon
 */
#include <assert.h>
#include <stdbool.h>

#include "iroptimize.h"
#include "obst.h"
#include "irnode_t.h"
#include "cdep_t.h"
#include "ircons.h"
#include "irgmod.h"
#include "irgopt.h"
#include "irgwalk.h"
#include "irtools.h"
#include "array_t.h"
#include "irpass_t.h"
#include "be.h"

#include "irdump.h"
#include "debug.h"

/**
 * Environment for if-conversion.
 */
typedef struct walker_env {
	arch_allow_ifconv_func allow_ifconv;
	bool                   changed; /**< Set if the graph was changed. */
} walker_env;

DEBUG_ONLY(static firm_dbg_module_t *dbg;)

/**
 * Returns non-zero if a Block can be emptied.
 *
 * @param block  the block
 */
static bool can_empty_block(ir_node *block)
{
	return get_Block_mark(block) == 0;
}

/**
 * Find the ProjX node leading from block dependency to block start.
 *
 * @param start       a block that is control depended on dependency
 * @param dependency  the block that decides whether start is executed
 *
 * @return a ProjX node that represent the decision control flow or
 *         NULL is start is not dependent at all or a block on the way
 *         cannot be emptied
 */
static ir_node* walk_to_projx(ir_node* start, const ir_node* dependency)
{
	int arity;
	int i;

	/* No need to find the conditional block if this block cannot be emptied and
	 * therefore not moved */
	if (!can_empty_block(start)) return NULL;

	arity = get_irn_arity(start);
	for (i = 0; i < arity; ++i) {
		ir_node* pred = get_irn_n(start, i);
		ir_node* pred_block = get_nodes_block(skip_Proj(pred));

		if (pred_block == dependency) {
			if (is_Proj(pred)) {
				assert(get_irn_mode(pred) == mode_X);
				/* we found it */
				return pred;
			}
			/* Not a Proj? Should not happen. */
			return NULL;
		}

		if (is_Proj(pred)) {
			assert(get_irn_mode(pred) == mode_X);
			/* another Proj but not from the control block */
			return NULL;
		}

		if (is_cdep_on(pred_block, dependency)) {
			return walk_to_projx(pred_block, dependency);
		}
	}
	return NULL;
}


/**
 * Recursively copies the DAG starting at node to the i-th predecessor
 * block of src_block
 * - if node isn't in the src_block, recursion ends and node is returned
 * - if node is a Phi in the src_block, the i-th predecessor of this Phi is
 *   returned and recursion ends
 * otherwise returns a copy of the passed node created in the i-th predecessor of
 * src_block.
 *
 * @param node       a root of a DAG
 * @param src_block  the block of the DAG
 * @param i          the position of the predecessor the DAG
 *                   is moved to
 *
 * @return  the root of the copied DAG
 */
static ir_node* copy_to(ir_node* node, ir_node* src_block, int i)
{
	ir_node* dst_block;
	ir_node* copy;
	int j;

	if (get_nodes_block(node) != src_block) {
		/* already outside src_block, do not copy */
		return node;
	}
	if (is_Phi(node)) {
		/* move through the Phi to the i-th predecessor */
		return get_irn_n(node, i);
	}

	/* else really need a copy */
	copy = exact_copy(node);
	dst_block = get_nodes_block(get_irn_n(src_block, i));
	set_nodes_block(copy, dst_block);

	DB((dbg, LEVEL_1, "Copying node %+F to block %+F, copy is %+F\n",
		node, dst_block, copy));

	/* move recursively all predecessors */
	for (j = get_irn_arity(node) - 1; j >= 0; --j) {
		set_irn_n(copy, j, copy_to(get_irn_n(node, j), src_block, i));
		DB((dbg, LEVEL_2, "-- pred %d is %+F\n", j, get_irn_n(copy, j)));
	}
	return copy;
}


/**
 * Remove predecessors i and j (i < j) from a node and
 * add an additional predecessor new_pred.
 *
 * @param node      the node whose inputs are changed
 * @param i         the first index to remove
 * @param j         the second index to remove
 * @param new_pred  a node that is added as a new input to node
 */
static void rewire(ir_node* node, int i, int j, ir_node* new_pred)
{
	int arity = get_irn_arity(node);
	ir_node **ins;
	int k;
	int l;

	NEW_ARR_A(ir_node *, ins, arity - 1);

	l = 0;
	for (k = 0; k < i;     ++k) ins[l++] = get_irn_n(node, k);
	for (++k;   k < j;     ++k) ins[l++] = get_irn_n(node, k);
	for (++k;   k < arity; ++k) ins[l++] = get_irn_n(node, k);
	ins[l++] = new_pred;
	assert(l == arity - 1);
	set_irn_in(node, l, ins);
}


/**
 * Remove the j-th predecessor from the i-th predecessor of block and add it to block
 */
static void split_block(ir_node* block, int i, int j)
{
	ir_node  *pred_block = get_nodes_block(get_Block_cfgpred(block, i));
	int       arity      = get_Block_n_cfgpreds(block);
	ir_node **ins        = ALLOCAN(ir_node*, arity + 1);
	int       new_pred_arity;
	ir_node  *phi;
	ir_node  *next;
	ir_node **pred_ins;
	int       k;

	DB((dbg, LEVEL_1, "Splitting predecessor %d of predecessor %d of %+F\n", j, i, block));

	for (phi = get_Block_phis(block); phi != NULL; phi = get_Phi_next(phi)) {
		ir_node* copy = copy_to(get_irn_n(phi, i), pred_block, j);

		for (k = 0; k < i; ++k) ins[k] = get_irn_n(phi, k);
		ins[k++] = copy;
		for (; k < arity; ++k) ins[k] = get_irn_n(phi, k);
		ins[k++] = get_irn_n(phi, i);
		set_irn_in(phi, k, ins);
	}

	for (k = 0; k < i; ++k) ins[k] = get_Block_cfgpred(block, k);
	ins[k++] = get_irn_n(pred_block, j);
	for (; k < arity; ++k) ins[k] = get_Block_cfgpred(block, k);
	ins[k++] = get_Block_cfgpred(block, i);
	set_irn_in(block, k, ins);

	new_pred_arity = get_irn_arity(pred_block) - 1;
	pred_ins       = ALLOCAN(ir_node*, new_pred_arity);

	for (phi = get_Block_phis(pred_block); phi != NULL; phi = next) {
		next = get_Phi_next(phi);
		for (k = 0; k != j;              ++k) pred_ins[k] = get_irn_n(phi, k);
		for (;      k != new_pred_arity; ++k) pred_ins[k] = get_irn_n(phi, k + 1);
		if (k == 1) {
			exchange(phi, pred_ins[0]);
		} else {
			set_irn_in(phi, k, pred_ins);
		}
	}

	for (k = 0; k != j;              ++k) pred_ins[k] = get_irn_n(pred_block, k);
	for (;      k != new_pred_arity; ++k) pred_ins[k] = get_irn_n(pred_block, k + 1);
	if (k == 1) {
		exchange(pred_block, get_nodes_block(pred_ins[0]));
	} else {
		set_irn_in(pred_block, k, pred_ins);
	}
}


static void prepare_path(ir_node* block, int i, const ir_node* dependency)
{
	ir_node* pred = get_nodes_block(get_Block_cfgpred(block, i));
	int pred_arity;
	int j;

	DB((dbg, LEVEL_1, "Preparing predecessor %d of %+F\n", i, block));

	pred_arity = get_irn_arity(pred);
	for (j = 0; j < pred_arity; ++j) {
		ir_node* pred_pred = get_nodes_block(get_irn_n(pred, j));

		if (pred_pred != dependency && is_cdep_on(pred_pred, dependency)) {
			prepare_path(pred, j, dependency);
			split_block(block, i, j);
			break;
		}
	}
}

/**
 * Block walker: Search for diamonds and do the if conversion.
 */
static void if_conv_walker(ir_node *block, void *ctx)
{
	walker_env *env = (walker_env*)ctx;
	int arity;
	int i;

	/* Bail out, if there are no Phis at all */
	if (get_Block_phis(block) == NULL) return;

restart:
	arity = get_irn_arity(block);
	for (i = 0; i < arity; ++i) {
		ir_node* pred0;
		ir_cdep* cdep;

		pred0 = get_Block_cfgpred_block(block, i);
		if (pred0 == block) continue;

		for (cdep = find_cdep(pred0); cdep != NULL; cdep = get_cdep_next(cdep)) {
			const ir_node* dependency = get_cdep_node(cdep);
			ir_node* projx0 = walk_to_projx(pred0, dependency);
			ir_node* cond;
			int j;

			if (projx0 == NULL) continue;

			cond = get_Proj_pred(projx0);
			if (! is_Cond(cond))
				continue;

			for (j = i + 1; j < arity; ++j) {
				ir_node* projx1;
				ir_node* sel;
				ir_node* mux_block;
				ir_node* phi;
				ir_node* p;
				ir_node* pred1;
				bool     supported;
				bool     negated;
				dbg_info* cond_dbg;

				pred1 = get_Block_cfgpred_block(block, j);
				if (pred1 == block) continue;

				if (!is_cdep_on(pred1, dependency)) continue;

				projx1 = walk_to_projx(pred1, dependency);

				if (projx1 == NULL) continue;

				sel = get_Cond_selector(cond);
				phi = get_Block_phis(block);
				supported = true;
				negated   = get_Proj_proj(projx0) == pn_Cond_false;
				for (p = phi; p != NULL; p = get_Phi_next(p)) {
					ir_node *mux_false;
					ir_node *mux_true;
					if (negated) {
						mux_true  = get_Phi_pred(p, j);
						mux_false = get_Phi_pred(p, i);
					} else {
						mux_true  = get_Phi_pred(p, i);
						mux_false = get_Phi_pred(p, j);
					}
					if (mux_true == mux_false)
						continue;
					ir_mode *mode = get_irn_mode(mux_true);
					if (mode == mode_M
						|| !env->allow_ifconv(sel, mux_false, mux_true)) {
						supported = false;
						break;
					}
				}
				if (!supported)
					continue;

				DB((dbg, LEVEL_1, "Found Cond %+F with proj %+F and %+F\n",
					cond, projx0, projx1
				));

				/* remove critical edges */
				env->changed = true;
				prepare_path(block, i, dependency);
				prepare_path(block, j, dependency);
				arity = get_irn_arity(block);

				mux_block = get_nodes_block(cond);
				cond_dbg = get_irn_dbg_info(cond);
				do { /* generate Mux nodes in mux_block for Phis in block */
					ir_node* val_i = get_irn_n(phi, i);
					ir_node* val_j = get_irn_n(phi, j);
					ir_node* mux;
					ir_node* next_phi;

					if (val_i == val_j) {
						mux = val_i;
						DB((dbg, LEVEL_2,  "Generating no Mux, because both values are equal\n"));
					} else {
						ir_node *t, *f;

						/* Something is very fishy if two predecessors of a PhiM point into
						 * one block, but not at the same memory node
						 */
						assert(get_irn_mode(phi) != mode_M);
						if (negated) {
							t = val_j;
							f = val_i;
						} else {
							t = val_i;
							f = val_j;
						}

						mux = new_rd_Mux(cond_dbg, mux_block, sel, f, t, get_irn_mode(phi));
						DB((dbg, LEVEL_2, "Generating %+F for %+F\n", mux, phi));
					}

					next_phi = get_Phi_next(phi);

					if (arity == 2) {
						exchange(phi, mux);
					} else {
						rewire(phi, i, j, mux);
					}
					phi = next_phi;
				} while (phi != NULL);

				/* move mux operands into mux_block */
				exchange(get_nodes_block(get_Block_cfgpred(block, i)), mux_block);
				exchange(get_nodes_block(get_Block_cfgpred(block, j)), mux_block);

				if (arity == 2) {
					unsigned mark;
					DB((dbg, LEVEL_1,  "Welding block %+F to %+F\n", block, mux_block));
					mark =  get_Block_mark(mux_block) | get_Block_mark(block);
					/* mark both block just to be sure, should be enough to mark mux_block */
					set_Block_mark(mux_block, mark);
					exchange(block, mux_block);
					return;
				} else {
					rewire(block, i, j, new_r_Jmp(mux_block));
					goto restart;
				}
			}
		}
	}
}

/**
 * Block walker: clear block marks and Phi lists.
 */
static void init_block_link(ir_node *block, void *env)
{
	(void)env;
	set_Block_mark(block, 0);
	set_Block_phis(block, NULL);
}


/**
 * Daisy-chain all Phis in a block.
 * If a non-movable node is encountered set the has_pinned flag in its block.
 */
static void collect_phis(ir_node *node, void *env)
{
	(void) env;

	if (is_Phi(node)) {
		ir_node *block = get_nodes_block(node);

		add_Block_phi(block, node);
	} else {
		if (!is_Block(node) && get_irn_pinned(node) == op_pin_state_pinned) {
			/*
			 * Ignore control flow nodes (except Raise), these will be removed.
			 */
			if (!is_cfop(node) && !is_Raise(node)) {
				ir_node *block = get_nodes_block(node);

				DB((dbg, LEVEL_2, "Node %+F in block %+F is unmovable\n", node, block));
				set_Block_mark(block, 1);
			}
		}
	}
}

void opt_if_conv(ir_graph *irg)
{
	walker_env            env;
	const backend_params *be_params = be_get_backend_param();

	assure_irg_properties(irg,
		IR_GRAPH_PROPERTY_NO_CRITICAL_EDGES
		| IR_GRAPH_PROPERTY_NO_UNREACHABLE_CODE
		| IR_GRAPH_PROPERTY_NO_BADS
		| IR_GRAPH_PROPERTY_ONE_RETURN);

	/* get the parameters */
	env.allow_ifconv = be_params->allow_ifconv;
	env.changed      = false;

	FIRM_DBG_REGISTER(dbg, "firm.opt.ifconv");

	DB((dbg, LEVEL_1, "Running if-conversion on %+F\n", irg));

	compute_cdep(irg);

	ir_reserve_resources(irg, IR_RESOURCE_BLOCK_MARK | IR_RESOURCE_PHI_LIST);

	irg_block_walk_graph(irg, init_block_link, NULL, NULL);
	irg_walk_graph(irg, collect_phis, NULL, NULL);
	irg_block_walk_graph(irg, NULL, if_conv_walker, &env);

	ir_free_resources(irg, IR_RESOURCE_BLOCK_MARK | IR_RESOURCE_PHI_LIST);

	if (env.changed) {
		local_optimize_graph(irg);
	}

	free_cdep(irg);

	confirm_irg_properties(irg,
		IR_GRAPH_PROPERTY_NO_CRITICAL_EDGES
		| IR_GRAPH_PROPERTY_ONE_RETURN);
}

ir_graph_pass_t *opt_if_conv_pass(const char *name)
{
	return def_graph_pass(name ? name : "ifconv", opt_if_conv);
}
