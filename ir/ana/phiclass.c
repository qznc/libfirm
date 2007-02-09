/**
 * @author Daniel Grund, Christian Wuerdig
 * @cvsid  $Id$
 * @date   09.08.2005
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef HAVE_STDLIB_H
# include <stdlib.h>
#endif

#include "irnode.h"
#include "debug.h"
#include "irgwalk.h"
#include "irop_t.h"
#include "iredges_t.h"
#include "phiclass.h"
#include "irphase_t.h"

struct _phi_classes_t {
	phase_t  ph;                 /* The phase object holding the irn data */
	pset     *all_phi_classes;   /* A set containing all Phi classes */
	ir_graph *irg;               /* The irg this is all about */
	DEBUG_ONLY(firm_dbg_module_t *dbg);
};

typedef struct _irn_phi_class_t {
	ir_node **phi_cls; /* the array of node pointers representing the class */
} irn_phi_class_t;

static INLINE ir_node **_get_phi_class(phase_t *ph, ir_node *irn) {
	irn_phi_class_t *ipc = phase_get_or_set_irn_data(ph, irn);
	return ipc->phi_cls;
}

static INLINE void _set_phi_class(phase_t *ph, ir_node *irn, ir_node **cls) {
	irn_phi_class_t *ipc = phase_get_or_set_irn_data(ph, irn);
	ipc->phi_cls = cls;
}

/* initialize data structure for given irn in given phase */
static void *irn_phi_class_init(phase_t *ph, ir_node *irn, void *data) {
	irn_phi_class_t *ipc = data ? data : phase_alloc(ph, sizeof(ipc[0]));
	memset(ipc, 0, sizeof(ipc[0]));
	return ipc;
}

/**
 * Builds the phi class, starting from.
 * @param phi_classes  The phi class object
 * @param irn          The to start from
 * @param pc           The phi class this irn should be put into.
 *                     Set to NULL if you want a new one.
 */
static void phi_class_build(phi_classes_t *phi_classes, ir_node *irn, ir_node **pc) {
	const ir_edge_t *edge;

	/* If irn has a phi class assigned already
	 * return immediately to stop recursion */
	if (_get_phi_class(&phi_classes->ph, irn)) {
		DBG((phi_classes->dbg, LEVEL_2, "  already done for %+F\n", irn));
		return;
	}

	/* The initial call to phi_class_build doesn't
	 * provide a nodeset, so alloc it */
	if (! pc) {
		DBG((phi_classes->dbg, LEVEL_1, "Computing phi class for %+F\n", irn));
		assert(is_Phi(irn));
		pc = NEW_ARR_F(ir_node *, 0);
	}

	/* Add the irn to the phi class */
	DBG((phi_classes->dbg, LEVEL_1, "  adding %+F\n", irn));
	ARR_APP1(ir_node *, pc, irn);
	_set_phi_class(&phi_classes->ph, irn, pc);

	/* Check the 'neighbour' irns */
	if (is_Phi(irn) && mode_is_datab(get_irn_mode(irn))) {
		int i;

		/* Add all args of the phi to the phi-class. */
		for (i = get_irn_arity(irn) - 1; i >= 0; --i) {
			ir_node *op = get_irn_n(irn, i);
			DBG((phi_classes->dbg, LEVEL_2, "  checking arg %+F\n", op));
		 	phi_class_build(phi_classes, op, pc);
		}
	}

	/* Add a user of the irn to the class,
	 * iff it is a phi node  */
	foreach_out_edge(irn, edge) {
		ir_node *user = edge->src;
		DBG((phi_classes->dbg, LEVEL_2, "  checking user %+F\n", user));
		if (is_Phi(user) && mode_is_datab(get_irn_mode(user)))
			phi_class_build(phi_classes, user, pc);
	}
}

/**
 * Call by a walker. If @p node is Phi, it build the Phi class starting from this node.
 */
static void phi_class_construction_walker(ir_node *node, void *env) {
	phi_classes_t *pc = env;

	if (is_Phi(node) && mode_is_datab(get_irn_mode(node))) {
		ir_node **irn_pc = _get_phi_class(&pc->ph, node);

		if (! irn_pc) {
			phi_class_build(pc, node, NULL);
			pset_insert_ptr(pc->all_phi_classes, _get_phi_class(&pc->ph, node));
		}
	}
}

/**
 * Walk over the irg and build the Phi classes.
 */
static void phi_class_compute(phi_classes_t *pc) {
	irg_walk_graph(pc->irg, phi_class_construction_walker, NULL, pc);
}

/**
 * Build the Phi classes for the set of given Phis.
 */
static void phi_class_compute_by_phis(phi_classes_t *pc, pset *all_phi_nodes) {
	if (pset_count(all_phi_nodes)) {
		ir_node  *phi;

		foreach_pset(all_phi_nodes, phi) {
			ir_node **irn_pc = _get_phi_class(&pc->ph, phi);

			assert(is_Phi(phi) && mode_is_datab(get_irn_mode(phi)));

			if (! irn_pc) {
				phi_class_build(pc, phi, NULL);
				pset_insert_ptr(pc->all_phi_classes, _get_phi_class(&pc->ph, phi));
			}
		}
	}
}

/**
 * Return the array containing all nodes assigned to the same Phi class as @p irn.
 */
ir_node **get_phi_class(phi_classes_t *pc, ir_node *irn) {
	return _get_phi_class(&pc->ph, irn);
}

/**
 * Assigns a new array of nodes representing the new Phi class to @p irn.
 */
void set_phi_class(phi_classes_t *pc, ir_node *irn, ir_node **cls) {
	_set_phi_class(&pc->ph, irn, cls);
}

/**
 * Returns a set containing all computed Phi classes.
 */
pset *get_all_phi_classes(phi_classes_t *pc) {
	return pc->all_phi_classes;
}

/**
 * Builds the Phi classes for all Phis in @p irg.
 * @return The Phi class object for the @p irg.
 */
phi_classes_t *phi_class_new_from_irg(ir_graph *irg) {
	phi_classes_t *res = xmalloc(sizeof(*res));

	FIRM_DBG_REGISTER(res->dbg, "ir.ana.phiclass");
	phase_init(&res->ph, "phi_classes", irg, PHASE_DEFAULT_GROWTH, irn_phi_class_init);

	res->irg             = irg;
	res->all_phi_classes = pset_new_ptr(5);

	phi_class_compute(res);

	return res;
}

/**
 * Builds all Phi classes for the given set of Phis.
 * @return The Phis class object for @p all_phis.
 */
phi_classes_t *phi_class_new_from_set(ir_graph *irg, pset *all_phis) {
	phi_classes_t *res = xmalloc(sizeof(*res));

	FIRM_DBG_REGISTER(res->dbg, "ir.ana.phiclass");
	phase_init(&res->ph, "phi_classes", irg, PHASE_DEFAULT_GROWTH, irn_phi_class_init);

	res->irg             = irg;
	res->all_phi_classes = pset_new_ptr(5);

	phi_class_compute_by_phis(res, all_phis);

	return res;
}

/**
 * Free all allocated data.
 */
void phi_class_free(phi_classes_t *pc) {
	ir_node **ipc;
	foreach_pset(pc->all_phi_classes, ipc) {
		DEL_ARR_F(ipc);
	}
	del_pset(pc->all_phi_classes);
	phase_free(&pc->ph);
	xfree(pc);
}
