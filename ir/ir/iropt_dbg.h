/*
 * Project:     libFIRM
 * File name:   ir/ir/iropt_dbg.h
 * Purpose:     Debug makros used in iropt.
 * Author:      Goetz Lindenmaier
 * Modified by:
 * Created:
 * CVS-ID:      $Id$
 * Copyright:   (c) 2001-2003 Universitšt Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */


/* This file contains makros that generate the calls to
   update the debug information after a transformation. */

#define SIZ(x)    sizeof(x)/sizeof((x)[0])


/**
 * Merge the debug info due to dead code elimination
 */
#define DBG_OPT_DEAD(oldn, n)                                      \
  do {                                                             \
	  ir_node *ons[2];                                         \
	  ons[0] = oldn;                                           \
	  ons[1] = get_Block_cfgpred(oldn, 0);                     \
	  stat_merge_nodes(&n, 1, ons, SIZ(ons), STAT_OPT_STG);	   \
	  __dbg_info_merge_sets(&n, 1, ons, SIZ(ons), dbg_dead_code); \
	} while(0)


/**
 * Merge the debug info due to a straightening optimization
 */
#define DBG_OPT_STG(oldn, n)                                       \
  do {                                                             \
	  ir_node *ons[2];                                         \
	  ons[0] = oldn;                                           \
	  ons[1] = get_Block_cfgpred(oldn, 0);                     \
	  stat_merge_nodes(&n, 1, ons, SIZ(ons), STAT_OPT_STG);	   \
	  __dbg_info_merge_sets(&n, 1, ons, SIZ(ons), dbg_straightening); \
	} while(0)

/**
 * Merge the debug info due to an if simplification
 */
#define DBG_OPT_IFSIM(oldn, a, b, n)                                  \
  do {                                                                \
	  ir_node *ons[4];                                            \
	  ons[0] = oldn;                                              \
	  ons[1] = a;                                                 \
	  ons[2] = b;                                                 \
	  ons[3] = get_Proj_pred(a);                                  \
	  stat_merge_nodes(&n, 1, ons, SIZ(ons), STAT_OPT_IFSIM);     \
	  __dbg_info_merge_sets(&n, 1, ons, SIZ(ons), dbg_if_simplification); \
	} while(0)

/**
 * Merge the debug info due to an algebraic_simplification
 */
#define DBG_OPT_CSTEVAL(oldn, n)                                     	\
  do {                                                          	\
	  stat_merge_nodes(&n, 1, &oldn, 1, STAT_OPT_CONST_EVAL);    	\
          __dbg_info_merge_pair(n, oldn, dbg_const_eval);	        \
  } while(0)

#define DBG_OPT_ALGSIM1(oldn, a, b, n)                                \
  do {                                                                \
	  ir_node *ons[3];                                            \
	  ons[0] = oldn;                                              \
	  ons[1] = a;                                                 \
	  ons[2] = b;                                                 \
	  stat_merge_nodes(&n, 1, ons, SIZ(ons), STAT_OPT_ALGSIM);    \
	  __dbg_info_merge_sets(&n, 1, ons, SIZ(ons), dbg_algebraic_simplification); \
  } while(0)

#define DBG_OPT_ALGSIM2(oldn, pred, n)                                \
  do {                                                                \
	  ir_node *ons[3];                                            \
	  ons[0] = oldn;                                              \
	  ons[1] = pred;                                              \
	  ons[2] = n;                                                 \
	  stat_merge_nodes(&n, 1, ons, SIZ(ons), STAT_OPT_ALGSIM);    \
	  __dbg_info_merge_sets(&n, 1, ons, SIZ(ons), dbg_algebraic_simplification); \
  } while(0)

#define DBG_OPT_ALGSIM3(oldn, a, n)                                   \
  do {                                                                \
	  ir_node *ons[2];                                            \
	  ons[0] = oldn;                                              \
	  ons[1] = a;                                                 \
	  stat_merge_nodes(&n, 1, ons, SIZ(ons), STAT_OPT_ALGSIM);    \
	  __dbg_info_merge_sets(&n, 1, ons, SIZ(ons), dbg_algebraic_simplification); \
  } while(0)

#define DBG_OPT_PHI(oldn, first_val, n)                               \
  do {                                                                \
	  ir_node *ons[2];                                            \
	  ons[0] = oldn;                                              \
	  ons[1] = first_val;                                         \
	  stat_merge_nodes(&n, 1, ons, SIZ(ons), STAT_OPT_PHI);	      \
	  __dbg_info_merge_sets(&n, 1, ons, SIZ(ons), dbg_opt_ssa);   \
  } while(0)


/**
 * Merge the debug info due to Write-after-Write optimization:
 * Store oldst will be replace by a reference to Store st
 */
#define DBG_OPT_WAW(oldst, st)                                        \
  do {                                                                \
	  ir_node *ons[2];                                            \
	  ons[0] = oldst;                                             \
	  ons[1] = st;                                                \
	  stat_merge_nodes(&st, 1, ons, SIZ(ons), STAT_OPT_WAW);      \
	  __dbg_info_merge_sets(&st, 1, ons, SIZ(ons), dbg_write_after_write); \
  } while(0)

/**
 * Merge the debug info due to Write-after-Read optimization:
 * store will be replace by a reference to load
 */
#define DBG_OPT_WAR(store, load)                                      \
  do {                                                                \
	  ir_node *ons[2];                                            \
	  ons[0] = store;                                             \
	  ons[1] = load;                                              \
	  stat_merge_nodes(&load, 1, ons, SIZ(ons), STAT_OPT_WAR);    \
	  __dbg_info_merge_sets(&load, 1, ons, SIZ(ons), dbg_write_after_read); \
  } while(0)

/**
 * Merge the debug info due to Read-after-Write optimization:
 * load will be replace by a reference to store
 */
#define DBG_OPT_RAW(store, load)                                      \
  do {                                                                \
	  ir_node *ons[2];                                            \
	  ons[0] = store;                                             \
	  ons[1] = load;                                              \
	  stat_merge_nodes(&store, 1, ons, SIZ(ons), STAT_OPT_RAW);   \
	  __dbg_info_merge_sets(&store, 1, ons, SIZ(ons), dbg_read_after_write); \
  } while(0)

/**
 * Merge the debug info due to Read-after-Read optimization:
 * Load oldld will be replace by a reference to Load ld
 */
#define DBG_OPT_RAR(oldld, ld)                                        \
  do {                                                                \
	  ir_node *ons[2];                                            \
	  ons[0] = oldld;                                             \
	  ons[1] = ld;                                                \
	  stat_merge_nodes(&ld, 1, ons, SIZ(ons), STAT_OPT_RAR);      \
	  __dbg_info_merge_sets(&ld, 1, ons, SIZ(ons), dbg_read_after_read); \
  } while(0)

/**
 * Merge the debug info due to Read-a-Const optimization:
 * Load ld will be replace by a Constant
 */
#define DBG_OPT_RC(ld, c)                                             \
  do {                                                                \
	  ir_node *ons[2];                                            \
	  ons[0] = ld;                                                \
	  ons[1] = c;                                                 \
	  stat_merge_nodes(&c, 1, ons, SIZ(ons), STAT_OPT_RC);        \
	  __dbg_info_merge_sets(&ld, 1, ons, SIZ(ons), dbg_read_a_const); \
	} while(0)

#define DBG_OPT_TUPLE(oldn, a, n)                                     \
  do {                                                                \
	  ir_node *ons[3];                                            \
	  ons[0] = oldn;                                              \
	  ons[1] = a;                                                 \
	  ons[2] = n;                                                 \
	  stat_merge_nodes(&n, 1, ons, SIZ(ons), STAT_OPT_TUPLE);     \
	  __dbg_info_merge_sets(&n, 1, ons, SIZ(ons), dbg_opt_auxnode);      \
  } while(0)

#define DBG_OPT_ID(oldn, n)                                           \
  do {                                                                \
	  ir_node *ons[2];                                            \
	  ons[0] = oldn;                                              \
	  ons[1] = n;                                                 \
	  stat_merge_nodes(&n, 1, ons, SIZ(ons), STAT_OPT_ID);	      \
	  __dbg_info_merge_sets(&n, 1, ons, SIZ(ons), dbg_opt_auxnode);      \
  } while(0)

/**
 * Merge the debug info due to ommon-subexpression elimination
 */
#define DBG_OPT_CSE(oldn, n)                                          \
  do {                                                                \
	  ir_node *ons[2];                                            \
	  ons[0] = oldn;                                              \
	  ons[1] = n;                                                 \
	  stat_merge_nodes(&n, 1, ons, SIZ(ons), STAT_OPT_CSE);	      \
	  __dbg_info_merge_sets(&n, 1, ons, SIZ(ons), dbg_opt_cse);   \
  } while(0)

#define DBG_OPT_POLY_ALLOC(oldn, n)                                 \
  do {                                                              \
    ir_node *ons[3];                                                \
    ons[0] = oldn;                                                  \
    ons[1] = skip_Proj(get_Sel_ptr(oldn));                          \
    ons[2] = n;                                                     \
    stat_merge_nodes(&n, 1, ons, SIZ(ons), STAT_OPT_POLY_CALL);	    \
    __dbg_info_merge_sets(&n, 1, ons, SIZ(ons), dbg_rem_poly_call); \
  } while(0)

#define DBG_OPT_POLY(oldn, n)                                   \
  do {                                                          \
    stat_merge_nodes(&n, 1, &oldn, 1, STAT_OPT_POLY_CALL);    	\
    __dbg_info_merge_pair(n, oldn, dbg_rem_poly_call);          \
  } while(0)
