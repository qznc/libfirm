/*
 * Project:     libFIRM
 * File name:   ir/ir/irgraph.c
 * Purpose:     Flags to control optimizations, inline implementation.
 * Author:      Michael Beck
 * Created:
 * CVS-ID:      $Id$
 * Copyright:   (c) 1998-2004 Universitšt Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */

/**
 * @file irflag_t.h
 *
 * Inline implementation of Optimization flags.
 *
 * @author Michael Beck
 */
#ifndef _IRFLAG_T_H_
#define _IRFLAG_T_H_

#include "irflag.h"

/**
 * current libFIRM optimizations
 */
typedef enum {
  /** Common subexpression eliminations: Hash the nodes. */
  OPT_CSE                                = 0x00000001,

  /** Don't use block predecessor for comparison.
   *  Default must be zero as code placement must
   *  be run right after a local optimize walk with
   *  opt_global_cse on. */
  OPT_GLOBAL_CSE                         = 0x00000002,

  /** Evaluate operations. */
  OPT_CONSTANT_FOLDING                   = 0x00000004,

  /** Bad node propagation. */
  OPT_UNREACHABLE_CODE                   = 0x00000008,

  /** */
  OPT_CONTROL_FLOW_STRAIGHTENING         = 0x00000010,

  /** */
  OPT_CONTROL_FLOW_WEAK_SIMPLIFICATION   = 0x00000020,

  /** */
  OPT_CONTROL_FLOW_STRONG_SIMPLIFICATION = 0x00000040,

  /** */
  OPT_CRITICAL_EDGES                     = 0x00000080,

  /** Reclaim memory. */
  OPT_DEAD_NODE_ELIMINATION              = 0x00000100,

  /** Reassociate nodes. */
  OPT_REASSOCIATION                      = 0x00000200,

  /** Do inlining transformation. */
  OPT_INLINE                             = 0x00000400,

  /** Remove dynamic method dispatch. */
  OPT_DYN_METH_DISPATCH                  = 0x00000800,

  /** Transformations that normalize the firm representation
   *  as removing Ids and Tuples, useless Phis, SymConst(id) -> Const(entity) ...
   */
  OPT_NORMALIZE                          = 0x00001000,

  /** Remove tail-recursion. */
  OPT_TAIL_RECURSION                     = 0x00002000,

  /** Turn off all optimizations. */
  OPT_OPTIMIZED                          = 0x40000000,
} libfirm_opts_t;

extern optimization_state_t libFIRM_opt;

/** Returns constant folding optimization setting. */
static INLINE int get_opt_cse(void)
{
  return libFIRM_opt & OPT_CSE;
}

/** Returns constant subexpression elimination setting. */
static INLINE int get_opt_global_cse(void)
{
  return libFIRM_opt & OPT_GLOBAL_CSE;
}

/** Returns global constant subexpression elimination setting. */
static INLINE int get_opt_constant_folding(void)
{
  return libFIRM_opt & OPT_CONSTANT_FOLDING;
}

/** Returns unreachable code elimination setting. */
static INLINE int get_opt_unreachable_code(void)
{
  return libFIRM_opt & OPT_UNREACHABLE_CODE;
}

/** Returns Straightening setting. */
static INLINE int get_opt_control_flow_straightening(void)
{
  return libFIRM_opt & OPT_CONTROL_FLOW_STRAIGHTENING;
}

/** Returns if simplifications in local optimizations setting. */
static INLINE int get_opt_control_flow_weak_simplification(void)
{
  return libFIRM_opt & OPT_CONTROL_FLOW_WEAK_SIMPLIFICATION;
}

/** Returns strong if and loop simplification setting */
static INLINE int get_opt_control_flow_strong_simplification(void)
{
  return libFIRM_opt & OPT_CONTROL_FLOW_STRONG_SIMPLIFICATION;
}

/** Returns whether critical edges are removed */
static INLINE int get_opt_critical_edges(void)
{
  return libFIRM_opt & OPT_CRITICAL_EDGES;
}

/** Returns reassociation setting. */
static INLINE int get_opt_reassociation(void)
{
  return libFIRM_opt & OPT_REASSOCIATION;
}

/** Returns dead node elimination setting. */
static INLINE int get_opt_dead_node_elimination(void)
{
  return libFIRM_opt & OPT_DEAD_NODE_ELIMINATION;
}

/** Returns global optimization setting */
static INLINE int get_opt_optimize(void)
{
  return libFIRM_opt & OPT_OPTIMIZED;
}

/** Returns inlining setting. */
static INLINE int get_opt_inline(void)
{
  return libFIRM_opt & OPT_INLINE;
}

static INLINE int get_opt_dyn_meth_dispatch(void)
{
  return libFIRM_opt & OPT_DYN_METH_DISPATCH;
}

static INLINE int get_opt_normalize(void)
{
  return libFIRM_opt & OPT_NORMALIZE;
}

/** Returns tail-recursion setting. */
static INLINE int get_opt_tail_recursion(void)
{
  return libFIRM_opt & OPT_TAIL_RECURSION;
}

#endif /* _IRFLAG_T_H_ */
