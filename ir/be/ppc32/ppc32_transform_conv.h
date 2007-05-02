/*
 * Copyright (C) 1995-2007 University of Karlsruhe.  All right reserved.
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
 * @brief   declarations for conv transform functions
 * @author  Moritz Kroll, Jens Mueller
 * @version $Id$
 */
#ifndef FIRM_BE_PPC32_PPC32_TRANSFORM_CONV_H
#define FIRM_BE_PPC32_PPC32_TRANSFORM_CONV_H

void ppc32_init_conv_walk(void);

void ppc32_conv_walk(ir_node *node, void *env);
void ppc32_pretransform_walk(ir_node *node, void *env);

#endif
