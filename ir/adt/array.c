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
 * @brief       Array --- dynamic & flexible arrays.
 * @author      Markus Armbruster
 * @version     $Id$
 */

#include "config.h"

#include <stdlib.h>

#include "array_t.h"
#include "xmalloc.h"

/* Undefine the macros to get the functions instead, cf tmalloc.c.  */
#undef xmalloc
#undef xrealloc
#undef xstrdup
#undef xfree

#ifndef MAX
# define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MIN
# define MIN(a,b) ((a) > (b) ? (b) : (a))
#endif

/**
 * An empty dynamic array descriptor.
 */
ir_arr_descr arr_mt_descr = { ARR_D_MAGIC, 0, {0}, 0, {{{0}}} };

void ir_verify_arr(const void *arr)
{
#ifndef NDEBUG
	ir_arr_descr *descr = ARR_DESCR(arr);
	assert(descr->magic == ARR_D_MAGIC || descr->magic == ARR_A_MAGIC
			 || descr->magic == ARR_F_MAGIC);
	if (descr->magic == ARR_F_MAGIC) {
		assert(descr->u.allocated >= descr->nelts);
	}
	assert(descr->nelts >= 0);
#else
	(void) arr;
#endif
}

/**
 * Creates a dynamic array on a obstack.
 *
 * @param obstack    An struct obstack * were the data will be allocated
 * @param nelts      The number of elements
 * @param elts_size  The size of the array elements.
 *
 * @return A pointer to the dynamic array (can be used as a pointer to the
 *         first element of this array).
 *
 * @remark Helper function, use NEW_ARR_D() instead.
 */
void *ir_new_arr_d(struct obstack *obstack, int nelts, size_t elts_size)
{
	ir_arr_descr *dp;

	assert(obstack && (nelts >= 0));

	dp = (ir_arr_descr*) obstack_alloc(obstack, ARR_ELTS_OFFS + elts_size);
	ARR_SET_DBGINF(dp, ARR_D_MAGIC, elts_size/nelts);
	dp->u.obstack = obstack;
	dp->nelts = nelts;
	return dp->v.elts;
}

/**
 * Creates a flexible array.
 *
 * @param nelts      The number of elements
 * @param elts_size  The size of the array elements.
 *
 * @return A pointer to the flexible array (can be used as a pointer to the
 *         first element of this array).
 *
 * @remark Helper function, use NEW_ARR_F() instead.
 */
void *ir_new_arr_f(int nelts, size_t elts_size)
{
	ir_arr_descr *newa;

	assert (nelts >= 0);
	newa = (ir_arr_descr*) xmalloc (ARR_ELTS_OFFS+elts_size);
	ARR_SET_DBGINF (newa, ARR_F_MAGIC, nelts ? elts_size/nelts : 0);
	newa->u.allocated = newa->nelts = nelts;
	return newa->v.elts;
}

/**
 * Delete a flexible array.
 *
 * @param elts    The flexible array (pointer to the first element).
 *
 * @remark Helper function, use DEL_ARR_F() instead.
 */
void ir_del_arr_f(void *elts)
{
	ir_arr_descr *dp = ARR_DESCR (elts);

	ARR_VRFY (elts);
	assert (dp->magic == ARR_F_MAGIC);

#ifndef NDEBUG
	{
		ir_arr_descr *wdp = (ir_arr_descr *)dp;
		wdp->magic = 0xdeadbeef;
	}
#endif
	free(dp);
}

/**
 * Resize a flexible array, always reallocate data.
 *
 * @param elts       The flexible array (pointer to the first element).
 * @param nelts      The new number of elements.
 * @param elts_size  The size of the array elements.
 *
 * @return A resized flexible array, possibly other address than
 *         elts.
 *
 * @remark Helper function, use ARR_SETLEN() instead.
 */
void *ir_arr_setlen (void *elts, int nelts, size_t elts_size)
{
	ir_arr_descr *dp = ARR_DESCR (elts);

	assert ((dp->magic == ARR_F_MAGIC) && (nelts >= 0));
	ARR_VRFY (elts);
	assert (!dp->eltsize || !nelts || (dp->eltsize == elts_size/nelts));

	dp = (ir_arr_descr*) xrealloc (dp, ARR_ELTS_OFFS+elts_size);
	dp->u.allocated = dp->nelts = nelts;

	return dp->v.elts;
}

/**
 * Resize a flexible array, allocate more data if needed but do NOT
 * reduce.
 *
 * @param elts     The flexible array (pointer to the first element).
 * @param nelts    The new number of elements.
 * @param eltsize  The size of the array elements.
 *
 * @return A resized flexible array, possibly other address than
 *         elts.
 *
 * @remark Helper function, use ARR_RESIZE() instead.
 */
void *ir_arr_resize(void *elts, int nelts, size_t eltsize)
{
	ir_arr_descr *dp = ARR_DESCR(elts);
	int n;

	assert((dp->magic == ARR_F_MAGIC) && (nelts >= 0));
	ARR_VRFY(elts);
	assert(dp->eltsize ? dp->eltsize == eltsize : (dp->eltsize = eltsize, 1));

	/* @@@ lots of resizes for small nelts */
	n = MAX(1, dp->u.allocated);
	while (nelts > n) n <<= 1;
	while (3*nelts < n) n >>= 1;
	assert(n >= nelts);

	if (n != dp->u.allocated) {
		dp = (ir_arr_descr*) xrealloc(dp, ARR_ELTS_OFFS+eltsize*n);
		dp->u.allocated = n;
#if defined(DEBUG) && defined(HAVE_GNU_MALLOC)
	} else {
		tmalloc_tag = NULL;
#endif
	}
	dp->nelts = nelts;

	return dp->v.elts;
}

#ifdef DEBUG_libfirm
/**
 * This function returns the length of a flexible array.
 * Do NOT use is in code, use ARR_LEN() macro!
 * This function is intended to be called from a debugger.
 */
int array_len(const void *arr);
int array_len(const void *arr)
{
	return ARR_LEN(arr);
}

/**
 * This function returns the array descriptor of a flexible array.
 * Do NOT use is in code!.
 * This function is intended to be called from a debugger.
 */
ir_arr_descr *array_descr(const void *arr);
ir_arr_descr *array_descr(const void *arr)
{
	if (! arr)
		return NULL;
	return ARR_DESCR(arr);
}
#endif /* DEBUG_libfirm */
