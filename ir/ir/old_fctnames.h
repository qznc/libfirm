
/* Copyright (C) 2001 by Universitaet Karlsruhe
** All rights reserved.
**
** Authors: Goetz Lindenmaier
**
** Some makros supporting old function names.
*/

/* $Id$ */

#ifndef __OLD_FCTNAMES_H__
#define __OLD_FCTNAMES_H__

/* irgraph */
#define get_irg_params     get_irg_n_locs
#define get_irg_n_loc      get_irg_n_locs
#define set_irg_params     set_irg_n_loc

/* irnode.h */
#define get_Return_n_res   get_Return_n_ress
#define get_Sel_n_index    get_Sel_n_indexs

/* irmode.h */
#define get_ident_of_mode     get_mode_ident
#define get_size_of_mode      get_mode_size
#define get_ld_align_of_mode  get_mode_ld_align
#define get_min_of_mode	      get_mode_min
#define get_max_of_mode	      get_mode_max
#define get_null_of_mode      get_mode_null
#define get_fsigned_of_mode   get_mode_fsigned
#define get_ffloat_of_mode    get_mode_ffloat

/* type.h */
#define get_type_nameid(_t_)     get_type_ident(_t_)
#define set_type_nameid(_t_,_i_) set_type_ident(_t_,_i_)
#define get_class_n_member    get_class_n_members
#define get_class_n_subtype   get_class_n_subtypes
#define get_class_n_supertype get_class_n_supertypes
#define get_struct_n_member   get_struct_n_members

#define get_method_n_res(X) get_method_n_ress(X)

#endif
