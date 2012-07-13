/*
 * Copyright (C) 1995-2011 University of Karlsruhe.  All right reserved.
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
 * @file    tr_inheritance.c
 * @brief   Utility routines for inheritance representation
 * @author  Goetz Lindenmaier
 */
#include "config.h"

#include "debug.h"
#include "typerep.h"
#include "irgraph_t.h"
#include "irprog_t.h"
#include "irprintf.h"
#include "pset.h"
#include "set.h"
#include "irgwalk.h"
#include "irflag.h"

/* ----------------------------------------------------------------------- */
/* Resolve implicit inheritance.                                           */
/* ----------------------------------------------------------------------- */

ident *default_mangle_inherited_name(const ir_entity *super, const ir_type *clss)
{
	return id_mangle_u(new_id_from_str("inh"), id_mangle_u(get_class_ident(clss), get_entity_ident(super)));
}

/** Replicates all entities in all super classes that are not overwritten
    by an entity of this class. */
static void copy_entities_from_superclass(ir_type *clss, void *env)
{
	size_t i;
	size_t j;
	size_t k;
	size_t l;
	int overwritten;
	ir_type *super;
	ir_entity *inhent, *thisent;
	mangle_inherited_name_func *mfunc = *(mangle_inherited_name_func **)env;

	for (i = 0; i < get_class_n_supertypes(clss); i++) {
		super = get_class_supertype(clss, i);
		assert(is_Class_type(super) && "not a class");
		for (j = 0; j < get_class_n_members(super); j++) {
			inhent = get_class_member(super, j);
			/* check whether inhent is already overwritten */
			overwritten = 0;
			for (k = 0; (k < get_class_n_members(clss)) && (overwritten == 0); k++) {
				thisent = get_class_member(clss, k);
				for (l = 0; l < get_entity_n_overwrites(thisent); l++) {
					if (inhent == get_entity_overwrites(thisent, l)) {
						/* overwritten - do not copy */
						overwritten = 1;
						break;
					}
				}
			}
			/* Inherit entity */
			if (!overwritten) {
				thisent = copy_entity_own(inhent, clss);
				add_entity_overwrites(thisent, inhent);
				if (get_entity_peculiarity(inhent) == peculiarity_existent)
					set_entity_peculiarity(thisent, peculiarity_inherited);
				set_entity_ld_ident(thisent, mfunc(inhent, clss));
				if (get_entity_linkage(inhent) & IR_LINKAGE_CONSTANT) {
					assert(is_atomic_entity(inhent) &&  /* @@@ */
						"Inheritance of constant, compound entities not implemented");
					add_entity_linkage(thisent, IR_LINKAGE_CONSTANT);
					set_atomic_ent_value(thisent, get_atomic_ent_value(inhent));
				}
			}
		}
	}
}

void resolve_inheritance(mangle_inherited_name_func *mfunc)
{
	if (!mfunc)
		mfunc = default_mangle_inherited_name;
	class_walk_super2sub(copy_entities_from_superclass, NULL, (void *)&mfunc);
}


/* ----------------------------------------------------------------------- */
/* The transitive closure of the subclass/superclass and                   */
/* overwrites/overwrittenby relation.                                      */
/*                                                                         */
/* A walk over the ir (O(#types+#entities)) computes the transitive        */
/* closure.  Adding a new type/entity or changing the basic relations in   */
/* some other way invalidates the transitive closure, i.e., it is not      */
/* updated by the basic functions.                                         */
/*                                                                         */
/* All functions are named as their counterparts for the basic relations,  */
/* adding the infix 'trans_'.                                              */
/* ----------------------------------------------------------------------- */

void                        set_irp_inh_transitive_closure_state(inh_transitive_closure_state s)
{
	irp->inh_trans_closure_state = s;
}
void                        invalidate_irp_inh_transitive_closure_state(void)
{
	if (irp->inh_trans_closure_state == inh_transitive_closure_valid)
		irp->inh_trans_closure_state = inh_transitive_closure_invalid;
}
inh_transitive_closure_state get_irp_inh_transitive_closure_state(void)
{
	return irp->inh_trans_closure_state;
}

static void assert_valid_state(void)
{
	assert(irp->inh_trans_closure_state == inh_transitive_closure_valid ||
	       irp->inh_trans_closure_state == inh_transitive_closure_invalid);
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
/* There is a set that extends each entity/type with two new               */
/* fields:  one for the upwards directed relation: 'up' (supertype,        */
/* overwrites) and one for the downwards directed relation: 'down' (sub-   */
/* type, overwrittenby.  These fields contain psets (and maybe later       */
/* arrays) listing all subtypes...                                         */
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

typedef enum {
	d_up   = 0,
	d_down = 1,
} dir;

typedef struct {
	const firm_kind *kind;   /**< An entity or type. */
	pset *directions[2];
} tr_inh_trans_tp;

/* We use this set for all types and entities.  */
static set *tr_inh_trans_set = NULL;

/**
 * Compare two tr_inh_trans_tp entries.
 */
static int tr_inh_trans_cmp(const void *e1, const void *e2, size_t size)
{
	const tr_inh_trans_tp *ef1 = (const tr_inh_trans_tp*)e1;
	const tr_inh_trans_tp *ef2 = (const tr_inh_trans_tp*)e2;
	(void) size;

	return ef1->kind != ef2->kind;
}

/**
 * calculate the hash value of an tr_inh_trans_tp
 */
static inline unsigned int tr_inh_trans_hash(const tr_inh_trans_tp *v)
{
	return hash_ptr(v->kind);
}

/* This always completes successfully. */
static tr_inh_trans_tp *get_firm_kind_entry(const firm_kind *k)
{
	tr_inh_trans_tp a, *found;
	a.kind = k;

	if (!tr_inh_trans_set) tr_inh_trans_set = new_set(tr_inh_trans_cmp, 128);

	found = set_find(tr_inh_trans_tp, tr_inh_trans_set, &a, sizeof(a), tr_inh_trans_hash(&a));
	if (!found) {
		a.directions[d_up]   = pset_new_ptr(16);
		a.directions[d_down] = pset_new_ptr(16);
		found = set_insert(tr_inh_trans_tp, tr_inh_trans_set, &a, sizeof(a), tr_inh_trans_hash(&a));
	}
	return found;
}

static pset *get_entity_map(const ir_entity *ent, dir d)
{
	tr_inh_trans_tp *found;

	assert(is_entity(ent));
	found = get_firm_kind_entry((const firm_kind *)ent);
	return found->directions[d];
}

static pset *get_type_map(const ir_type *tp, dir d)
{
	tr_inh_trans_tp *found;

	assert(is_type(tp));
	found = get_firm_kind_entry((const firm_kind *)tp);
	return found->directions[d];
}


/**
 * Walk over all types reachable from tp in the sub/supertype
 * relation and compute the closure for the two downwards directed
 * relations.
 *
 * The walk in the dag formed by the relation is tricky:  We must visit
 * all subtypes before visiting the supertypes.  So we first walk down.
 * Then we can compute the closure for this type.  Then we walk up.
 * As we call ourselves recursive, and walk in both directions, there
 * can be cycles.  So we have to make sure, that if we visit a node
 * a second time (in a walk up) we do nothing.  For this we increment
 * the master visited flag twice.
 * If the type is marked with master_flag_visited-1 it is on the stack.
 * If it is marked with master_flag_visited it is fully processed.
 *
 * Well, we still miss some candidates ... */
static void compute_down_closure(ir_type *tp)
{
	pset *myset, *subset;
	size_t i, n_subtypes, n_members, n_supertypes;
	ir_visited_t master_visited = get_master_type_visited();

	assert(is_Class_type(tp));

	set_type_visited(tp, master_visited-1);

	/* Recursive descend. */
	n_subtypes = get_class_n_subtypes(tp);
	for (i = 0; i < n_subtypes; ++i) {
		ir_type *stp = get_class_subtype(tp, i);
		if (get_type_visited(stp) < master_visited-1) {
			compute_down_closure(stp);
		}
	}

	/* types */
	myset = get_type_map(tp, d_down);
	for (i = 0; i < n_subtypes; ++i) {
		ir_type *stp = get_class_subtype(tp, i);
		subset = get_type_map(stp, d_down);
		pset_insert_ptr(myset, stp);
		pset_insert_pset_ptr(myset, subset);
	}

	/* entities */
	n_members = get_class_n_members(tp);
	for (i = 0; i < n_members; ++i) {
		ir_entity *mem = get_class_member(tp, i);
		size_t j, n_overwrittenby = get_entity_n_overwrittenby(mem);

		myset = get_entity_map(mem, d_down);
		for (j = 0; j < n_overwrittenby; ++j) {
			ir_entity *ov = get_entity_overwrittenby(mem, j);
			subset = get_entity_map(ov, d_down);
			pset_insert_ptr(myset, ov);
			pset_insert_pset_ptr(myset, subset);
		}
	}

	mark_type_visited(tp);

	/* Walk up. */
	n_supertypes = get_class_n_supertypes(tp);
	for (i = 0; i < n_supertypes; ++i) {
		ir_type *stp = get_class_supertype(tp, i);
		if (get_type_visited(stp) < master_visited-1) {
			compute_down_closure(stp);
		}
	}
}

static void compute_up_closure(ir_type *tp)
{
	pset *myset, *subset;
	size_t i, n_subtypes, n_members, n_supertypes;
	ir_visited_t master_visited = get_master_type_visited();

	assert(is_Class_type(tp));

	set_type_visited(tp, master_visited-1);

	/* Recursive descend. */
	n_supertypes = get_class_n_supertypes(tp);
	for (i = 0; i < n_supertypes; ++i) {
		ir_type *stp = get_class_supertype(tp, i);
		if (get_type_visited(stp) < get_master_type_visited()-1) {
			compute_up_closure(stp);
		}
	}

	/* types */
	myset = get_type_map(tp, d_up);
	for (i = 0; i < n_supertypes; ++i) {
		ir_type *stp = get_class_supertype(tp, i);
		subset = get_type_map(stp, d_up);
		pset_insert_ptr(myset, stp);
		pset_insert_pset_ptr(myset, subset);
	}

	/* entities */
	n_members = get_class_n_members(tp);
	for (i = 0; i < n_members; ++i) {
		ir_entity *mem = get_class_member(tp, i);
		size_t j, n_overwrites = get_entity_n_overwrites(mem);

		myset = get_entity_map(mem, d_up);
		for (j = 0; j < n_overwrites; ++j) {
			ir_entity *ov = get_entity_overwrites(mem, j);
			subset = get_entity_map(ov, d_up);
			pset_insert_pset_ptr(myset, subset);
			pset_insert_ptr(myset, ov);
		}
	}

	mark_type_visited(tp);

	/* Walk down. */
	n_subtypes = get_class_n_subtypes(tp);
	for (i = 0; i < n_subtypes; ++i) {
		ir_type *stp = get_class_subtype(tp, i);
		if (get_type_visited(stp) < master_visited-1) {
			compute_up_closure(stp);
		}
	}
}

void compute_inh_transitive_closure(void)
{
	size_t i, n_types = get_irp_n_types();
	free_inh_transitive_closure();

	/* The 'down' relation */
	irp_reserve_resources(irp, IRP_RESOURCE_TYPE_VISITED);
	inc_master_type_visited();  /* Inc twice: one if on stack, second if values computed. */
	inc_master_type_visited();
	for (i = 0; i < n_types; ++i) {
		ir_type *tp = get_irp_type(i);
		if (is_Class_type(tp) && type_not_visited(tp)) { /* For others there is nothing to accumulate. */
			size_t j, n_subtypes = get_class_n_subtypes(tp);
			int has_unmarked_subtype = 0;

			assert(get_type_visited(tp) < get_master_type_visited()-1);
			for (j = 0; j < n_subtypes; ++j) {
				ir_type *stp = get_class_subtype(tp, j);
				if (type_not_visited(stp)) {
					has_unmarked_subtype = 1;
					break;
				}
			}

			/* This is a good starting point. */
			if (!has_unmarked_subtype)
				compute_down_closure(tp);
		}
	}

	/* The 'up' relation */
	inc_master_type_visited();
	inc_master_type_visited();
	for (i = 0; i < n_types; ++i) {
		ir_type *tp = get_irp_type(i);
		if (is_Class_type(tp) && type_not_visited(tp)) { /* For others there is nothing to accumulate. */
			size_t j, n_supertypes = get_class_n_supertypes(tp);
			int has_unmarked_supertype = 0;

			assert(get_type_visited(tp) < get_master_type_visited()-1);
			for (j = 0; j < n_supertypes; ++j) {
				ir_type *stp = get_class_supertype(tp, j);
				if (type_not_visited(stp)) {
					has_unmarked_supertype = 1;
					break;
				}
			}

			/* This is a good starting point. */
			if (!has_unmarked_supertype)
				compute_up_closure(tp);
		}
	}

	irp->inh_trans_closure_state = inh_transitive_closure_valid;
	irp_free_resources(irp, IRP_RESOURCE_TYPE_VISITED);
}

void free_inh_transitive_closure(void)
{
	if (tr_inh_trans_set) {
		foreach_set(tr_inh_trans_set, tr_inh_trans_tp, elt) {
			del_pset(elt->directions[d_up]);
			del_pset(elt->directions[d_down]);
		}
		del_set(tr_inh_trans_set);
		tr_inh_trans_set = NULL;
	}
	irp->inh_trans_closure_state = inh_transitive_closure_none;
}

/* - subtype ------------------------------------------------------------- */

ir_type *get_class_trans_subtype_first(const ir_type *tp)
{
	assert_valid_state();
	return (ir_type*)pset_first(get_type_map(tp, d_down));
}

ir_type *get_class_trans_subtype_next(const ir_type *tp)
{
	assert_valid_state();
	return (ir_type*)pset_next(get_type_map(tp, d_down));
}

int is_class_trans_subtype(const ir_type *tp, const ir_type *subtp)
{
	assert_valid_state();
	return (pset_find_ptr(get_type_map(tp, d_down), subtp) != NULL);
}

/* - supertype ----------------------------------------------------------- */

ir_type *get_class_trans_supertype_first(const ir_type *tp)
{
	assert_valid_state();
	return (ir_type*)pset_first(get_type_map(tp, d_up));
}

ir_type *get_class_trans_supertype_next(const ir_type *tp)
{
	assert_valid_state();
	return (ir_type*)pset_next(get_type_map(tp, d_up));
}

/* - overwrittenby ------------------------------------------------------- */

ir_entity *get_entity_trans_overwrittenby_first(const ir_entity *ent)
{
	assert_valid_state();
	return (ir_entity*)pset_first(get_entity_map(ent, d_down));
}

ir_entity *get_entity_trans_overwrittenby_next(const ir_entity *ent)
{
	assert_valid_state();
	return (ir_entity*)pset_next(get_entity_map(ent, d_down));
}

/* - overwrites ---------------------------------------------------------- */


ir_entity *get_entity_trans_overwrites_first(const ir_entity *ent)
{
	assert_valid_state();
	return (ir_entity*)pset_first(get_entity_map(ent, d_up));
}

ir_entity *get_entity_trans_overwrites_next(const ir_entity *ent)
{
	assert_valid_state();
	return (ir_entity*)pset_next(get_entity_map(ent, d_up));
}


/* ----------------------------------------------------------------------- */
/* Classify pairs of types/entities in the inheritance relations.          */
/* ----------------------------------------------------------------------- */

/** Returns true if low is subclass of high. */
static int check_is_SubClass_of(ir_type *low, ir_type *high)
{
	size_t i, n_subtypes;

	/* depth first search from high downwards. */
	n_subtypes = get_class_n_subtypes(high);
	for (i = 0; i < n_subtypes; i++) {
		ir_type *stp = get_class_subtype(high, i);
		if (low == stp) return 1;
		if (is_SubClass_of(low, stp))
			return 1;
	}
	return 0;
}

int is_SubClass_of(ir_type *low, ir_type *high)
{
	assert(is_Class_type(low) && is_Class_type(high));

	if (low == high) return 1;

	if (get_irp_inh_transitive_closure_state() == inh_transitive_closure_valid) {
		pset *m = get_type_map(high, d_down);
		return pset_find_ptr(m, low) ? 1 : 0;
	}
	return check_is_SubClass_of(low, high);
}

int is_SubClass_ptr_of(ir_type *low, ir_type *high)
{
	while (is_Pointer_type(low) && is_Pointer_type(high)) {
		low  = get_pointer_points_to_type(low);
		high = get_pointer_points_to_type(high);
	}

	if (is_Class_type(low) && is_Class_type(high))
		return is_SubClass_of(low, high);
	return 0;
}

int is_overwritten_by(ir_entity *high, ir_entity *low)
{
	size_t i, n_overwrittenby;
	assert(is_entity(low) && is_entity(high));

	if (get_irp_inh_transitive_closure_state() == inh_transitive_closure_valid) {
		pset *m = get_entity_map(high, d_down);
		return pset_find_ptr(m, low) ? 1 : 0;
	}

	/* depth first search from high downwards. */
	n_overwrittenby = get_entity_n_overwrittenby(high);
	for (i = 0; i < n_overwrittenby; i++) {
		ir_entity *ov = get_entity_overwrittenby(high, i);
		if (low == ov) return 1;
		if (is_overwritten_by(low, ov))
			return 1;
	}
	return 0;
}

/** Resolve polymorphy in the inheritance relation.
 *
 * Returns the dynamically referenced entity if the static entity and the
 * dynamic type are given.
 * Search downwards in overwritten tree.
 *
 * Need two routines because I want to assert the result.
 */
static ir_entity *do_resolve_ent_polymorphy(ir_type *dynamic_class, ir_entity *static_ent)
{
	size_t i, n_overwrittenby;

	ir_type *owner = get_entity_owner(static_ent);
	if (owner == dynamic_class) return static_ent;

	// if the owner of the static_ent already is more special than the dynamic
	// type to check against - stop here.
	if (! is_SubClass_of(dynamic_class, owner)) return NULL;

	n_overwrittenby = get_entity_n_overwrittenby(static_ent);
	for (i = 0; i < n_overwrittenby; ++i) {
		ir_entity *ent = get_entity_overwrittenby(static_ent, i);
		ent = do_resolve_ent_polymorphy(dynamic_class, ent);
		if (ent) return ent;
	}

	// No further specialization of static_ent has been found
	return static_ent;
}

ir_entity *resolve_ent_polymorphy(ir_type *dynamic_class, ir_entity *static_ent)
{
	ir_entity *res;
	assert(static_ent && is_entity(static_ent));

	res = do_resolve_ent_polymorphy(dynamic_class, static_ent);
	assert(res);

	return res;
}



/* ----------------------------------------------------------------------- */
/* Class cast state handling.                                              */
/* ----------------------------------------------------------------------- */

/* - State handling. ----------------------------------------- */

void set_irg_class_cast_state(ir_graph *irg, ir_class_cast_state s)
{
	if (get_irp_class_cast_state() > s)
		set_irp_class_cast_state(s);
	irg->class_cast_state = s;
}

ir_class_cast_state get_irg_class_cast_state(const ir_graph *irg)
{
	return irg->class_cast_state;
}

void set_irp_class_cast_state(ir_class_cast_state s)
{
#ifndef NDEBUG
	size_t i, n;
	for (i = 0, n = get_irp_n_irgs(); i < n; ++i)
		assert(get_irg_class_cast_state(get_irp_irg(i)) >= s);
#endif
	irp->class_cast_state = s;
}

ir_class_cast_state get_irp_class_cast_state(void)
{
	return irp->class_cast_state;
}
