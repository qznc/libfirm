/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief       Handling of ia32 specific firm opcodes.
 * @author      Christian Wuerdig
 *
 * This file implements the creation of the achitecture specific firm opcodes
 * and the coresponding node constructors for the ia32 assembler irg.
 */
#ifndef FIRM_BE_IA32_IA32_NEW_NODES_H
#define FIRM_BE_IA32_IA32_NEW_NODES_H

#include "ia32_nodes_attr.h"

/** indices for AM inputs */
enum {
	n_ia32_base         = 0,
	n_ia32_index        = 1,
	n_ia32_mem          = 2,
	n_ia32_unary_op     = 3,
	n_ia32_binary_left  = 3,
	n_ia32_binary_right = 4
};

/** proj numbers for "normal" one-result nodes (for the complicated cases where we not only
 * need the result) */
enum {
	pn_ia32_res   = 0,
	pn_ia32_flags = 1,
	pn_ia32_mem   = 2
};

extern struct obstack opcodes_obst;

/**
 * Returns the attributes of an ia32 node.
 */
ia32_attr_t *get_ia32_attr(ir_node *node);
const ia32_attr_t *get_ia32_attr_const(const ir_node *node);

ia32_x87_attr_t *get_ia32_x87_attr(ir_node *node);
const ia32_x87_attr_t *get_ia32_x87_attr_const(const ir_node *node);

ia32_immediate_attr_t *get_ia32_immediate_attr(ir_node *node);
const ia32_immediate_attr_t *get_ia32_immediate_attr_const(const ir_node *node);

const ia32_asm_attr_t *get_ia32_asm_attr_const(const ir_node *node);

/**
 * Gets the condcode attributes of a node.
 */
ia32_condcode_attr_t *get_ia32_condcode_attr(ir_node *node);
const ia32_condcode_attr_t *get_ia32_condcode_attr_const(const ir_node *node);

/**
 * Gets the Call node attributes.
 */
ia32_call_attr_t *get_ia32_call_attr(ir_node *node);
const ia32_call_attr_t *get_ia32_call_attr_const(const ir_node *node);

/**
 * Gets the CopyB node attributes.
 */
ia32_copyb_attr_t *get_ia32_copyb_attr(ir_node *node);
const ia32_copyb_attr_t *get_ia32_copyb_attr_const(const ir_node *node);

/**
 * Gets the ClimbFrame node attributes.
 */
ia32_climbframe_attr_t *get_ia32_climbframe_attr(ir_node *node);
const ia32_climbframe_attr_t *get_ia32_climbframe_attr_const(const ir_node *node);

ia32_switch_attr_t *get_ia32_switch_attr(ir_node *node);
const ia32_switch_attr_t *get_ia32_switch_attr_const(const ir_node *node);

/**
 * Gets the type of an ia32 node.
 */
ia32_op_type_t get_ia32_op_type(const ir_node *node);

/**
 * Sets the type of an ia32 node.
 */
void set_ia32_op_type(ir_node *node, ia32_op_type_t tp);

/**
 * Gets the supported address mode of an ia32 node
 */
ia32_am_type_t get_ia32_am_support(const ir_node *node);

/**
 * Sets the supported addrmode of an ia32 node
 */
void set_ia32_am_support(ir_node *node, ia32_am_type_t am_arity);

/**
 * Gets the addressmode offset as long.
 */
int get_ia32_am_offs_int(const ir_node *node);

/**
 * Sets the addressmode offset
 */
void set_ia32_am_offs_int(ir_node *node, int offset);

void add_ia32_am_offs_int(ir_node *node, int offset);

/**
 * Returns the symconst entity associated to addrmode.
 */
ir_entity *get_ia32_am_sc(const ir_node *node);

/**
 * Sets the symconst entity associated to addrmode.
 */
void set_ia32_am_sc(ir_node *node, ir_entity *sc);

/**
 * Sets the sign bit for address mode symconst.
 */
void set_ia32_am_sc_sign(ir_node *node);

/**
 * Clears the sign bit for address mode symconst.
 */
void clear_ia32_am_sc_sign(ir_node *node);

/**
 * Returns the sign bit for address mode symconst.
 */
int is_ia32_am_sc_sign(const ir_node *node);

void set_ia32_am_tls_segment(ir_node *node, bool value);

bool get_ia32_am_tls_segment(const ir_node *node);

/**
 * Gets the addr mode const.
 */
unsigned get_ia32_am_scale(const ir_node *node);

/**
 * Sets the const for addr mode.
 */
void set_ia32_am_scale(ir_node *node, unsigned scale);

/**
 * Sets the uses_frame flag.
 */
void set_ia32_use_frame(ir_node *node);

/**
 * Clears the uses_frame flag.
 */
void clear_ia32_use_frame(ir_node *node);

/**
 * Gets the uses_frame flag.
 */
int is_ia32_use_frame(const ir_node *node);

/**
 * copies all address-mode attributes from one node to the other
 */
void ia32_copy_am_attrs(ir_node *to, const ir_node *from);

/**
 * Sets node to commutative.
 */
void set_ia32_commutative(ir_node *node);

/**
 * Sets node to non-commutative.
 */
void clear_ia32_commutative(ir_node *node);

/**
 * Checks if node is commutative.
 */
int is_ia32_commutative(const ir_node *node);

/**
 * Sets node needs_stackent
 */
void set_ia32_need_stackent(ir_node *node);

/**
 * Clears node needs_stackent
 */
void clear_ia32_need_stackent(ir_node *node);

/**
 * Checks if node needs a stack entity assigned
 */
int is_ia32_need_stackent(const ir_node *node);

void set_ia32_is_reload(ir_node *node);
int is_ia32_is_reload(const ir_node *node);

void set_ia32_is_spill(ir_node *node);
int is_ia32_is_spill(const ir_node *node);

void set_ia32_is_remat(ir_node *node);
int is_ia32_is_remat(const ir_node *node);

/**
 * Gets the mode of the stored/loaded value (only set for Store/Load)
 */
ir_mode *get_ia32_ls_mode(const ir_node *node);

/**
 * Sets the mode of the stored/loaded value (only set for Store/Load)
 */
void set_ia32_ls_mode(ir_node *node, ir_mode *mode);

/**
 * Gets the frame entity assigned to this node;
 */
ir_entity *get_ia32_frame_ent(const ir_node *node);

/**
 * Sets the frame entity for this node;
 */
void set_ia32_frame_ent(ir_node *node, ir_entity *ent);

/**
 * Returns the condition code of a node.
 */
ia32_condition_code_t get_ia32_condcode(const ir_node *node);

/**
 * Sets the condition code of a node
 */
void set_ia32_condcode(ir_node *node, ia32_condition_code_t code);

const ir_switch_table *get_ia32_switch_table(const ir_node *node);

unsigned get_ia32_copyb_size(const ir_node *node);

/**
 * Gets the instruction latency.
 */
unsigned get_ia32_latency(const ir_node *node);

/**
 * Get the exception label attribute.
 */
unsigned get_ia32_exc_label(const ir_node *node);

/**
 * Set the exception label attribute.
 */
void set_ia32_exc_label(ir_node *node, unsigned flag);

/**
 * Return the exception label id.
 */
ir_label_t get_ia32_exc_label_id(const ir_node *node);

/**
 * Assign the exception label id.
 */
void set_ia32_exc_label_id(ir_node *node, ir_label_t id);

#ifndef NDEBUG

/**
 * Sets the name of the original ir node.
 */
void set_ia32_orig_node(ir_node *node, const ir_node *old);

#endif /* NDEBUG */

/**
 * Swaps left/right input of a node (and sets ins_permuted accordingly)
 */
void ia32_swap_left_right(ir_node *node);

/* Include the generated headers */
#include "gen_ia32_new_nodes.h"

#endif
