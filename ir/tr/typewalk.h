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
 * @file    typewalk.h
 * @brief   Functionality to modify the type graph.
 * @author  Goetz Lindenmaier
 * @version $Id$
 * @summary
 *
 * Traverse the type information.
 *
 * The walker walks the whole ir graph
 * to find the distinct type trees in the type graph forest.
 * - execute the pre() function before recursion
 * - execute the post() function after recursion
 */
#ifndef FIRM_TR_TYPEWALK_H
#define FIRM_TR_TYPEWALK_H

#include "type.h"
#include "type_or_entity.h"

#include "irgraph.h"

/** Type of argument functions for type walkers.
 *
 * @param tore    points to the visited type or entity
 * @param env     free environment pointer
 */
typedef void type_walk_func(type_or_ent *tore, void *env);

/**  The class walk function
 *
 * @param clss    points to the visited class
 * @param env     free environment pointer
 */
typedef void class_walk_func(ir_type *clss, void *env);

/** Touches every type and entity in unspecified order.  If new
 *  types/entities are created during the traversal these will
 *  be visited, too.
 *  Does not touch frame types or types for value params ... */
void type_walk(type_walk_func *pre, type_walk_func *post, void *env);

/** Walks over all type information reachable from an ir graph.
 *
 *  Walks over all type information reachable from irg, i.e., starts a
 *  type walk at the irgs entity, the irgs frame type and all types and
 *  entities that are attributes to firm nodes. */
void type_walk_irg(ir_graph *irg,
           type_walk_func *pre,
           type_walk_func *post,
           void *env);

/**
    Touches every class in specified order:
    - first the super class
    - second the class itself
    - third the sub classes.  If new classes are created
    during the traversal these will be visited, too.

    @todo should be named class-walk

    @deprecated will be removed?
*/
void type_walk_super2sub(type_walk_func *pre,
             type_walk_func *post,
             void *env);

/** Walker for class types in inheritance order.
 *
 *  Touches every class in specified order:
 *   - first the super class
 *   - second the class itself
 *   If new classes are created during the traversal these
 *   will be visited, too.
 * Starts the walk at arbitrary classes.
 * Executes pre when first visiting a class.  Executes post after
 * visiting all superclasses.
 *
 * The arguments pre, post, env may be NULL. */
void type_walk_super(type_walk_func *pre,
             type_walk_func *post,
             void *env);

/** Same as type_walk_super2sub, but visits only class types.
   Executes pre for a class if all superclasses have been visited.
   Then iterates to subclasses.  Executes post after return from
   subclass.
   Does not visit global type, frame types.

   @bug ?? something is wrong with this.
*/
void class_walk_super2sub(class_walk_func *pre,
                          class_walk_func *post,
                          void *env);

/**
 * the entity walk function.  A function type for entity walkers.
 *
 * @param ent     points to the visited entity
 * @param env     free environment pointer
 */
typedef void entity_walk_func(ir_entity *ent, void *env);

/**
 * Walks over all entities in the type.
 *
 * @param tp    the type
 * @param doit  the entity walker function
 * @param env   environment, will be passed to the walker function
 */
void walk_types_entities(ir_type *tp,
             entity_walk_func *doit,
             void *env);

#endif /* FIRM_TR_TYPEWALK_H */
