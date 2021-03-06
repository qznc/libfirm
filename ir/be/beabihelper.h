/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief       Helper functions for handling ABI constraints in the code
 *              selection phase.
 * @author      Matthias Braun
 */
#ifndef FIRM_BE_BEABI_HELPER_H
#define FIRM_BE_BEABI_HELPER_H

#include "firm_types.h"
#include "be_types.h"
#include "bearch.h"

typedef struct beabi_helper_env_t beabi_helper_env_t;
typedef struct be_stackorder_t    be_stackorder_t;

/**
 * Creates a helper object for the ABI constraint handling.
 */
beabi_helper_env_t *be_abihelper_prepare(ir_graph *irg);

/**
 * Terminates a helper object for the ABI constraint handling.
 */
void be_abihelper_finish(beabi_helper_env_t *env);

/**
 * Mark a registers value at the beginning of the function as significant.
 * This is necessary for things like:
 *  - Callee-Save registers (we need to restore that value at the end)
 *  - Parameters passed in registers
 *  - stack pointer, base pointer, ...
 * It is possible to specify additional irn flags (useful to mark a value
 * as ignore or produces_sp).
 */
void be_prolog_add_reg(beabi_helper_env_t *env, const arch_register_t *reg,
                       arch_register_req_type_t flags);

/**
 * Creates a start node.
 * Must be called after all prolog_add_reg calls
 */
ir_node *be_prolog_create_start(beabi_helper_env_t *env, dbg_info *dbgi,
                                ir_node *block);

/**
 * Get "value" of a register.
 * This usually creates a Proj node for the start-node.
 * Or returns the value set by a abi_helper_set_reg_value call
 */
ir_node *be_prolog_get_reg_value(beabi_helper_env_t *env,
                                 const arch_register_t *reg);

ir_node *be_prolog_get_memory(beabi_helper_env_t *env);

/**
 * Set current register value.
 */
void be_prolog_set_reg_value(beabi_helper_env_t *env,
                             const arch_register_t *reg, ir_node *value);

void be_prolog_set_memory(beabi_helper_env_t *env, ir_node *value);

/**
 * Set value of register at the end of the function. Necessary for:
 *  - Callee-save registers
 *  - Return values in registers
 *  - stack pointer, base pointer
 */
void be_epilog_add_reg(beabi_helper_env_t *env, const arch_register_t *reg,
                       arch_register_req_type_t flags, ir_node *value);

void be_epilog_set_reg_value(beabi_helper_env_t *env,
                             const arch_register_t *reg, ir_node *value);

ir_node *be_epilog_get_reg_value(beabi_helper_env_t *env,
                             const arch_register_t *reg);

void be_epilog_set_memory(beabi_helper_env_t *env, ir_node *value);

ir_node *be_epilog_get_memory(beabi_helper_env_t *env);

void be_epilog_begin(beabi_helper_env_t *env);

/**
 * Create return node and finishes epilog handling
 */
ir_node *be_epilog_create_return(beabi_helper_env_t *env, dbg_info *dbgi,
                                 ir_node *block);

/**
 * Adds a X->Proj->Keep for each output value of X which has no Proj yet
 */
void be_add_missing_keeps(ir_graph *irg);

/**
 * Make sure all outputs of a node are used, add keeps otherwise
 */
void be_add_missing_keeps_node(ir_node *node);

/**
 * In the normal firm representation some nodes like pure calls, builtins
 * have no memory inputs+outputs. However in the backend these sometimes have to
 * access the stack to work and therefore suddenly need to be enqueued into the
 * memory edge again.
 * This API creates a possible order to enqueue them so we can be sure to create
 * a legal dependency graph when transforming them.
 */
be_stackorder_t *be_collect_stacknodes(ir_graph *irg);

/**
 * return node that should produce the predecessor stack node in a block.
 * returns NULL if there's no predecessor in the current block.
 */
ir_node *be_get_stack_pred(const be_stackorder_t *env, const ir_node *node);

/**
 * free memory associated with a stackorder structure
 */
void be_free_stackorder(be_stackorder_t *env);

/**
 * In case where a parameter is transmitted via register but someone takes its
 * address a store to the frame which can be references is necessary.
 * This function can be used as a preprocessing phase before transformation to
 * do this. The assumption is that all parameter_entities which are passed
 * through the stack are already moved to the arg_type and all remaining
 * parameter_entities on the frame type need stores.
 */
void be_add_parameter_entity_stores(ir_graph *irg);

#endif
