/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief   Procedure cloning.
 * @author  Beyhan Veliev, Michael Beck
 * @brief
 *
 * The purpose is first to find and analyze functions, that are called
 * with constant parameter(s).
 * The second step is to optimize the function that are found from our
 * analyze. Optimize mean to make a new function with parameters, that
 * aren't be constant. The constant parameters of the function are placed
 * in the function graph. They aren't be passed as parameters.
 */
#include "iroptimize.h"
#include "tv.h"
#include "set.h"
#include "irprog_t.h"
#include "hashptr.h"
#include "irgwalk.h"
#include "analyze_irg_args.h"
#include "irprintf.h"
#include "ircons.h"
#include "irouts.h"
#include "irnode_t.h"
#include "irtools.h"
#include "irgmod.h"
#include "array_t.h"
#include "irpass_t.h"

/**
 * This struct contains the information quadruple for a Call, which we need to
 * decide if this function must be cloned.
 */
typedef struct quadruple {
	ir_entity *ent;     /**< The entity of our Call. */
	size_t    pos;      /**< Position of a constant argument of our Call. */
	ir_tarval *tv;      /**< The tarval of this argument if Const node. */
	ir_node   **calls;  /**< The list of all calls with the same characteristics */
} quadruple_t;

/**
 * The quadruplets are hold in a sorted list
 */
typedef struct entry {
	quadruple_t  q;      /**< the quadruple */
	float        weight; /**< its weight */
	struct entry *next;  /**< link to the next one */
} entry_t;

typedef struct q_set {
	struct obstack obst;        /**< an obstack containing all entries */
	pset           *map;        /**< a hash map containing the quadruples */
	entry_t        *heavy_uses; /**< the ordered list of heavy uses */
} q_set;

/**
 * Compare two quadruplets.
 *
 * @return zero if they are identically, non-zero else
 */
static int entry_cmp(const void *elt, const void *key)
{
	const entry_t *e1 = (const entry_t*)elt;
	const entry_t *e2 = (const entry_t*)key;

	return (e1->q.ent != e2->q.ent) || (e1->q.pos != e2->q.pos) || (e1->q.tv != e2->q.tv);
}

/**
 * Hash an element of type entry_t.
 *
 * @param entry  The element to be hashed.
 */
static unsigned hash_entry(const entry_t *entry)
{
	return hash_ptr(entry->q.ent) ^ hash_ptr(entry->q.tv) ^ (unsigned)(entry->q.pos * 9);
}

/**
 * Free memory associated with a quadruplet.
 */
static void kill_entry(entry_t *entry)
{
	if (entry->q.calls) {
		DEL_ARR_F(entry->q.calls);
		entry->q.calls = NULL;
	}
}

/**
 * Process a call node.
 *
 * @param call    A ir_node to be checked.
 * @param callee  The entity of the callee
 * @param hmap    The quadruple-set containing the calls with constant parameters
 */
static void process_call(ir_node *call, ir_entity *callee, q_set *hmap)
{
	entry_t *key, *entry;
	ir_node *call_param;
	size_t i, n_params;

	n_params = get_Call_n_params(call);

	/* TODO
	 * Beware: we cannot clone variadic parameters as well as the
	 * last non-variadic one, which might be needed for the va_start()
	 * magic
	 */

	/* In this for loop we collect the calls, that have
	   an constant parameter. */
	for (i = n_params; i > 0;) {
		call_param = get_Call_param(call, --i);
		if (is_Const(call_param)) {
			/* we have found a Call to collect and we save the informations,
			   which we need.*/
			if (! hmap->map)
				hmap->map = new_pset(entry_cmp, 8);

			key = OALLOC(&hmap->obst, entry_t);

			key->q.ent   = callee;
			key->q.pos   = i;
			key->q.tv    = get_Const_tarval(call_param);
			key->q.calls = NULL;
			key->weight  = 0.0F;
			key->next    = NULL;

			/* We insert our information in the set, where we collect the calls.*/
			entry = (entry_t*)pset_insert(hmap->map, key, hash_entry(key));

			if (entry != key)
				obstack_free(&hmap->obst, key);

			/* add the call to the list */
			if (! entry->q.calls) {
				entry->q.calls = NEW_ARR_F(ir_node *, 1);
				entry->q.calls[0] = call;
			} else
				ARR_APP1(ir_node *, entry->q.calls, call);
		}
	}
}

/**
 * Collect all calls in a ir_graph to a set.
 *
 * @param call   A ir_node to be checked.
 * @param env   The quadruple-set containing the calls with constant parameters
 */
static void collect_irg_calls(ir_node *call, void *env)
{
	q_set *hmap = (q_set*)env;
	ir_node *call_ptr;
	ir_entity *callee;

	/* We collect just "Call" nodes */
	if (is_Call(call)) {
		call_ptr = get_Call_ptr(call);

		if (! is_SymConst_addr_ent(call_ptr))
			return;

		callee = get_SymConst_entity(call_ptr);

		/* we don't know which function gets finally bound to a weak symbol */
		if (get_entity_linkage(callee) & IR_LINKAGE_WEAK)
			return;

		/* we can only clone calls to existing entities */
		if (get_entity_irg(callee) == NULL)
			return;

		process_call(call, callee, hmap);
	}
}

/**
 * Make a name for a clone. The clone name is
 * the name of the original method suffixed with "_cl_pos_nr".
 * pos is the pos from our quadruplet and nr is a counter.
 *
 * @param id  The ident of the cloned function.
 * @param pos The "pos" from our quadruplet.
 * @param nr  A counter for the clones.
 */
static ident *get_clone_ident(ident *id, size_t pos, size_t nr)
{
	char clone_postfix[32];

	ir_snprintf(clone_postfix, sizeof(clone_postfix), "_cl_%zu_%zu", pos, nr);

	return id_mangle(id, new_id_from_str(clone_postfix));
}

/**
 * Pre-Walker: Copies blocks and nodes from the original method graph
 * to the cloned graph. Fixes the argument projection numbers for
 * all arguments behind the removed one.
 *
 * @param irn  A node from the original method graph.
 * @param env  The clone graph.
 */
static void copy_nodes(ir_node *irn, void *env)
{
	ir_graph *clone_irg = (ir_graph*)env;
	ir_node  *arg       = (ir_node*)get_irg_link(clone_irg);
	ir_node  *irg_args  = get_Proj_pred(arg);
	ir_node  *irn_copy;
	long      proj_nr;

	/* Copy all nodes except the arg. */
	if (irn != arg)
		copy_irn_to_irg(irn, clone_irg);

	irn_copy = (ir_node*)get_irn_link(irn);

	/* Fix argument numbers */
	if (is_Proj(irn) && get_Proj_pred(irn) == irg_args) {
		proj_nr = get_Proj_proj(irn);
		if (get_Proj_proj(arg) < proj_nr)
			set_Proj_proj(irn_copy, proj_nr - 1);
	}
}

/**
 * Post-walker: Set the predecessors of the copied nodes.
 * The copied nodes are set as link of their original nodes. The links of
 * "irn" predecessors are the predecessors of copied node.
 */
static void set_preds(ir_node *irn, void *env)
{
	ir_graph *clone_irg = (ir_graph*)env;
	ir_node  *arg       = (ir_node*)get_irg_link(clone_irg);
	int       i;
	ir_node  *irn_copy;
	ir_node  *pred;

	/* Arg is the method argument, that we have replaced by a constant.*/
	if (arg == irn)
		return;

	irn_copy = (ir_node*)get_irn_link(irn);

	if (is_Block(irn)) {
		ir_graph *const irg = get_Block_irg(irn);
		for (i = get_Block_n_cfgpreds(irn) - 1; i >= 0; --i) {
			pred = get_Block_cfgpred(irn, i);
			/* "End" block must be handled extra, because it is not matured.*/
			if (get_irg_end_block(irg) == irn)
				add_immBlock_pred(get_irg_end_block(clone_irg), (ir_node*)get_irn_link(pred));
			else
				set_Block_cfgpred(irn_copy, i, (ir_node*)get_irn_link(pred));
		}
	} else {
		/* First we set the block our copy if it is not a block.*/
		set_nodes_block(irn_copy, (ir_node*)get_irn_link(get_nodes_block(irn)));
		if (is_End(irn)) {
			/* Handle the keep-alives. This must be done separately, because
			   the End node was NOT copied */
			for (i = 0; i < get_End_n_keepalives(irn); ++i)
				add_End_keepalive(irn_copy, (ir_node*)get_irn_link(get_End_keepalive(irn, i)));
		} else {
			for (i = get_irn_arity(irn) - 1; i >= 0; i--) {
				pred = get_irn_n(irn, i);
				set_irn_n(irn_copy, i, (ir_node*)get_irn_link(pred));
			}
		}
	}
}

/**
 * Get the method argument at the position "pos".
 *
 * @param irg  irg that must be cloned.
 * @param pos  The position of the argument.
 */
static ir_node *get_irg_arg(ir_graph *irg, size_t pos)
{
	ir_node *irg_args = get_irg_args(irg), *arg = NULL;

	/* Call algorithm that computes the out edges */
	assure_irg_outs(irg);

	/* Search the argument with the number pos.*/
	for (unsigned i = get_irn_n_outs(irg_args); i-- > 0; ) {
		ir_node *proj = get_irn_out(irg_args, i);
		if ((int)pos == get_Proj_proj(proj)) {
			if (arg) {
				/*
				 * More than one arg node found:
				 * We rely on the fact that only one arg exists, so do
				 * a cheap CSE in this case.
				 */
				set_irn_out(irg_args, i, arg, 0);
				exchange(proj, arg);
			} else
				arg = proj;
		}
	}
	assert(arg && "Argument not found");
	return arg;
}

/**
 * Create a new graph for the clone of the method,
 * that we want to clone.
 *
 * @param ent The entity of the method that must be cloned.
 * @param q   Our quadruplet.
 */
static void create_clone_proc_irg(ir_entity *ent, const quadruple_t *q)
{
	ir_graph *method_irg, *clone_irg;
	ir_node *arg, *const_arg;

	method_irg = get_entity_irg(ent);

	/* We create the skeleton of the clone irg.*/
	clone_irg  = new_ir_graph(ent, 0);

	arg        = get_irg_arg(get_entity_irg(q->ent), q->pos);
	/* we will replace the argument in position "q->pos" by this constant. */
	const_arg  = new_r_Const(clone_irg, q->tv);

	/* args copy in the cloned graph will be the const. */
	set_irn_link(arg, const_arg);

	/* Store the arg that will be replaced here, so we can easily detect it. */
	set_irg_link(clone_irg, arg);

	/* We copy the blocks and nodes, that must be in
	the clone graph and set their predecessors. */
	irg_walk_graph(method_irg, copy_nodes, set_preds, clone_irg);

	/* The "cloned" graph must be matured. */
	mature_immBlock(get_irg_end_block(clone_irg));
	irg_finalize_cons(clone_irg);
}

/**
 * The function create a new entity type
 * for our clone and set it to clone entity.
 *
 * @param q   Contains information for the method to clone.
 * @param ent The entity of the clone.
 * @param nr  A pointer to the counter of clones.
 **/
static void change_entity_type(const quadruple_t *q, ir_entity *ent)
{
	ir_type *mtp, *new_mtp, *tp;
	size_t  i, j, n_params, n_ress;

	mtp      = get_entity_type(q->ent);
	n_params = get_method_n_params(mtp);
	n_ress   = get_method_n_ress(mtp);

	/* Create the new type for our clone. It must have one parameter
	   less then the original.*/
	new_mtp  = new_type_method(n_params - 1, n_ress);

	/* We must set the type of the methods parameters.*/
	for (i = j = 0; i < n_params; ++i) {
		if (i == q->pos) {
			/* This is the position of the argument, that we have
			   replaced. */
			continue;
		}
		tp = get_method_param_type(mtp, i);
		set_method_param_type(new_mtp, j++, tp);
	}
	/* Copy the methods result types. */
	for (i = 0; i < n_ress; ++i) {
		tp = get_method_res_type(mtp, i);
		set_method_res_type(new_mtp, i, tp);
	}
	set_entity_type(ent, new_mtp);
}

/**
 * Make a clone of a method.
 *
 * @param q   Contains information for the method to clone.
 */
static ir_entity *clone_method(const quadruple_t *q)
{
	ir_entity *new_entity;
	ident *clone_ident;
	/* A counter for the clones.*/
	static size_t nr = 0;

	/* We get a new ident for our clone method.*/
	clone_ident = get_clone_ident(get_entity_ident(q->ent), q->pos, nr);
	/* We get our entity for the clone method. */
	new_entity  = copy_entity_name(q->ent, clone_ident);

	/* a cloned entity is always local */
	set_entity_visibility(new_entity, ir_visibility_local);

	/* set a ld name here: Should we mangle this ? */
	set_entity_ld_ident(new_entity, get_entity_ident(new_entity));

	/* set a new type here. */
	change_entity_type(q, new_entity);

	/* We need now a new ir_graph for our clone method. */
	create_clone_proc_irg(new_entity, q);

	/* The "new_entity" don't have this information. */
	new_entity->attr.mtd_attr.param_access = NULL;
	new_entity->attr.mtd_attr.param_weight = NULL;

	return new_entity;
}

/**
 * Creates a new "cloned" Call node and return it.
 *
 * @param call        The call that must be cloned.
 * @param new_entity  The entity of the cloned function.
 * @param pos         The position of the replaced parameter of this call.
 **/
static ir_node *new_cl_Call(ir_node *call, ir_entity *new_entity, size_t pos)
{
	ir_node **in;
	size_t i, n_params, new_params = 0;
	ir_node *callee;
	symconst_symbol sym;
	ir_graph *irg = get_irn_irg(call);
	ir_node *bl = get_nodes_block(call);

	sym.entity_p = new_entity;
	callee = new_r_SymConst(irg, mode_P_code, sym, symconst_addr_ent);

	n_params = get_Call_n_params(call);
	NEW_ARR_A(ir_node *, in, n_params - 1);

	/* we save the parameters of the new call in the array "in" without the
	 * parameter in position "pos", that is replaced with a constant.*/
	for (i = 0; i < n_params; ++i) {
		if (pos != i)
			in[new_params++] = get_Call_param(call, i);
	}
	/* Create and return the new Call. */
	return new_r_Call(bl, get_Call_mem(call),
		callee, n_params - 1, in, get_entity_type(new_entity));
}

/**
 * Exchange all Calls stored in the quadruplet to Calls of the cloned entity.
 *
 * @param q             The quadruple
 * @param cloned_ent    The entity of the new function that must be called
 *                      from the new Call.
 */
static void exchange_calls(quadruple_t *q, ir_entity *cloned_ent)
{
	size_t pos = q->pos;
	ir_node *new_call, *call;
	size_t i;

	/* We iterate the list of the "call".*/
	for (i = 0; i < ARR_LEN(q->calls); ++i) {
		call = q->calls[i];

		/* A clone exist and the copy of "call" in this
		 * clone graph must be exchanged with new one.*/
		new_call = new_cl_Call(call, cloned_ent, pos);
		exchange(call, new_call);
	}
}

/**
 * The weight formula:
 * We save one instruction in every caller and param_weight instructions
 * in the callee.
 */
static float calculate_weight(const entry_t *entry)
{
	return ARR_LEN(entry->q.calls) *
		(float)(get_method_param_weight(entry->q.ent, entry->q.pos) + 1);
}

/**
 * After we exchanged all calls, some entries on the list for
 * the next cloned entity may get invalid, so we have to check
 * them and may even update the list of heavy uses.
 */
static void reorder_weights(q_set *hmap, float threshold)
{
	entry_t **adr, *p, *entry;
	size_t i, len;

restart:
	entry = hmap->heavy_uses;
	if (! entry)
		return;

	len = ARR_LEN(entry->q.calls);
	for (i = 0; i < len; ++i) {
		ir_node *ptr, *call = entry->q.calls[i];

		/* might be exchanged, so skip Id nodes here. */
		call = skip_Id(call);

		/* we know, that a SymConst is here */
		ptr = get_Call_ptr(call);

		ir_entity *const callee = get_SymConst_entity(ptr);
		if (callee != entry->q.ent) {
			/*
			 * This call is already changed because of a previous
			 * optimization. Remove it from the list.
			 */
			--len;
			entry->q.calls[i] = entry->q.calls[len];
			entry->q.calls[len] = NULL;

			/* the new call should be processed */
			process_call(call, callee, hmap);
			--i;
		}
	}

	/* the length might be changed */
	ARR_SHRINKLEN(entry->q.calls, len);

	/* recalculate the weight and resort the heavy uses map */
	entry->weight = calculate_weight(entry);

	if (len <= 0 || entry->weight < threshold) {
		hmap->heavy_uses = entry->next;
		kill_entry(entry);

		/* we have changed the list, check the next one */
		goto restart;
	}

	adr = NULL;
	for (p = entry->next; p && entry->weight < p->weight; p = p->next) {
		adr = &p->next;
	}

	if (adr) {
		hmap->heavy_uses = entry->next;
		entry->next      = *adr;
		*adr             = entry;

		/* we have changed the list, check the next one */
		goto restart;
	}
}

/*
 * Do the procedure cloning. Evaluate a heuristic weight for every
 * call(..., Const, ...). If the weight is bigger than threshold,
 * clone the entity and fix the calls.
 */
void proc_cloning(float threshold)
{
	entry_t *p;
	size_t i, n;
	q_set hmap;

	DEBUG_ONLY(firm_dbg_module_t *dbg;)

	/* register a debug mask */
	FIRM_DBG_REGISTER(dbg, "firm.opt.proc_cloning");

	obstack_init(&hmap.obst);
	hmap.map        = NULL;
	hmap.heavy_uses = NULL;

	/* initially fill our map by visiting all irgs */
	for (i = 0, n = get_irp_n_irgs(); i < n; ++i) {
		ir_graph *irg = get_irp_irg(i);
		irg_walk_graph(irg, collect_irg_calls, NULL, &hmap);
	}

	/* We have the "Call" nodes to optimize in set "set_entries". Our algorithm
	   replace one constant parameter and make a new "Call" node for all found "Calls". It exchange the
	   old one with the new one and the algorithm is called with the new "Call".
	 */
	while (hmap.map || hmap.heavy_uses) {
		/* We iterate the set and arrange the element of the set in a list.
		   The elements are arranged dependent of their value descending.*/
		if (hmap.map) {
			foreach_pset(hmap.map, entry_t, entry) {
				entry->weight = calculate_weight(entry);

				/*
				 * Do not put entry with a weight < threshold in the list
				 */
				if (entry->weight < threshold) {
					kill_entry(entry);
					continue;
				}

				/* put entry in the heavy uses list */
				entry->next = NULL;
				if (! hmap.heavy_uses)
					hmap.heavy_uses = entry;
				else {
					if (entry->weight >= hmap.heavy_uses->weight) {
						entry->next     = hmap.heavy_uses;
						hmap.heavy_uses = entry;
					} else {
						for (p = hmap.heavy_uses; p->next; p = p->next) {
							if (entry->weight >= p->next->weight) {
								entry->next = p->next;
								p->next     = entry;
								break;
							}
						}
						if (! p->next)
							p->next = entry;
					}
				}
			}
			del_pset(hmap.map);
			hmap.map = NULL;
		}

#ifdef DEBUG_libfirm
		/* Print some information about the list. */
		DB((dbg, LEVEL_2, "-----------------\n"));
		for (entry_t *entry = hmap.heavy_uses; entry; entry = entry->next) {
			DB((dbg, LEVEL_2, "\nweight: is %f\n", entry->weight));
			DB((dbg, LEVEL_2, "Call for Method %E\n", entry->q.ent));
			DB((dbg, LEVEL_2, "Position %zu\n", entry->q.pos));
			DB((dbg, LEVEL_2, "Value %T\n", entry->q.tv));
		}
#endif
		entry_t *const entry = hmap.heavy_uses;
		if (entry) {
			quadruple_t *qp = &entry->q;

			ir_entity *ent = clone_method(qp);
			DB((dbg, LEVEL_1, "Cloned <%+F, %zu, %T> info %+F\n", qp->ent, qp->pos, qp->tv, ent));

			hmap.heavy_uses = entry->next;

			/* We must exchange the copies of this call in all clones too.*/
			exchange_calls(&entry->q, ent);
			kill_entry(entry);

			/*
			 * after we exchanged all calls, some entries on the list for
			 * the next cloned entity may get invalid, so we have to check
			 * them and may even update the list of heavy uses.
			 */
			reorder_weights(&hmap, threshold);
		}
	}
	obstack_free(&hmap.obst, NULL);
}

typedef struct pass_t {
	ir_prog_pass_t pass;
	float          threshold;
} pass_t;

/**
 * Wrapper to run proc_cloning() as an ir_prog pass.
 */
static int proc_cloning_wrapper(ir_prog *irp, void *context)
{
	pass_t *pass = (pass_t*)context;

	(void)irp;
	proc_cloning(pass->threshold);
	return 0;
}

/* create a ir_prog pass */
ir_prog_pass_t *proc_cloning_pass(const char *name, float threshold)
{
	pass_t *pass = XMALLOCZ(pass_t);

	pass->threshold = threshold;
	return def_prog_pass_constructor(
		&pass->pass, name ? name : "cloning", proc_cloning_wrapper);
}
