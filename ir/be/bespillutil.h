/*
 * Copyright (C) 1995-2008 University of Karlsruhe.  All right reserved.
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
 * @brief       higher level abstraction for the creation of spill and reload
 *              instructions and rematerialisation of values.
 * @author      Daniel Grund, Sebastian Hack, Matthias Braun
 * @date		29.09.2005
 * @version     $Id$
 */
#ifndef FIRM_BE_BESPILLUTIL_H
#define FIRM_BE_BESPILLUTIL_H

#include "firm_types.h"
#include "debug.h"

#include "bearch.h"
#include "beirg.h"

typedef struct spill_env_t spill_env_t;

/**
 * Creates a new spill environment.
 */
spill_env_t *be_new_spill_env(be_irg_t *birg);

/**
 * Deletes a spill environment.
 */
void be_delete_spill_env(spill_env_t *senv);

/**
 * Return the last control flow node of a block.
 */
ir_node *be_get_end_of_block_insertion_point(const ir_node *block);

/**
 * Marks a point until which a node must be spilled.
 */
void be_add_spill(spill_env_t *senv, ir_node *to_spill, ir_node *after);

/**
 * Inserts a new entry into the list of reloads to place (the real nodes will
 * be created when be_insert_spills_reloads is run). You don't have to
 * explicitly create spill nodes, they will be created automatically after
 * the definition of a value as soon as a reload is created. (we should add a
 * possibility for explicit spill placement in the future)
 *
 * @param senv        The spill environment
 * @param to_spill    The node which is about to be spilled
 * @param before      The node before the reload should be added
 * @param reload_cls  The register class the reloaded value will be put into
 * @param allow_remat Set to 1 if the node may be rematerialized instead of
 *                    reloaded
 */
void be_add_reload(spill_env_t *senv, ir_node *to_spill, ir_node *before,
                   const arch_register_class_t *reload_cls, int allow_remat);

void be_add_reload2(spill_env_t *senv, ir_node *to_spill, ir_node *before, ir_node *can_spill_after,
                   const arch_register_class_t *reload_cls, int allow_remat);

/**
 * Add a reload at the end of a block.
 * Similar to be_add_reload_on_edge().
 */
void be_add_reload_at_end(spill_env_t *env, ir_node *to_spill, const ir_node *block,
                          const arch_register_class_t *reload_cls,
                          int allow_remat);

/**
 * Analog to be_add_reload, but places the reload "on an edge" between 2 blocks
 * @see be_add_reload
 */
void be_add_reload_on_edge(spill_env_t *senv, ir_node *to_spill, ir_node *bl,
                           int pos, const arch_register_class_t *reload_cls,
                           int allow_remat);

/**
 * Analog to be_add_reload but adds an already created rematerialized node.
 */
void be_add_remat(spill_env_t *env, ir_node *to_spill, ir_node *before,
                  ir_node *rematted_node);

/**
 * The main function that places real spills/reloads (or rematerializes values)
 * for all values where be_add_reload was called. It then rebuilds the
 * SSA-form and updates liveness information
 */
void be_insert_spills_reloads(spill_env_t *senv);

/**
 * There are 2 possibilities to spill a phi node: Only it's value, or replacing
 * the whole phi-node with a memory phi. Normally only the value of a phi will
 * be spilled unless you mark the phi with be_spill_phi.
 * (Remember that each phi needs a register, so you have to spill phis when
 *  there are more phis than registers in a block)
 */
void be_spill_phi(spill_env_t *env, ir_node *node);

/**
 * Returns the estimated costs if a node would ge spilled. This does only return
 * the costs for the spill instructions, not the costs for needed reload
 * instructions. The value is weighted by the estimated execution frequency of
 * the spill.
 */
double be_get_spill_costs(spill_env_t *env, ir_node *to_spill, ir_node *before);

/**
 * Returns the estimated costs if a node would get reloaded at a specific place
 * This returns the costs for a reload instructions, or when possible the costs
 * for a rematerialisation. The value is weighted by the estimated execution
 * frequency of the reload/rematerialisation.
 */
double be_get_reload_costs(spill_env_t *env, ir_node *to_spill,
                           ir_node *before);

unsigned be_get_reload_costs_no_weight(spill_env_t *env, const ir_node *to_spill,
                                       const ir_node *before);


/**
 * Analog to be_get_reload_costs but returns the cost if the reload would be
 * placed "on an edge" between 2 blocks
 */
double be_get_reload_costs_on_edge(spill_env_t *env, ir_node *to_spill,
                                   ir_node *block, int pos);

typedef struct {
	unsigned n_spills;
	unsigned n_reloads;
	double spill_costs;
	double reload_costs;
} be_total_spill_costs_t;

/**
 * Insert a spill after the definition of the given node if there is a reload that is not dominated by some spill.
 * This function checks whether there is a reload that is not dominated by some spill for that node.
 * If so, it inserts a spill right after the definition of the node.
 * @param env The spill environment.
 * @param irn The node to check for.
 */
void make_spill_locations_dominate_irn(spill_env_t *env, ir_node *irn);

/**
 * Collect spill/reload cost statistics for a graph.
 * @param birg   The backend graph.
 * @param costs  A struct which will be filled with the costs.
 */
void be_get_total_spill_costs(be_irg_t *birg, be_total_spill_costs_t *costs);

/**
 * Check, if a node is rematerializable.
 * @param env  The spill env.

 */
int be_is_rematerializable(spill_env_t *env, const ir_node *to_remat, const ir_node *before);

#endif