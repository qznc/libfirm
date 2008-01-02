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
 * @brief   declaration of attributes for mips nodes
 * @author  Matthias Braun
 * @version $Id$
 */
#ifndef FIRM_BE_MIPS_MIPS_NODES_ATTR_H
#define FIRM_BE_MIPS_MIPS_NODES_ATTR_H

#include "../bearch_t.h"
#include "irmode_t.h"
#include "irnode_t.h"

typedef struct mips_attr_t {
	except_attr  exc;               /**< the exception attribute. MUST be the first one. */
	arch_irn_flags_t flags;     /**< indicating if spillable, rematerializeable ... etc. */

	int switch_default_pn;		/**< proj number of default case in switch */

	const arch_register_req_t **in_req;  /**< register requirements for arguments */
	const arch_register_req_t **out_req; /**< register requirements for results */

	const arch_register_t **slots;     /**< register slots for assigned registers */
} mips_attr_t;

typedef enum mips_immediate_type_t {
	MIPS_IMM_CONST,
	MIPS_IMM_SYMCONST_LO,
	MIPS_IMM_SYMCONST_HI
} mips_immediate_type_t;

typedef struct mips_immediate_attr_t {
	mips_attr_t            attr;
	mips_immediate_type_t  imm_type;
	ir_entity             *entity;
	long                   val;
} mips_immediate_attr_t;

typedef struct mips_load_store_attr_t {
	mips_attr_t  attr;
	ir_entity   *stack_entity;
	long         offset;
} mips_load_store_attr_t;

#endif
