/*
 * Project:     libFIRM
 * File name:   ir/tr/type_t.h
 * Purpose:     Representation of types -- private header.
 * Author:      Goetz Lindenmaier
 * Modified by:
 * Created:
 * CVS-ID:      $Id$
 * Copyright:   (c) 2001-2003 Universitšt Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */

# ifndef _TYPE_T_H_
# define _TYPE_T_H_

# include "firm_config.h"
# include "type.h"
# include "tpop_t.h"

# include "array.h"

/**
 * @file type_t.h
 * This file contains the datatypes hidden in type.h.
 *
 * @author Goetz Lindenmaier
 * @see  type.h tpop_t.h tpop.h
 */

/** class attributes */
typedef struct {
  entity **members;    /**< fields and methods of this class */
  type   **subtypes;   /**< direct subtypes */
  type   **supertypes; /**< direct supertypes */
  peculiarity peculiarity;
  int dfn;             /**< number used for 'instanceof' operator */
} cls_attr;

/** struct attributes */
typedef struct {
  entity **members;    /**< fields of this struct. No method entities allowed. */
} stc_attr;

/** method attributes */
typedef struct {
  int n_params;              /**< number of parameters */
  type **param_type;         /**< code generation needs this information. */
  type *value_params;        /**< A type whose entities represent copied value arguments. */
  int n_res;                 /**< number of results */
  type **res_type;           /**< array with result types */
  type *value_ress;          /**< A type whose entities represent copied value results. */
  variadicity variadicity;   /**< variadicity of the method. */
  int first_variadic_param;  /**< index of the first variadic param or -1 if non-variadic .*/
} mtd_attr;

/** union attributes */
typedef struct {
  entity **members;    /**< fields of this union. No method entities allowed. */
} uni_attr;

/** array attributes */
typedef struct {
  int   n_dimensions;  /**< Number of array dimensions.  */
  ir_node **lower_bound;   /**< Lower bounds of dimensions.  Usually all 0. */
  ir_node **upper_bound;   /**< Upper bounds or dimensions. */
  int *order;              /**< Ordering of dimensions. */
  type *element_type;  /**< The type of the array elements. */
  entity *element_ent; /**< Entity for the array elements, to be used for
              element selection with Sel. */
} arr_attr;

/** enum attributes */
typedef struct {
  int      n_enums;    /**< Number of enumerators. */
  tarval **enumer;     /**< Contains all constants that represent a member
                          of the enum -- enumerators. */
  ident  **enum_nameid;/**< Contains the names of the enum fields as specified by
                          the source program */
} enm_attr;

/** pointer attributes */
typedef struct {
  type *points_to;     /**< The type of the entity the pointer points to. */
} ptr_attr;

/*
typedef struct {   * No private attr yet! *
} pri_attr; */


/*
typedef struct {        * No private attr, must be smaller than others! *
} id_attr;
*/

/** General type attributes. */
typedef union {
  cls_attr ca;      /**< attributes of a class type */
  stc_attr sa;      /**< attributes of a struct type */
  mtd_attr ma;      /**< attributes of a method type */
  uni_attr ua;      /**< attributes of an union type */
  arr_attr aa;      /**< attributes of an array type */
  enm_attr ea;      /**< attributes of an enumeration type */
  ptr_attr pa;      /**< attributes of a pointer type */
} tp_attr;

/** the structure of a type */
struct type {
  firm_kind kind;          /**< the firm kind, must be k_type */
  const tp_op *type_op;    /**< the type operation of the type */
  ident *name;             /**< The name of the type */
  visibility visibility;   /**< Visibility of entities of this type. */
  char frame_type;         /**< True if this is a frame type, false else */
  type_state state;        /**< Represents the types state: layout undefined or
                                fixed. */
  int size;                /**< Size of an entity of this type. This is determined
                                when fixing the layout of this class.  Size must be
                                given in bits. */
  int align;               /**< Alignment of an entity of this type. This should be
                                set according to the source language needs. If not set it's
                                calculated automatically by get_type_alignment().
                                Alignment must be given in bits. */
  ir_mode *mode;           /**< The mode for atomic types */
  unsigned long visit;     /**< visited counter for walks of the type information */
  void *link;              /**< holds temporary data - like in irnode_t.h */
  struct dbg_info *dbi;    /**< A pointer to information for debug support. */

  /* ------------- fields for analyses ---------------*/

#ifdef DEBUG_libfirm
  int nr;             /**< a unique node number for each node to make output
                           readable. */
#endif
  tp_attr attr;            /* type kind specific fields. This must be the last
                  entry in this struct!  Varying size! */
};

/**
 *   Creates a new type representation:
 *
 *   @param type_op  the kind of this type.  May not be type_id.
 *   @param mode     the mode to be used for this type, may be NULL
 *   @param name     an ident for the name of this type.
 *   @param db       debug info
 *
 *   @return A new type of the given type.  The remaining private attributes are not
 *           initialized.  The type is in state layout_undefined.
 */
INLINE type *
new_type(tp_op *type_op, ir_mode *mode, ident *name, dbg_info *db);
void free_type_attrs       (type *tp);

INLINE void free_class_entities      (type *clss);
INLINE void free_struct_entities     (type *strct);
INLINE void free_method_entities     (type *method);
INLINE void free_union_entities      (type *uni);
INLINE void free_array_entities      (type *array);
INLINE void free_enumeration_entities(type *enumeration);
INLINE void free_pointer_entities    (type *pointer);
INLINE void free_primitive_entities  (type *primitive);

INLINE void free_class_attrs      (type *clss);
INLINE void free_struct_attrs     (type *strct);
INLINE void free_method_attrs     (type *method);
INLINE void free_union_attrs      (type *uni);
INLINE void free_array_attrs      (type *array);
INLINE void free_enumeration_attrs(type *enumeration);
INLINE void free_pointer_attrs    (type *pointer);
INLINE void free_primitive_attrs  (type *primitive);


/**
 * Initialize the type module.
 *
 * @param buildin_db  debug info for builtin objects
 */
void firm_init_type(dbg_info *builtin_db);


/* ------------------- *
 *  inline functions   *
 * ------------------- */

extern unsigned long firm_type_visited;

static INLINE void _set_master_type_visited(unsigned long val) { firm_type_visited = val; }
static INLINE unsigned long _get_master_type_visited(void)     { return firm_type_visited; }
static INLINE void _inc_master_type_visited(void)              { ++firm_type_visited; }

static INLINE void *
_get_type_link(const type *tp) {
  assert(tp && tp->kind == k_type);
  return(tp -> link);
}

static INLINE void
_set_type_link(type *tp, void *l) {
  assert(tp && tp->kind == k_type);
  tp -> link = l;
}

static INLINE const tp_op*
_get_type_tpop(const type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->type_op;
}

static INLINE ident*
_get_type_tpop_nameid(const type *tp) {
  assert(tp && tp->kind == k_type);
  return get_tpop_ident(tp->type_op);
}

static INLINE tp_opcode
_get_type_tpop_code(const type *tp) {
  assert(tp && tp->kind == k_type);
  return get_tpop_code(tp->type_op);
}

static INLINE ir_mode *
_get_type_mode(const type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->mode;
}

static INLINE ident *
_get_type_ident(const type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->name;
}

static INLINE void
_set_type_ident(type *tp, ident* id) {
  assert(tp && tp->kind == k_type);
  tp->name = id;
}

static INLINE int
_get_type_size_bits(const type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->size;
}

static INLINE int
_get_type_size_bytes(const type *tp) {
  int size = _get_type_size_bits(tp);
  if (size < 0)
    return -1;
  if ((size & 7) != 0) {
    assert(0 && "cannot take byte size of this type");
    return -1;
  }
  return size >> 3;
}

static INLINE type_state
_get_type_state(const type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->state;
}

static INLINE unsigned long
_get_type_visited(const type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->visit;
}

static INLINE void
_set_type_visited(type *tp, unsigned long num) {
  assert(tp && tp->kind == k_type);
  tp->visit = num;
}

static INLINE void
_mark_type_visited(type *tp) {
  assert(tp && tp->kind == k_type);
  assert(tp->visit < firm_type_visited);
  tp->visit = firm_type_visited;
}

static INLINE int
_type_visited(const type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->visit >= firm_type_visited;
}

static INLINE int
_type_not_visited(const type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->visit  < firm_type_visited;
}

static INLINE int
_is_type(const void *thing) {
  return (get_kind(thing) == k_type);
}

static INLINE int
_is_class_type(const type *clss) {
  assert(clss);
  return (clss->type_op == type_class);
}

static INLINE int
_get_class_n_members (const type *clss) {
  assert(clss && (clss->type_op == type_class));
  return (ARR_LEN (clss->attr.ca.members));
}

static INLINE entity *
_get_class_member   (const type *clss, int pos) {
  assert(clss && (clss->type_op == type_class));
  assert(pos >= 0 && pos < _get_class_n_members(clss));
  return clss->attr.ca.members[pos];
}

static INLINE int
_is_struct_type(const type *strct) {
  assert(strct);
  return (strct->type_op == type_struct);
}

static INLINE int
_is_method_type(const type *method) {
  assert(method);
  return (method->type_op == type_method);
}

static INLINE int
_is_union_type(const type *uni) {
  assert(uni);
  return (uni->type_op == type_union);
}

static INLINE int
_is_array_type(const type *array) {
  assert(array);
  return (array->type_op == type_array);
}

static INLINE int
_is_enumeration_type(const type *enumeration) {
  assert(enumeration);
  return (enumeration->type_op == type_enumeration);
}

static INLINE int
_is_pointer_type(const type *pointer) {
  assert(pointer);
  return (pointer->type_op == type_pointer);
}

/** Returns true if a type is a primitive type. */
static INLINE int
_is_primitive_type(const type *primitive) {
  assert(primitive && primitive->kind == k_type);
  return (primitive->type_op == type_primitive);
}

static INLINE int
_is_atomic_type(const type *tp) {
  assert(tp && tp->kind == k_type);
  return (_is_primitive_type(tp) || _is_pointer_type(tp) ||
      _is_enumeration_type(tp));
}


#define set_master_type_visited(val)      _set_master_type_visited(val)
#define get_master_type_visited()         _get_master_type_visited()
#define inc_master_type_visited()         _inc_master_type_visited()
#define get_type_link(tp)                 _get_type_link(tp)
#define set_type_link(tp, l)              _set_type_link(tp, l)
#define get_type_tpop(tp)                 _get_type_tpop(tp)
#define get_type_tpop_nameid(tp)          _get_type_tpop_nameid(tp)
#define get_type_tpop_code(tp)            _get_type_tpop_code(tp)
#define get_type_mode(tp)                 _get_type_mode(tp)
#define get_type_ident(tp)                _get_type_ident(tp)
#define set_type_ident(tp, id)            _set_type_ident(tp, id)
#define get_type_size(tp)                 _get_type_size(tp)
#define get_type_state(tp)                _get_type_state(tp)
#define get_type_visited(tp)              _get_type_visited(tp)
#define set_type_visited(tp, num)         _set_type_visited(tp, num)
#define mark_type_visited(tp)             _mark_type_visited(tp)
#define type_visited(tp)                  _type_visited(tp)
#define type_not_visited(tp)              _type_not_visited(tp)
#define is_type(thing)                    _is_type(thing)
#define is_Class_type(clss)               _is_class_type(clss)
#define get_class_n_members(clss)         _get_class_n_members(clss)
#define get_class_member(clss, pos)       _get_class_member(clss, pos)
#define is_Struct_type(strct)             _is_struct_type(strct)
#define is_Method_type(method)            _is_method_type(method)
#define is_Union_type(uni)                _is_union_type(uni)
#define is_Array_type(array)              _is_array_type(array)
#define is_Enumeration_type(enumeration)  _is_enumeration_type(enumeration)
#define is_Pointer_type(pointer)          _is_pointer_type(pointer)
#define is_Primitive_type(primitive)      _is_primitive_type(primitive)
#define is_atomic_type(tp)                _is_atomic_type(tp)

# endif /* _TYPE_T_H_ */
