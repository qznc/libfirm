/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief       implementation of the spill/reload placement abstraction layer
 * @author      Daniel Grund, Sebastian Hack, Matthias Braun
 * @date        29.09.2005
 */
#include <stdlib.h>
#include <stdbool.h>

#include "irnode_t.h"
#include "ircons_t.h"
#include "iredges_t.h"
#include "irbackedge_t.h"
#include "ident_t.h"
#include "type_t.h"
#include "entity_t.h"
#include "debug.h"
#include "irgwalk.h"
#include "array.h"
#include "execfreq.h"
#include "error.h"
#include "bearch.h"
#include "belive_t.h"
#include "besched.h"
#include "bespill.h"
#include "bespillutil.h"
#include "belive_t.h"
#include "benode.h"
#include "bechordal_t.h"
#include "statev_t.h"
#include "bessaconstr.h"
#include "beirg.h"
#include "beirgmod.h"
#include "beintlive_t.h"
#include "bemodule.h"
#include "be_t.h"

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

#define REMAT_COST_INFINITE  1000

typedef struct reloader_t reloader_t;
struct reloader_t {
	reloader_t *next;
	ir_node    *reloader;
	ir_node    *rematted_node;
	int         remat_cost_delta; /** costs needed for rematerialization,
	                                   compared to placing a reload */
};

typedef struct spill_t spill_t;
struct spill_t {
	spill_t *next;
	ir_node *after;  /**< spill has to be placed after this node (or earlier) */
	ir_node *spill;
};

typedef struct spill_info_t spill_info_t;
struct spill_info_t {
	ir_node    *to_spill;  /**< the value that should get spilled */
	reloader_t *reloaders; /**< list of places where the value should get
	                            reloaded */
	spill_t    *spills;    /**< list of latest places where spill must be
	                            placed */
	double      spill_costs; /**< costs needed for spilling the value */
	const arch_register_class_t *reload_cls; /** the register class in which the
	                                             reload should be placed */
	bool        spilled_phi; /* true when the whole Phi has been spilled and
	                            will be replaced with a PhiM. false if only the
	                            value of the Phi gets spilled */
};

struct spill_env_t {
	const arch_env_t *arch_env;
	ir_graph         *irg;
	struct obstack    obst;
	int               spill_cost;     /**< the cost of a single spill node */
	int               reload_cost;    /**< the cost of a reload node */
	set              *spills;         /**< all spill_info_t's, which must be
	                                       placed */
	spill_info_t    **mem_phis;       /**< set of all spilled phis. */

	unsigned          spill_count;
	unsigned          reload_count;
	unsigned          remat_count;
	unsigned          spilled_phi_count;
};

/**
 * Compare two spill infos.
 */
static int cmp_spillinfo(const void *x, const void *y, size_t size)
{
	const spill_info_t *xx = (const spill_info_t*)x;
	const spill_info_t *yy = (const spill_info_t*)y;
	(void) size;

	return xx->to_spill != yy->to_spill;
}

/**
 * Returns spill info for a specific value (the value that is to be spilled)
 */
static spill_info_t *get_spillinfo(const spill_env_t *env, ir_node *value)
{
	spill_info_t info, *res;
	int hash = hash_irn(value);

	info.to_spill = value;
	res = set_find(spill_info_t, env->spills, &info, sizeof(info), hash);

	if (res == NULL) {
		info.reloaders   = NULL;
		info.spills      = NULL;
		info.spill_costs = -1;
		info.reload_cls  = NULL;
		info.spilled_phi = false;
		res = set_insert(spill_info_t, env->spills, &info, sizeof(info), hash);
	}

	return res;
}

spill_env_t *be_new_spill_env(ir_graph *irg)
{
	const arch_env_t *arch_env = be_get_irg_arch_env(irg);

	spill_env_t *env = XMALLOC(spill_env_t);
	env->spills         = new_set(cmp_spillinfo, 1024);
	env->irg            = irg;
	env->arch_env       = arch_env;
	env->mem_phis       = NEW_ARR_F(spill_info_t*, 0);
	env->spill_cost     = arch_env->spill_cost;
	env->reload_cost    = arch_env->reload_cost;
	obstack_init(&env->obst);

	env->spill_count       = 0;
	env->reload_count      = 0;
	env->remat_count       = 0;
	env->spilled_phi_count = 0;

	return env;
}

void be_delete_spill_env(spill_env_t *env)
{
	del_set(env->spills);
	DEL_ARR_F(env->mem_phis);
	obstack_free(&env->obst, NULL);
	free(env);
}

/*
 *  ____  _                  ____      _                 _
 * |  _ \| | __ _  ___ ___  |  _ \ ___| | ___   __ _  __| |___
 * | |_) | |/ _` |/ __/ _ \ | |_) / _ \ |/ _ \ / _` |/ _` / __|
 * |  __/| | (_| | (_|  __/ |  _ <  __/ | (_) | (_| | (_| \__ \
 * |_|   |_|\__,_|\___\___| |_| \_\___|_|\___/ \__,_|\__,_|___/
 *
 */

void be_add_spill(spill_env_t *env, ir_node *to_spill, ir_node *after)
{
	spill_info_t  *spill_info = get_spillinfo(env, to_spill);
	spill_t       *spill;
	spill_t       *s;
	spill_t       *last;

	assert(!arch_irn_is(skip_Proj_const(to_spill), dont_spill));
	DB((dbg, LEVEL_1, "Add spill of %+F after %+F\n", to_spill, after));

	/* Just for safety make sure that we do not insert the spill in front of a phi */
	assert(!is_Phi(sched_next(after)));

	/* spills that are dominated by others are not needed */
	last = NULL;
	s    = spill_info->spills;
	for ( ; s != NULL; s = s->next) {
		/* no need to add this spill if it is dominated by another */
		if (value_dominates(s->after, after)) {
			DB((dbg, LEVEL_1, "...dominated by %+F, not added\n", s->after));
			return;
		}
		/* remove spills that we dominate */
		if (value_dominates(after, s->after)) {
			DB((dbg, LEVEL_1, "...remove old spill at %+F\n", s->after));
			if (last != NULL) {
				last->next         = s->next;
			} else {
				spill_info->spills = s->next;
			}
		} else {
			last = s;
		}
	}

	spill         = OALLOC(&env->obst, spill_t);
	spill->after  = after;
	spill->next   = spill_info->spills;
	spill->spill  = NULL;

	spill_info->spills = spill;
}

void be_add_reload(spill_env_t *env, ir_node *to_spill, ir_node *before, const arch_register_class_t *reload_cls, int allow_remat)
{
	spill_info_t  *info;
	reloader_t    *rel;

	assert(!arch_irn_is(skip_Proj_const(to_spill), dont_spill));

	info = get_spillinfo(env, to_spill);

	if (is_Phi(to_spill)) {
		int i, arity;

		/* create spillinfos for the phi arguments */
		for (i = 0, arity = get_irn_arity(to_spill); i < arity; ++i) {
			ir_node *arg = get_irn_n(to_spill, i);
			get_spillinfo(env, arg);
		}
	}

	assert(!be_is_Keep(before));

	/* put reload into list */
	rel                   = OALLOC(&env->obst, reloader_t);
	rel->next             = info->reloaders;
	rel->reloader         = before;
	rel->rematted_node    = NULL;
	rel->remat_cost_delta = allow_remat ? 0 : REMAT_COST_INFINITE;

	info->reloaders  = rel;
	assert(info->reload_cls == NULL || info->reload_cls == reload_cls);
	info->reload_cls = reload_cls;

	DBG((dbg, LEVEL_1, "creating spillinfo for %+F, will be reloaded before %+F, may%s be rematerialized\n",
		to_spill, before, allow_remat ? "" : " not"));
}

ir_node *be_get_end_of_block_insertion_point(const ir_node *block)
{
	ir_node *last = sched_last(block);

	/* we might have keeps behind the jump... */
	while (be_is_Keep(last)) {
		last = sched_prev(last);
		assert(!sched_is_end(last));
	}

	assert(is_cfop(last));

	/* add the reload before the (cond-)jump */
	return last;
}

/**
 * determine final spill position: it should be after all phis, keep nodes
 * and behind nodes marked as prolog
 */
static ir_node *determine_spill_point(ir_node *node)
{
	node = skip_Proj(node);
	while (true) {
		ir_node *next = sched_next(node);
		if (!is_Phi(next) && !be_is_Keep(next) && !be_is_CopyKeep(next))
			break;
		node = next;
	}
	return node;
}

/**
 * Returns the point at which you can insert a node that should be executed
 * before block @p block when coming from pred @p pos.
 */
static ir_node *get_block_insertion_point(ir_node *block, int pos)
{
	ir_node *predblock;

	/* simply add the reload to the beginning of the block if we only have 1
	 * predecessor. We don't need to check for phis as there can't be any in a
	 * block with only 1 pred. */
	if (get_Block_n_cfgpreds(block) == 1) {
		assert(!is_Phi(sched_first(block)));
		return sched_first(block);
	}

	/* We have to reload the value in pred-block */
	predblock = get_Block_cfgpred_block(block, pos);
	return be_get_end_of_block_insertion_point(predblock);
}

void be_add_reload_on_edge(spill_env_t *env, ir_node *to_spill, ir_node *block,
                           int pos, const arch_register_class_t *reload_cls,
                           int allow_remat)
{
	ir_node *before = get_block_insertion_point(block, pos);
	be_add_reload(env, to_spill, before, reload_cls, allow_remat);
}

void be_spill_phi(spill_env_t *env, ir_node *node)
{
	ir_node *block;
	int i, arity;
	spill_info_t *info;

	assert(is_Phi(node));

	info              = get_spillinfo(env, node);
	info->spilled_phi = true;
	ARR_APP1(spill_info_t*, env->mem_phis, info);

	/* create spills for the phi arguments */
	block = get_nodes_block(node);
	for (i = 0, arity = get_irn_arity(node); i < arity; ++i) {
		ir_node *arg = get_irn_n(node, i);
		ir_node *insert;

		/* some backends have virtual noreg/unknown nodes that are not scheduled
		 * and simply always available. */
		if (!sched_is_scheduled(arg)) {
			ir_node *pred_block = get_Block_cfgpred_block(block, i);
			insert = be_get_end_of_block_insertion_point(pred_block);
			insert = sched_prev(insert);
		} else {
			insert = determine_spill_point(arg);
		}

		be_add_spill(env, arg, insert);
	}
}

/*
 *   ____                _         ____        _ _ _
 *  / ___|_ __ ___  __ _| |_ ___  / ___| _ __ (_) | |___
 * | |   | '__/ _ \/ _` | __/ _ \ \___ \| '_ \| | | / __|
 * | |___| | |  __/ (_| | ||  __/  ___) | |_) | | | \__ \
 *  \____|_|  \___|\__,_|\__\___| |____/| .__/|_|_|_|___/
 *                                      |_|
 */

static void determine_spill_costs(spill_env_t *env, spill_info_t *spillinfo);

/**
 * Creates a spill.
 *
 * @param senv      the spill environment
 * @param irn       the node that should be spilled
 * @param ctx_irn   an user of the spilled node
 *
 * @return a be_Spill node
 */
static void spill_irn(spill_env_t *env, spill_info_t *spillinfo)
{
	ir_node       *to_spill = spillinfo->to_spill;
	const ir_node *insn     = skip_Proj_const(to_spill);
	spill_t *spill;

	/* determine_spill_costs must have been run before */
	assert(spillinfo->spill_costs >= 0);

	/* some backends have virtual noreg/unknown nodes that are not scheduled
	 * and simply always available. */
	if (!sched_is_scheduled(insn)) {
		/* override spillinfos or create a new one */
		ir_graph *irg = get_irn_irg(to_spill);
		spillinfo->spills->spill = get_irg_no_mem(irg);
		DB((dbg, LEVEL_1, "don't spill %+F use NoMem\n", to_spill));
		return;
	}

	DBG((dbg, LEVEL_1, "spilling %+F ... \n", to_spill));
	spill = spillinfo->spills;
	for ( ; spill != NULL; spill = spill->next) {
		ir_node *after = spill->after;
		after = determine_spill_point(after);

		spill->spill = arch_env_new_spill(env->arch_env, to_spill, after);
		DB((dbg, LEVEL_1, "\t%+F after %+F\n", spill->spill, after));
		env->spill_count++;
	}
	DBG((dbg, LEVEL_1, "\n"));
}

static void spill_node(spill_env_t *env, spill_info_t *spillinfo);

/**
 * If the first usage of a Phi result would be out of memory
 * there is no sense in allocating a register for it.
 * Thus we spill it and all its operands to the same spill slot.
 * Therefore the phi/dataB becomes a phi/Memory
 *
 * @param senv      the spill environment
 * @param phi       the Phi node that should be spilled
 * @param ctx_irn   an user of the spilled node
 */
static void spill_phi(spill_env_t *env, spill_info_t *spillinfo)
{
	ir_graph *irg   = env->irg;
	ir_node  *phi   = spillinfo->to_spill;
	ir_node  *block = get_nodes_block(phi);
	spill_t  *spill;
	int       i;

	assert(!get_opt_cse());
	DBG((dbg, LEVEL_1, "spilling Phi %+F:\n", phi));

	/* build a new PhiM */
	int       const arity   = get_Phi_n_preds(phi);
	ir_node **const ins     = ALLOCAN(ir_node*, arity);
	ir_node  *const unknown = new_r_Unknown(irg, mode_M);
	for (i = 0; i < arity; ++i) {
		ins[i] = unknown;
	}

	/* override or replace spills list... */
	spill         = OALLOC(&env->obst, spill_t);
	spill->after  = determine_spill_point(phi);
	spill->spill  = be_new_Phi(block, arity, ins, mode_M, arch_no_register_req);
	spill->next   = NULL;
	sched_add_after(block, spill->spill);

	spillinfo->spills = spill;
	env->spilled_phi_count++;

	for (i = 0; i < arity; ++i) {
		ir_node      *arg      = get_irn_n(phi, i);
		spill_info_t *arg_info = get_spillinfo(env, arg);

		determine_spill_costs(env, arg_info);
		spill_node(env, arg_info);

		set_irn_n(spill->spill, i, arg_info->spills->spill);
	}
	DBG((dbg, LEVEL_1, "... done spilling Phi %+F, created PhiM %+F\n", phi,
	     spill->spill));
}

/**
 * Spill a node.
 *
 * @param senv      the spill environment
 * @param to_spill  the node that should be spilled
 */
static void spill_node(spill_env_t *env, spill_info_t *spillinfo)
{
	/* node is already spilled */
	if (spillinfo->spills != NULL && spillinfo->spills->spill != NULL)
		return;

	if (spillinfo->spilled_phi) {
		spill_phi(env, spillinfo);
	} else {
		spill_irn(env, spillinfo);
	}
}

/*
 *
 *  ____                      _            _       _ _
 * |  _ \ ___ _ __ ___   __ _| |_ ___ _ __(_) __ _| (_)_______
 * | |_) / _ \ '_ ` _ \ / _` | __/ _ \ '__| |/ _` | | |_  / _ \
 * |  _ <  __/ | | | | | (_| | ||  __/ |  | | (_| | | |/ /  __/
 * |_| \_\___|_| |_| |_|\__,_|\__\___|_|  |_|\__,_|_|_/___\___|
 *
 */

/**
 * Tests whether value @p arg is available before node @p reloader
 * @returns 1 if value is available, 0 otherwise
 */
static int is_value_available(spill_env_t *env, const ir_node *arg,
                              const ir_node *reloader)
{
	if (is_Unknown(arg) || is_NoMem(arg))
		return 1;

	if (arch_irn_is(skip_Proj_const(arg), spill))
		return 1;

	if (arg == get_irg_frame(env->irg))
		return 1;

	(void)reloader;

	if (get_irn_mode(arg) == mode_T)
		return 0;

	/*
	 * Ignore registers are always available
	 */
	if (arch_irn_is_ignore(arg))
		return 1;

	return 0;
}

/**
 * Check if a node is rematerializable. This tests for the following conditions:
 *
 * - The node itself is rematerializable
 * - All arguments of the node are available or also rematerialisable
 * - The costs for the rematerialisation operation is less or equal a limit
 *
 * Returns the costs needed for rematerialisation or something
 * >= REMAT_COST_INFINITE if remat is not possible.
 */
static int check_remat_conditions_costs(spill_env_t *env,
		const ir_node *spilled, const ir_node *reloader, int parentcosts)
{
	int i, arity;
	int argremats;
	int costs = 0;
	const ir_node *insn = skip_Proj_const(spilled);

	assert(!be_is_Spill(insn));
	if (!arch_irn_is(insn, rematerializable))
		return REMAT_COST_INFINITE;

	if (be_is_Reload(insn)) {
		costs += 2;
	} else {
		costs += arch_get_op_estimated_cost(insn);
	}
	if (parentcosts + costs >= env->reload_cost + env->spill_cost) {
		return REMAT_COST_INFINITE;
	}
	/* never rematerialize a node which modifies the flags.
	 * (would be better to test whether the flags are actually live at point
	 * reloader...)
	 */
	if (arch_irn_is(insn, modify_flags)) {
		return REMAT_COST_INFINITE;
	}

	argremats = 0;
	for (i = 0, arity = get_irn_arity(insn); i < arity; ++i) {
		ir_node *arg = get_irn_n(insn, i);

		if (is_value_available(env, arg, reloader))
			continue;

		/* we have to rematerialize the argument as well */
		++argremats;
		if (argremats > 1) {
			/* we only support rematerializing 1 argument at the moment,
			 * as multiple arguments could increase register pressure */
			return REMAT_COST_INFINITE;
		}

		costs += check_remat_conditions_costs(env, arg, reloader,
		                                      parentcosts + costs);
		if (parentcosts + costs >= env->reload_cost + env->spill_cost)
			return REMAT_COST_INFINITE;
	}

	return costs;
}

/**
 * Re-materialize a node.
 *
 * @param env       the spill environment
 * @param spilled   the node that was spilled
 * @param reloader  a irn that requires a reload
 */
static ir_node *do_remat(spill_env_t *env, ir_node *spilled, ir_node *reloader)
{
	int i, arity;
	ir_node *res;
	ir_node **ins;

	ins = ALLOCAN(ir_node*, get_irn_arity(spilled));
	for (i = 0, arity = get_irn_arity(spilled); i < arity; ++i) {
		ir_node *arg = get_irn_n(spilled, i);

		if (is_value_available(env, arg, reloader)) {
			ins[i] = arg;
		} else {
			ins[i] = do_remat(env, arg, reloader);
			/* don't count the argument rematerialization as an extra remat */
			--env->remat_count;
		}
	}

	/* create a copy of the node */
	ir_node *const bl = get_nodes_block(reloader);
	res = new_ir_node(get_irn_dbg_info(spilled), env->irg, bl,
	                  get_irn_op(spilled), get_irn_mode(spilled),
	                  get_irn_arity(spilled), ins);
	copy_node_attr(env->irg, spilled, res);
	arch_env_mark_remat(env->arch_env, res);

	DBG((dbg, LEVEL_1, "Insert remat %+F of %+F before reloader %+F\n", res, spilled, reloader));

	if (! is_Proj(res)) {
		/* insert in schedule */
		sched_reset(res);
		sched_add_before(reloader, res);
		++env->remat_count;
	}

	return res;
}

double be_get_spill_costs(spill_env_t *env, ir_node *to_spill, ir_node *before)
{
	ir_node *block = get_nodes_block(before);
	double   freq  = get_block_execfreq(block);
	(void) to_spill;

	return env->spill_cost * freq;
}

unsigned be_get_reload_costs_no_weight(spill_env_t *env, const ir_node *to_spill,
                                       const ir_node *before)
{
	if (be_do_remats) {
		/* is the node rematerializable? */
		unsigned costs = check_remat_conditions_costs(env, to_spill, before, 0);
		if (costs < (unsigned) env->reload_cost)
			return costs;
	}

	return env->reload_cost;
}

double be_get_reload_costs(spill_env_t *env, ir_node *to_spill, ir_node *before)
{
	ir_node *block = get_nodes_block(before);
	double   freq  = get_block_execfreq(block);

	if (be_do_remats) {
		/* is the node rematerializable? */
		int costs = check_remat_conditions_costs(env, to_spill, before, 0);
		if (costs < env->reload_cost)
			return costs * freq;
	}

	return env->reload_cost * freq;
}

int be_is_rematerializable(spill_env_t *env, const ir_node *to_remat,
                           const ir_node *before)
{
	return check_remat_conditions_costs(env, to_remat, before, 0) < REMAT_COST_INFINITE;
}

double be_get_reload_costs_on_edge(spill_env_t *env, ir_node *to_spill,
                                   ir_node *block, int pos)
{
	ir_node *before = get_block_insertion_point(block, pos);
	return be_get_reload_costs(env, to_spill, before);
}

ir_node *be_new_spill(ir_node *value, ir_node *after)
{
	ir_graph                    *irg       = get_irn_irg(value);
	ir_node                     *frame     = get_irg_frame(irg);
	const arch_register_class_t *cls       = arch_get_irn_reg_class(value);
	const arch_register_class_t *cls_frame = arch_get_irn_reg_class(frame);
	ir_node                     *block     = get_block(after);
	ir_node                     *spill
		= be_new_Spill(cls, cls_frame, block, frame, value);

	sched_add_after(after, spill);
	return spill;
}

ir_node *be_new_reload(ir_node *value, ir_node *spill, ir_node *before)
{
	ir_graph *irg   = get_irn_irg(value);
	ir_node  *frame = get_irg_frame(irg);
	ir_node  *block = get_block(before);
	const arch_register_class_t *cls       = arch_get_irn_reg_class(value);
	const arch_register_class_t *cls_frame = arch_get_irn_reg_class(frame);
	ir_mode                     *mode      = get_irn_mode(value);
	ir_node  *reload;

	assert(be_is_Spill(spill) || is_Phi(spill));
	assert(get_irn_mode(spill) == mode_M);

	reload = be_new_Reload(cls, cls_frame, block, frame, spill, mode);
	sched_add_before(before, reload);

	return reload;
}

/*
 *  ___                     _     ____      _                 _
 * |_ _|_ __  ___  ___ _ __| |_  |  _ \ ___| | ___   __ _  __| |___
 *  | || '_ \/ __|/ _ \ '__| __| | |_) / _ \ |/ _ \ / _` |/ _` / __|
 *  | || | | \__ \  __/ |  | |_  |  _ <  __/ | (_) | (_| | (_| \__ \
 * |___|_| |_|___/\___|_|   \__| |_| \_\___|_|\___/ \__,_|\__,_|___/
 *
 */

/**
 * analyzes how to best spill a node and determine costs for that
 */
static void determine_spill_costs(spill_env_t *env, spill_info_t *spillinfo)
{
	ir_node       *to_spill = spillinfo->to_spill;
	const ir_node *insn     = skip_Proj_const(to_spill);
	ir_node       *spill_block;
	spill_t       *spill;
	double         spill_execfreq;

	/* already calculated? */
	if (spillinfo->spill_costs >= 0)
		return;

	assert(!arch_irn_is(insn, dont_spill));
	assert(!be_is_Reload(insn));

	/* some backends have virtual noreg/unknown nodes that are not scheduled
	 * and simply always available.
	 * TODO: this is kinda hairy, the NoMem is correct for an Unknown as Phi
	 * predecessor (of a PhiM) but this test might match other things too...
	 */
	if (!sched_is_scheduled(insn)) {
		ir_graph *irg = get_irn_irg(to_spill);
		/* override spillinfos or create a new one */
		spill_t *spill = OALLOC(&env->obst, spill_t);
		spill->after = NULL;
		spill->next  = NULL;
		spill->spill = get_irg_no_mem(irg);

		spillinfo->spills      = spill;
		spillinfo->spill_costs = 0;

		DB((dbg, LEVEL_1, "don't spill %+F use NoMem\n", to_spill));
		return;
	}

	spill_block    = get_nodes_block(insn);
	spill_execfreq = get_block_execfreq(spill_block);

	if (spillinfo->spilled_phi) {
		/* TODO calculate correct costs...
		 * (though we can't remat this node anyway so no big problem) */
		spillinfo->spill_costs = env->spill_cost * spill_execfreq;
		return;
	}

	if (spillinfo->spills != NULL) {
		spill_t *s;
		double   spills_execfreq;

		/* calculate sum of execution frequencies of individual spills */
		spills_execfreq = 0;
		s               = spillinfo->spills;
		for ( ; s != NULL; s = s->next) {
			ir_node *spill_block = get_block(s->after);
			double   freq = get_block_execfreq(spill_block);

			spills_execfreq += freq;
		}

		DB((dbg, LEVEL_1, "%+F: latespillcosts %f after def: %f\n", to_spill,
		    spills_execfreq * env->spill_cost,
		    spill_execfreq * env->spill_cost));

		/* multi-/latespill is advantageous -> return*/
		if (spills_execfreq < spill_execfreq) {
			DB((dbg, LEVEL_1, "use latespills for %+F\n", to_spill));
			spillinfo->spill_costs = spills_execfreq * env->spill_cost;
			return;
		}
	}

	/* override spillinfos or create a new one */
	spill        = OALLOC(&env->obst, spill_t);
	spill->after = determine_spill_point(to_spill);
	spill->next  = NULL;
	spill->spill = NULL;

	spillinfo->spills      = spill;
	spillinfo->spill_costs = spill_execfreq * env->spill_cost;
	DB((dbg, LEVEL_1, "spill %+F after definition\n", to_spill));
}

void make_spill_locations_dominate_irn(spill_env_t *env, ir_node *irn)
{
	const spill_info_t *si = get_spillinfo(env, irn);
	ir_node *start_block   = get_irg_start_block(get_irn_irg(irn));
	int n_blocks           = get_Block_dom_max_subtree_pre_num(start_block);
	bitset_t *reloads      = bitset_alloca(n_blocks);
	reloader_t *r;
	spill_t *s;

	if (si == NULL)
		return;

	/* Fill the bitset with the dominance pre-order numbers
	 * of the blocks the reloads are located in. */
	for (r = si->reloaders; r != NULL; r = r->next) {
		ir_node *bl = get_nodes_block(r->reloader);
		bitset_set(reloads, get_Block_dom_tree_pre_num(bl));
	}

	/* Now, cancel out all the blocks that are dominated by each spill.
	 * If the bitset is not empty after that, we have reloads that are
	 * not dominated by any spill. */
	for (s = si->spills; s != NULL; s = s->next) {
		ir_node *bl = get_nodes_block(s->after);
		int start   = get_Block_dom_tree_pre_num(bl);
		int end     = get_Block_dom_max_subtree_pre_num(bl);

		bitset_clear_range(reloads, start, end);
	}

	if (!bitset_is_empty(reloads))
		be_add_spill(env, si->to_spill, si->to_spill);
}

void be_insert_spills_reloads(spill_env_t *env)
{
	size_t n_mem_phis = ARR_LEN(env->mem_phis);
	size_t i;

	be_timer_push(T_RA_SPILL_APPLY);

	/* create all phi-ms first, this is needed so, that phis, hanging on
	   spilled phis work correctly */
	for (i = 0; i < n_mem_phis; ++i) {
		spill_info_t *info = env->mem_phis[i];
		spill_node(env, info);
	}

	/* process each spilled node */
	foreach_set(env->spills, spill_info_t, si) {
		ir_node  *to_spill        = si->to_spill;
		ir_node **copies          = NEW_ARR_F(ir_node*, 0);
		double    all_remat_costs = 0; /** costs when we would remat all nodes */
		bool      force_remat     = false;
		reloader_t *rld;

		DBG((dbg, LEVEL_1, "\nhandling all reloaders of %+F:\n", to_spill));

		determine_spill_costs(env, si);

		/* determine possibility of rematerialisations */
		if (be_do_remats) {
			/* calculate cost savings for each indivial value when it would
			   be rematted instead of reloaded */
			for (rld = si->reloaders; rld != NULL; rld = rld->next) {
				double   freq;
				int      remat_cost;
				int      remat_cost_delta;
				ir_node *block;
				ir_node *reloader = rld->reloader;

				if (rld->rematted_node != NULL) {
					DBG((dbg, LEVEL_2, "\tforced remat %+F before %+F\n",
					     rld->rematted_node, reloader));
					continue;
				}
				if (rld->remat_cost_delta >= REMAT_COST_INFINITE) {
					DBG((dbg, LEVEL_2, "\treload before %+F is forbidden\n",
					     reloader));
					all_remat_costs = REMAT_COST_INFINITE;
					continue;
				}

				remat_cost  = check_remat_conditions_costs(env, to_spill,
				                                           reloader, 0);
				if (remat_cost >= REMAT_COST_INFINITE) {
					DBG((dbg, LEVEL_2, "\tremat before %+F not possible\n",
					     reloader));
					rld->remat_cost_delta = REMAT_COST_INFINITE;
					all_remat_costs       = REMAT_COST_INFINITE;
					continue;
				}

				remat_cost_delta      = remat_cost - env->reload_cost;
				rld->remat_cost_delta = remat_cost_delta;
				block                 = is_Block(reloader) ? reloader : get_nodes_block(reloader);
				freq                  = get_block_execfreq(block);
				all_remat_costs      += remat_cost_delta * freq;
				DBG((dbg, LEVEL_2, "\tremat costs delta before %+F: "
				     "%d (rel %f)\n", reloader, remat_cost_delta,
				     remat_cost_delta * freq));
			}
			if (all_remat_costs < REMAT_COST_INFINITE) {
				/* we don't need the costs for the spill if we can remat
				   all reloaders */
				all_remat_costs -= si->spill_costs;

				DBG((dbg, LEVEL_2, "\tspill costs %d (rel %f)\n",
				     env->spill_cost, si->spill_costs));
			}

			if (all_remat_costs < 0) {
				DBG((dbg, LEVEL_1, "\nforcing remats of all reloaders (%f)\n",
				     all_remat_costs));
				force_remat = true;
			}
		}

		/* go through all reloads for this spill */
		for (rld = si->reloaders; rld != NULL; rld = rld->next) {
			ir_node *copy; /* a reload is a "copy" of the original value */

			if (rld->rematted_node != NULL) {
				copy = rld->rematted_node;
				sched_add_before(rld->reloader, copy);
			} else if (be_do_remats &&
					(force_remat || rld->remat_cost_delta < 0)) {
				copy = do_remat(env, to_spill, rld->reloader);
			} else {
				/* make sure we have a spill */
				spill_node(env, si);

				/* create a reload, use the first spill for now SSA
				 * reconstruction for memory comes below */
				assert(si->spills != NULL);
				copy = arch_env_new_reload(env->arch_env, si->to_spill,
				                           si->spills->spill, rld->reloader);
				env->reload_count++;
			}

			DBG((dbg, LEVEL_1, " %+F of %+F before %+F\n",
			     copy, to_spill, rld->reloader));
			ARR_APP1(ir_node*, copies, copy);
		}

		/* if we had any reloads or remats, then we need to reconstruct the
		 * SSA form for the spilled value */
		if (ARR_LEN(copies) > 0) {
			be_ssa_construction_env_t senv;
			/* be_lv_t *lv = be_get_irg_liveness(env->irg); */

			be_ssa_construction_init(&senv, env->irg);
			be_ssa_construction_add_copy(&senv, to_spill);
			be_ssa_construction_add_copies(&senv, copies, ARR_LEN(copies));
			be_ssa_construction_fix_users(&senv, to_spill);

			be_ssa_construction_destroy(&senv);
		}
		/* need to reconstruct SSA form if we had multiple spills */
		if (si->spills != NULL && si->spills->next != NULL) {
			spill_t *spill;
			int      spill_count = 0;

			be_ssa_construction_env_t senv;

			be_ssa_construction_init(&senv, env->irg);
			spill = si->spills;
			for ( ; spill != NULL; spill = spill->next) {
				/* maybe we rematerialized the value and need no spill */
				if (spill->spill == NULL)
					continue;
				be_ssa_construction_add_copy(&senv, spill->spill);
				spill_count++;
			}
			if (spill_count > 1) {
				/* all reloads are attached to the first spill, fix them now */
				be_ssa_construction_fix_users(&senv, si->spills->spill);
			}

			be_ssa_construction_destroy(&senv);
		}

		DEL_ARR_F(copies);
		si->reloaders = NULL;
	}

	stat_ev_dbl("spill_spills", env->spill_count);
	stat_ev_dbl("spill_reloads", env->reload_count);
	stat_ev_dbl("spill_remats", env->remat_count);
	stat_ev_dbl("spill_spilled_phis", env->spilled_phi_count);

	/* Matze: In theory be_ssa_construction should take care of the liveness...
	 * try to disable this again in the future */
	be_invalidate_live_sets(env->irg);

	be_remove_dead_nodes_from_schedule(env->irg);

	be_timer_pop(T_RA_SPILL_APPLY);
}

BE_REGISTER_MODULE_CONSTRUCTOR(be_init_spill)
void be_init_spill(void)
{
	FIRM_DBG_REGISTER(dbg, "firm.be.spill");
}
