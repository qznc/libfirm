/*
 * Copyright (C) 1995-2008 University of Karlsruhe.  All right reserved.
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
 * @file
 * @brief    This file implements the creation of the architecture specific firm
 *           opcodes and the corresponding node constructors for the MIPS
 *           assembler irg.
 * @author   Matthias Braun, Mehdi
 * @version  $Id$
 */
#include "config.h"

#include <stdlib.h>

#include "irprog_t.h"
#include "irgraph_t.h"
#include "irnode_t.h"
#include "irmode_t.h"
#include "ircons_t.h"
#include "iropt_t.h"
#include "irop.h"
#include "irvrfy_t.h"
#include "irprintf.h"
#include "xmalloc.h"

#include "../bearch.h"

#include "mips_nodes_attr.h"
#include "mips_new_nodes.h"
#include "gen_mips_regalloc_if.h"



/***********************************************************************************
 *      _                                   _       _             __
 *     | |                                 (_)     | |           / _|
 *   __| |_   _ _ __ ___  _ __   ___ _ __   _ _ __ | |_ ___ _ __| |_ __ _  ___ ___
 *  / _` | | | | '_ ` _ \| '_ \ / _ \ '__| | | '_ \| __/ _ \ '__|  _/ _` |/ __/ _ \
 * | (_| | |_| | | | | | | |_) |  __/ |    | | | | | ||  __/ |  | || (_| | (_|  __/
 *  \__,_|\__,_|_| |_| |_| .__/ \___|_|    |_|_| |_|\__\___|_|  |_| \__,_|\___\___|
 *                       | |
 *                       |_|
 ***********************************************************************************/

/**
 * Dumper interface for dumping mips nodes in vcg.
 * @param n        the node to dump
 * @param F        the output file
 * @param reason   indicates which kind of information should be dumped
 * @return 0 on success or != 0 on failure
 */
static int mips_dump_node(ir_node *n, FILE *F, dump_reason_t reason)
{
	switch (reason) {
		case dump_node_opcode_txt:
			fprintf(F, "%s", get_irn_opname(n));
			break;

		case dump_node_mode_txt:
			break;

		case dump_node_nodeattr_txt:

			if(is_mips_Immediate(n)) {
				const mips_immediate_attr_t *attr
					= get_mips_immediate_attr_const(n);
				switch(attr->imm_type) {
				case MIPS_IMM_CONST:
					fprintf(F, " %ld ", attr->val);
					break;
				case MIPS_IMM_SYMCONST_LO:
					fprintf(F, " lo(%s", get_entity_ld_name(attr->entity));
					if(attr->val != 0) {
						fprintf(F, "%+ld", attr->val);
					}
					fprintf(F, ") ");
					break;
				case MIPS_IMM_SYMCONST_HI:
					fprintf(F, " hi(%s", get_entity_ld_name(attr->entity));
					if(attr->val != 0) {
						fprintf(F, "%+ld", attr->val);
					}
					fprintf(F, ") ");
					break;
				default:
					fprintf(F, " INVALID ");
					break;
				}
			}
			break;

		case dump_node_info_txt:
			arch_dump_reqs_and_registers(F, n);
			break;
	}

	return 0;
}



/***************************************************************************************************
 *        _   _                   _       __        _                    _   _               _
 *       | | | |                 | |     / /       | |                  | | | |             | |
 *   __ _| |_| |_ _ __   ___  ___| |_   / /_ _  ___| |_   _ __ ___   ___| |_| |__   ___   __| |___
 *  / _` | __| __| '__| / __|/ _ \ __| / / _` |/ _ \ __| | '_ ` _ \ / _ \ __| '_ \ / _ \ / _` / __|
 * | (_| | |_| |_| |    \__ \  __/ |_ / / (_| |  __/ |_  | | | | | |  __/ |_| | | | (_) | (_| \__ \
 *  \__,_|\__|\__|_|    |___/\___|\__/_/ \__, |\___|\__| |_| |_| |_|\___|\__|_| |_|\___/ \__,_|___/
 *                                        __/ |
 *                                       |___/
 ***************************************************************************************************/

mips_attr_t *get_mips_attr(ir_node *node)
{
	assert(is_mips_irn(node) && "need mips node to get attributes");
	return (mips_attr_t *) get_irn_generic_attr(node);
}

const mips_attr_t *get_mips_attr_const(const ir_node *node)
{
	assert(is_mips_irn(node) && "need mips node to get attributes");
	return get_irn_generic_attr_const(node);
}

const mips_immediate_attr_t *get_mips_immediate_attr_const(const ir_node *node)
{
	assert(is_mips_irn(node) && "need mips node to get attributes");
	return get_irn_generic_attr_const(node);
}

const mips_load_store_attr_t *get_mips_load_store_attr_const(
		const ir_node *node)
{
	assert(is_mips_irn(node) && "need mips node to get attributes");
	return get_irn_generic_attr_const(node);
}

/**
 * Returns the argument register requirements of a mips node.
 */
const arch_register_req_t **get_mips_in_req_all(const ir_node *node)
{
	const mips_attr_t *attr = get_mips_attr_const(node);
	return attr->in_req;
}

/**
 * Returns the argument register requirement at position pos of an mips node.
 */
const arch_register_req_t *get_mips_in_req(const ir_node *node, int pos)
{
	const mips_attr_t *attr = get_mips_attr_const(node);
	return attr->in_req[pos];
}

/**
 * Returns the result register requirement at position pos of an mips node.
 */
const arch_register_req_t *get_mips_out_req(const ir_node *node, int pos)
{
	const backend_info_t *info = be_get_info(node);
	return info->out_infos[pos].req;
}

/**
 * Sets the IN register requirements at position pos.
 */
void set_mips_req_in(ir_node *node, const arch_register_req_t *req, int pos)
{
	mips_attr_t *attr  = get_mips_attr(node);
	attr->in_req[pos] = req;
}

/**
 * Initializes the nodes attributes.
 */
static void init_mips_attributes(ir_node *node, arch_irn_flags_t flags,
                                 const arch_register_req_t **in_reqs,
                                 const be_execution_unit_t ***execution_units,
                                 int n_res)
{
	ir_graph       *irg  = get_irn_irg(node);
	struct obstack *obst = get_irg_obstack(irg);
	mips_attr_t    *attr = get_mips_attr(node);
	backend_info_t *info;
	(void) execution_units;

	arch_irn_set_flags(node, flags);
	attr->in_req  = in_reqs;

	info            = be_get_info(node);
	info->out_infos = NEW_ARR_D(reg_out_info_t, obst, n_res);
	memset(info->out_infos, 0, n_res * sizeof(info->out_infos[0]));
}

static void init_mips_immediate_attributes(ir_node *node,
                                           mips_immediate_type_t type,
                                           ir_entity *entity, long val)
{
	mips_immediate_attr_t *attr = get_irn_generic_attr(node);

	attr->imm_type = type;
	attr->entity   = entity;
	attr->val      = val;
}

static void init_mips_load_store_attributes(ir_node *node, ir_entity *entity,
                                            long offset)
{
	mips_load_store_attr_t *attr = get_irn_generic_attr(node);
	attr->stack_entity = entity;
	attr->offset       = offset;
}

static int mips_compare_nodes_attr(ir_node *node_a, ir_node *node_b)
{
	const mips_attr_t *a = get_mips_attr_const(node_a);
	const mips_attr_t *b = get_mips_attr_const(node_b);

	if(a->switch_default_pn != b->switch_default_pn)
		return 1;

	return 0;
}

static int mips_compare_immediate_attr(ir_node *node_a, ir_node *node_b)
{
	const mips_immediate_attr_t *a = get_mips_immediate_attr_const(node_a);
	const mips_immediate_attr_t *b = get_mips_immediate_attr_const(node_b);

	if(a->val != b->val)
		return 1;

	return 0;
}

static int mips_compare_load_store_attr(ir_node *node_a, ir_node *node_b)
{
	const mips_load_store_attr_t *a = get_mips_load_store_attr_const(node_a);
	const mips_load_store_attr_t *b = get_mips_load_store_attr_const(node_b);

	if(mips_compare_nodes_attr(node_a, node_b))
		return 1;
	if(a->stack_entity != b->stack_entity)
		return 1;
	if(a->offset != b->offset)
		return 1;

	return 0;
}

static void mips_copy_attr(const ir_node *old_node , ir_node *new_node)
{
	ir_graph          *irg      = get_irn_irg(new_node);
	struct obstack    *obst     = get_irg_obstack(irg);
	const mips_attr_t *attr_old = get_mips_attr_const(old_node);
	mips_attr_t       *attr_new = get_mips_attr(new_node);
	backend_info_t    *old_info = be_get_info(old_node);
	backend_info_t    *new_info = be_get_info(new_node);

	/* copy the attributes */
	memcpy(attr_new, attr_old, get_op_attr_size(get_irn_op(old_node)));

	/* copy out flags */
	new_info->out_infos =
		DUP_ARR_D(reg_out_info_t, obst, old_info->out_infos);
}

/***************************************************************************************
 *                  _                            _                   _
 *                 | |                          | |                 | |
 *  _ __   ___   __| | ___    ___ ___  _ __  ___| |_ _ __ _   _  ___| |_ ___  _ __ ___
 * | '_ \ / _ \ / _` |/ _ \  / __/ _ \| '_ \/ __| __| '__| | | |/ __| __/ _ \| '__/ __|
 * | | | | (_) | (_| |  __/ | (_| (_) | | | \__ \ |_| |  | |_| | (__| || (_) | |  \__ \
 * |_| |_|\___/ \__,_|\___|  \___\___/|_| |_|___/\__|_|   \__,_|\___|\__\___/|_|  |___/
 *
 ***************************************************************************************/

/* Include the generated constructor functions */
#include "gen_mips_new_nodes.c.inl"
