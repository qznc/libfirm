/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief   iropt --- optimizations of an ir node.
 * @author  Martin Trapp, Christian Schaefer, Goetz Lindenmaier, Michael Beck
 */
#ifndef FIRM_IR_IROPT_H
#define FIRM_IR_IROPT_H

#include "firm_types.h"
#include "begin.h"

/**
 * @ingroup iroptimize
 * @defgroup iropt  Local Optimizations
 * @{
 */

/**
 * The Floating point model.
 *
 * Several basic properties are defined:
 * - fp_explicit_rounding
 * - fp_strict_algebraic
 * - fp_contradictions
 * - fp_strict_eval_order
 * - fp_exceptions
 * - fp_environment_access
 *
 * From those basic properties three general models are defined,
 * compatible to the VC8 compiler:
 * - fp_model_precise:
 *     Default mode. Associative and distributive law forbidden unless a transformation
 *     is guaranteed to produce the same result.
 *     No FPU environment access. No FP exception semantics.
 * - fp_model_strict:
 *     Slowest mode. Additionally to fp_model_precise allows correct handling of
 *     FP exceptions and FPU environment access.
 * - fp_model_fast:
 *     Fastest mode. Associative and distributive law allowed at the expense
 *     of floating point accuracy and correctness. Explicit rounding is disabled.
 */
typedef enum fp_model_t {
	fp_explicit_rounding  = (1u << 0),  /**< Explicit rounding at assignments, typecasts, return
	                                  and function calls. Conv nodes may NOT be removed, even
	                                  if they look useless. */
	fp_strict_algebraic   = (1u << 1),  /**< Strict adherence to non-associative and non-distributive
	                                  algebra unless the same result is guaranteed. */
	fp_contradictions     = (1u << 2),  /**< FP contradictions are enabled. Only for backend. */
	fp_strict_eval_order  = (1u << 3),  /**< FP instructions must be strict evaluated in given order. */
	fp_exceptions         = (1u << 4),  /**< FP exceptions are supported. No reordering that changes
	                                  the exception flow are allowed. Backends must generate
	                                  synchronized exception code. */
	fp_environment_access = (1u << 5),  /**< FPU environment can be accessed. Even Constant folding
	                                  cannot be done. */

	/** Precise floating point model. Default. */
	fp_model_precise = fp_explicit_rounding|fp_strict_algebraic|fp_contradictions,
	/** Strict floating point model. */
	fp_model_strict  = fp_explicit_rounding|fp_strict_algebraic|fp_strict_eval_order|
	                   fp_exceptions|fp_environment_access,
	/** Fast floating point model. */
	fp_model_fast    = fp_contradictions
} fp_model_t;

/** If the expression referenced can be evaluated statically
 *  computed_value returns a tarval representing the result.
 *  Else returns tarval_bad. */
FIRM_API ir_tarval *computed_value(const ir_node *n);

/** Applies all optimizations to n that are expressible as a pattern
 *  in Firm, i.e., they need not a walk of the graph.
 *  Returns a better node for n.  Does not free n -- other nodes could
 *  reference n.
 *
 *  An equivalent optimization is applied in the constructors defined in
 *  ircons.ch.  There n is freed if a better node could be found.
 */
FIRM_API ir_node *optimize_in_place(ir_node *n);

/**
 * checks whether 1 value is the negated other value
 */
FIRM_API int ir_is_negated_value(const ir_node *a, const ir_node *b);

/**
 * (conservatively) approximates all possible relations when comparing
 * the value @p left and @p right
 */
FIRM_API ir_relation ir_get_possible_cmp_relations(const ir_node *left,
                                                   const ir_node *right);

/** @} */

#include "end.h"

#endif
