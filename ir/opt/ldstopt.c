/*
 * Project:     libFIRM
 * File name:   ir/opt/ldstopt.c
 * Purpose:     load store optimizations
 * Author:
 * Created:
 * CVS-ID:      $Id$
 * Copyright:   (c) 1998-2004 Universit��t Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */
# include "irnode_t.h"
# include "irgraph_t.h"
# include "irmode_t.h"
# include "iropt_t.h"
# include "ircons_t.h"
# include "irgmod.h"
# include "irgwalk.h"
# include "irvrfy.h"
# include "tv_t.h"
# include "dbginfo_t.h"
# include "iropt_dbg.h"
# include "irflag_t.h"
# include "array.h"
# include "firmstat.h"

#undef IMAX
#define IMAX(a,b)	((a) > (b) ? (a) : (b))

#define MAX_PROJ	IMAX(pn_Load_max, pn_Store_max)

/**
 * walker environment
 */
typedef struct _walk_env_t {
  struct obstack obst;		/**< list of all stores */
  int changes;
} walk_env_t;

/**
 * a Load/Store info
 */
typedef struct _ldst_info_t {
  ir_node *projs[MAX_PROJ];	/**< list of Proj's of this node */
  ir_node *exc_block;           /**< the exception block if available */
  int     exc_idx;              /**< predecessor index in the exception block */
} ldst_info_t;

/**
 * flags for control flow
 */
enum block_flags_t {
  BLOCK_HAS_COND = 1,      /**< Block has conditional control flow */
  BLOCK_HAS_EXC  = 2       /**< Block has exceptionl control flow */
};

/**
 * a Block info
 */
typedef struct _block_info_t {
  unsigned flags;               /**< flags for the block */
} block_info_t;

/**
 * walker, clears all links first
 */
static void init_links(ir_node *n, void *env)
{
  set_irn_link(n, NULL);
}

/**
 * get the Load/Store info of a node
 */
static ldst_info_t *get_ldst_info(ir_node *node, walk_env_t *env)
{
  ldst_info_t *info = get_irn_link(node);

  if (! info) {
    info = obstack_alloc(&env->obst, sizeof(*info));

    memset(info, 0, sizeof(*info));

    set_irn_link(node, info);
  }
  return info;
}

/**
 * get the Block info of a node
 */
static block_info_t *get_block_info(ir_node *node, walk_env_t *env)
{
  block_info_t *info = get_irn_link(node);

  if (! info) {
    info = obstack_alloc(&env->obst, sizeof(*info));

    memset(info, 0, sizeof(*info));

    set_irn_link(node, info);
  }
  return info;
}

/**
 * update the projection info for a Load/Store
 */
static int update_projs(ldst_info_t *info, ir_node *proj)
{
  long nr = get_Proj_proj(proj);

  assert(0 <= nr && nr <= MAX_PROJ && "Wrong proj from LoadStore");

  if (info->projs[nr]) {
    /* there is already one, do CSE */
    exchange(proj, info->projs[nr]);
    return 1;
  }
  else {
    info->projs[nr] = proj;
    return 0;
  }
}

/**
 * update the exception block info for a Load/Store
 */
static int update_exc(ldst_info_t *info, ir_node *block, int pos)
{
  assert(info->exc_block == NULL && "more than one exception block found");

  info->exc_block = block;
  info->exc_idx   = pos;
  return 0;
}

/**
 * walker, collects all Load/Store/Proj nodes
 *
 * walks form Start -> End
 */
static void collect_nodes(ir_node *node, void *env)
{
  ir_node     *pred;
  ldst_info_t *ldst_info;
  walk_env_t  *wenv = env;

  if (get_irn_op(node) == op_Proj) {
    pred = get_Proj_pred(node);

    if (get_irn_op(pred) == op_Load || get_irn_op(pred) == op_Store) {
      ldst_info = get_ldst_info(pred, wenv);

      wenv->changes |= update_projs(ldst_info, node);
    }
  }
  else if (get_irn_op(node) == op_Block) { /* check, if it's an exception block */
    int i, n;

    for (i = 0, n = get_Block_n_cfgpreds(node); i < n; ++i) {
      ir_node      *pred_block;
      block_info_t *bl_info;

      pred = skip_Proj(get_Block_cfgpred(node, i));

      /* ignore Bad predecessors, they will be removed later */
      if (is_Bad(pred))
	continue;

      pred_block = get_nodes_block(pred);
      bl_info    = get_block_info(pred_block, wenv);

      if (is_fragile_op(pred))
	bl_info->flags |= BLOCK_HAS_EXC;
      else if (is_forking_op(pred))
	bl_info->flags |= BLOCK_HAS_COND;

      if (get_irn_op(pred) == op_Load || get_irn_op(pred) == op_Store) {
        ldst_info = get_ldst_info(pred, wenv);

        wenv->changes |= update_exc(ldst_info, node, i);
      }
    }
  }
}

/**
 * optimize a Load
 */
static int optimize_load(ir_node *load)
{
  ldst_info_t *info = get_irn_link(load);
  ir_mode *load_mode = get_Load_mode(load);
  ir_node *pred, *mem, *ptr;
  int res = 1;

  if (get_Load_volatility(load) == volatility_is_volatile)
    return 0;

  /*
   * BEWARE: one might think that checking the modes is useless, because
   * if the pointers are identical, they refer to the same object.
   * This is only true in strong typed languages, not is C were the following
   * is possible a = *(type1 *)p; b = *(type2 *)p ...
   */

  ptr  = get_Load_ptr(load);
  mem  = get_Load_mem(load);
  pred = skip_Proj(mem);

  if (! info->projs[pn_Load_res] && ! info->projs[pn_Load_X_except]) {
    /* a Load which value is neither used nor exception checked, remove it */
    exchange(info->projs[pn_Load_M], mem);
    res = 1;
  }
  else if (get_irn_op(pred) == op_Store && get_Store_ptr(pred) == ptr &&
	   get_irn_mode(get_Store_value(pred)) == load_mode) {
    /*
     * a load immediately after a store -- a read after write.
     * We may remove the Load, if it does not have an exception handler
     * OR they are in the same block. In the latter case the Load cannot
     * throw an exception when the previous Store was quiet.
     */
    if (! info->projs[pn_Load_X_except] || get_nodes_block(load) == get_nodes_block(pred)) {
      exchange( info->projs[pn_Load_res], get_Store_value(pred) );
      if (info->projs[pn_Load_M])
	exchange(info->projs[pn_Load_M], mem);

      /* no exception */
      if (info->projs[pn_Load_X_except])
	exchange( info->projs[pn_Load_X_except], new_Bad());
      res = 1;
    }
  }
  else if (get_irn_op(pred) == op_Load && get_Load_ptr(pred) == ptr &&
	   get_Load_mode(pred) == load_mode) {
    /*
     * a load immediately after a load -- a read after read.
     * We may remove the second Load, if it does not have an exception handler
     * OR they are in the same block. In the later case the Load cannot
     * throw an exception when the previous Load was quiet.
     */
    if (! info->projs[pn_Load_X_except] || get_nodes_block(load) == get_nodes_block(pred)) {
      ldst_info_t *pred_info = get_irn_link(pred);

      if (pred_info->projs[pn_Load_res]) {
	/* we need a data proj from the previous load for this optimization */
	exchange( info->projs[pn_Load_res], pred_info->projs[pn_Load_res] );
	if (info->projs[pn_Load_M])
	  exchange(info->projs[pn_Load_M], mem);
      }
      else {
	if (info->projs[pn_Load_res]) {
	  set_Proj_pred(info->projs[pn_Load_res], pred);
	  set_nodes_block(info->projs[pn_Load_res], get_nodes_block(pred));
	}
	if (info->projs[pn_Load_M]) {
	  /* Actually, this if should not be necessary.  Construct the Loads
	     properly!!! */
	  exchange(info->projs[pn_Load_M], mem);
	}
      }

      /* no exception */
      if (info->projs[pn_Load_X_except])
	exchange(info->projs[pn_Load_X_except], new_Bad());

      res = 1;
    }
  }
  return res;
}

/**
 * optimize a Store
 */
static int optimize_store(ir_node *store)
{
  ldst_info_t *info = get_irn_link(store);
  ir_node *pred, *mem, *ptr, *value, *block;
  ir_mode *mode;
  ldst_info_t *pred_info;
  int res = 0;

  if (get_Store_volatility(store) == volatility_is_volatile)
    return 0;

  /*
   * BEWARE: one might think that checking the modes is useless, because
   * if the pointers are identical, they refer to the same object.
   * This is only true in strong typed languages, not is C were the following
   * is possible *(type1 *)p = a; *(type2 *)p = b ...
   */

  block = get_nodes_block(store);
  ptr   = get_Store_ptr(store);
  mem   = get_Store_mem(store);
  value = get_Store_value(store);
  pred  = skip_Proj(mem);
  mode  = get_irn_mode(value);

  pred_info = get_irn_link(pred);

  if (get_irn_op(pred) == op_Store && get_Store_ptr(pred) == ptr &&
      get_nodes_block(pred) == block && get_irn_mode(get_Store_value(pred)) == mode) {
    /*
     * a store immediately after a store in the same block -- a write after write.
     * We may remove the first Store, if it does not have an exception handler.
     *
     * TODO: What, if both have the same exception handler ???
     */
    if (get_Store_volatility(pred) != volatility_is_volatile && !pred_info->projs[pn_Store_X_except]) {
      exchange( pred_info->projs[pn_Store_M], get_Store_mem(pred) );
      res = 1;
    }
  }
  else if (get_irn_op(pred) == op_Load && get_Load_ptr(pred) == ptr &&
	   value == pred_info->projs[pn_Load_res]) {
    /*
     * a store a value immediately after a load -- a write after read.
     * We may remove the second Store, if it does not have an exception handler.
     */
    if (! info->projs[pn_Store_X_except]) {
      exchange( info->projs[pn_Store_M], mem );
      res = 1;
    }
  }
  return res;
}

/**
 * walker, optimizes Phi after Stores:
 * Does the following optimization:
 *
 *   val1   val2   val3          val1  val2  val3
 *    |      |      |               \    |    /
 *   Str    Str    Str               \   |   /
 *      \    |    /                     Phi
 *       \   |   /                       |
 *        \  |  /                       Str
 *          Phi
 *
 * This removes the number of stores and allows for predicated execution.
 * Moves Stores back to the end of a function which may be bad
 *
 * Is only allowed if the predecessor blocks have only one successor.
 */
static int optimize_phi(ir_node *phi)
{
  int i, n;
  ir_node *store, *ptr, *block, *phiM, *phiD, *exc;
  ir_mode *mode;
  ir_node **inM, **inD;
  int *idx;
  dbg_info *db = NULL;
  ldst_info_t *info;
  block_info_t *bl_info;

  /* Must be a memory Phi */
  if (get_irn_mode(phi) != mode_M)
    return 0;

  n = get_Phi_n_preds(phi);
  if (n <= 0)
    return 0;

  store = skip_Proj(get_Phi_pred(phi, 0));
  if (get_irn_op(store) != op_Store)
    return 0;

  /* abort on bad blocks */
  if (is_Bad(get_nodes_block(store)))
    return 0;

  /* check if the block has only one output */
  bl_info = get_irn_link(get_nodes_block(store));
  if (bl_info->flags)
    return 0;

  /* this is the address of the store */
  ptr  = get_Store_ptr(store);
  mode = get_irn_mode(get_Store_value(store));
  info = get_irn_link(store);
  exc  = info->exc_block;

  for (i = 1; i < n; ++i) {
    ir_node *pred = skip_Proj(get_Phi_pred(phi, i));

    if (get_irn_op(pred) != op_Store)
      return 0;

    if (mode != get_irn_mode(get_Store_value(pred)) || ptr != get_Store_ptr(pred))
      return 0;

    info = get_irn_link(pred);

    /* check, if all stores have the same exception flow */
    if (exc != info->exc_block)
      return 0;

    /* abort on bad blocks */
    if (is_Bad(get_nodes_block(store)))
      return 0;

    /* check if the block has only one output */
    bl_info = get_irn_link(get_nodes_block(store));
    if (bl_info->flags)
      return 0;
  }

  /*
   * ok, when we are here, we found all predecessors of a Phi that
   * are Stores to the same address. That means whatever we do before
   * we enter the block of the Phi, we do a Store.
   * So, we can move the store to the current block:
   *
   *   val1    val2    val3          val1  val2  val3
   *    |       |       |               \    |    /
   * | Str | | Str | | Str |             \   |   /
   *      \     |     /                     Phi
   *       \    |    /                       |
   *        \   |   /                       Str
   *           Phi
   *
   * Is only allowed if the predecessor blocks have only one successor.
   */

  /* first step: collect all inputs */
  NEW_ARR_A(ir_node *, inM, n);
  NEW_ARR_A(ir_node *, inD, n);
  NEW_ARR_A(int, idx, n);

  for (i = 0; i < n; ++i) {
    ir_node *pred = skip_Proj(get_Phi_pred(phi, i));
    info = get_irn_link(pred);

    inM[i] = get_Store_mem(pred);
    inD[i] = get_Store_value(pred);
    idx[i] = info->exc_idx;
  }
  block = get_nodes_block(phi);

  /* second step: create a new memory Phi */
  phiM = new_rd_Phi(get_irn_dbg_info(phi), current_ir_graph, block, n, inM, mode_M);

  /* third step: create a new data Phi */
  phiD = new_rd_Phi(get_irn_dbg_info(phi), current_ir_graph, block, n, inD, mode);

  /* fourth step: create the Store */
  store = new_rd_Store(db, current_ir_graph, block, phiM, ptr, phiD);

  /* fifths step: repair exception flow */
  if (exc) {
    ir_node *projX = new_rd_Proj(NULL, current_ir_graph, block, store, mode_X, pn_Store_X_except);

    for (i = 0; i < n; ++i) {
      set_Block_cfgpred(exc, idx[i], projX);
    }

    if (n > 1) {
      /* the exception block should be optimized as some inputs are identical now */
    }
  }

  /* sixt step: replace old Phi */
  exchange(phi, new_rd_Proj(NULL, current_ir_graph, block, store, mode_M, pn_Store_M));

  return 1;
}

/**
 * walker, collects all Load/Store/Proj nodes
 */
static void do_load_store_optimize(ir_node *n, void *env)
{
  walk_env_t *wenv = env;

  switch (get_irn_opcode(n)) {

  case iro_Load:
    wenv->changes |= optimize_load(n);
    break;

  case iro_Store:
    wenv->changes |= optimize_store(n);
    break;

//  case iro_Phi:
//    wenv->changes |= optimize_phi(n);

  default:
    ;
  }
}

/*
 * do the load store optimization
 */
void optimize_load_store(ir_graph *irg)
{
  walk_env_t env;

  assert(get_irg_phase_state(irg) != phase_building);

  if (!get_opt_redundant_LoadStore())
    return;

  obstack_init(&env.obst);
  env.changes = 0;

  /* init the links, then collect Loads/Stores/Proj's in lists */
  irg_walk_graph(irg, init_links, collect_nodes, &env);

  /* now we have collected enough information, optimize */
  irg_walk_graph(irg, NULL, do_load_store_optimize, &env);

  obstack_free(&env.obst, NULL);

  /* Handle graph state */
  if (env.changes) {
    if (get_irg_outs_state(current_ir_graph) == outs_consistent)
      set_irg_outs_inconsistent(current_ir_graph);

    /* is this really needed: Yes, as exception block may get bad but this might be tested */
    if (get_irg_dom_state(current_ir_graph) == dom_consistent)
      set_irg_dom_inconsistent(current_ir_graph);
  }
}
