/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief    Representation of and static computations on target machine
 *           values.
 * @date     2003
 * @author   Mathias Heil
 * @brief
 *
 * Values are stored in a format depending upon chosen arithmetic
 * module. Default uses strcalc and fltcalc.
 * This implementation assumes:
 *  - target has IEEE-754 floating-point arithmetic.
 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>

#include "bitfiddle.h"
#include "tv_t.h"
#include "set.h"
#include "entity_t.h"
#include "irmode_t.h"
#include "irnode.h"
#include "strcalc.h"
#include "fltcalc.h"
#include "util.h"
#include "xmalloc.h"
#include "firm_common.h"
#include "error.h"

/** Size of hash tables.  Should correspond to average number of distinct constant
    target values */
#define N_CONSTANTS 2048

/****************************************************************************
 *   local definitions and macros
 ****************************************************************************/
#ifndef NDEBUG
#  define TARVAL_VERIFY(a) tarval_verify((a))
#else
#  define TARVAL_VERIFY(a) ((void)0)
#endif

#define INSERT_TARVAL(tv) (set_insert(ir_tarval, tarvals, (tv), sizeof(ir_tarval), hash_tv((tv))))
#define FIND_TARVAL(tv) (set_find(ir_tarval, tarvals, (tv), sizeof(ir_tarval), hash_tv((tv))))

#define INSERT_VALUE(val, size) (set_insert(char, values, (val), size, hash_val((val), size)))
#define FIND_VALUE(val, size) (set_find(char, values, (val), size, hash_val((val), size)))

#define fail_verify(a) _fail_verify((a), __FILE__, __LINE__)

/** A set containing all existing tarvals. */
static struct set *tarvals = NULL;
/** A set containing all existing values. */
static struct set *values = NULL;

/** The integer overflow mode. */
static tarval_int_overflow_mode_t int_overflow_mode = TV_OVERFLOW_WRAP;

/****************************************************************************
 *   private functions
 ****************************************************************************/
#ifndef NDEBUG
static unsigned hash_val(const void *value, size_t length);
static unsigned hash_tv(ir_tarval *tv);
static void _fail_verify(ir_tarval *tv, const char* file, int line)
{
	/* print a memory image of the tarval and throw an assertion */
	if (tv)
		panic("%s:%d: Invalid tarval: mode: %F\n value: [%p]", file, line, tv->mode, tv->value);
	else
		panic("%s:%d: Invalid tarval (null)", file, line);
}

inline static
#ifdef __GNUC__
	__attribute__((unused))
#endif
void tarval_verify(ir_tarval *tv)
{
	assert(tv);
	assert(tv->mode);
	assert(tv->value);

	if ((tv == tarval_bad) || (tv == tarval_undefined)) return;
	if ((tv == tarval_b_true) || (tv == tarval_b_false)) return;

	if (!FIND_TARVAL(tv)) fail_verify(tv);
	if (tv->length > 0 && !FIND_VALUE(tv->value, tv->length)) fail_verify(tv);
}
#endif /* NDEBUG */

/** Hash a tarval. */
static unsigned hash_tv(ir_tarval *tv)
{
	return (unsigned)((PTR_TO_INT(tv->value) ^ PTR_TO_INT(tv->mode)) + tv->length);
}

/** Hash a value. Treat it as a byte array. */
static unsigned hash_val(const void *value, size_t length)
{
	size_t i;
	unsigned hash = 0;

	/* scramble the byte - array */
	for (i = 0; i < length; ++i) {
		hash += (hash << 5) ^ (hash >> 27) ^ ((char*)value)[i];
		hash += (hash << 11) ^ (hash >> 17);
	}

	return hash;
}

static int cmp_tv(const void *p1, const void *p2, size_t n)
{
	const ir_tarval *tv1 = (const ir_tarval*) p1;
	const ir_tarval *tv2 = (const ir_tarval*) p2;
	(void) n;

	assert(tv1->kind == k_tarval);
	assert(tv2->kind == k_tarval);
	if (tv1->mode < tv2->mode)
		return -1;
	if (tv1->mode > tv2->mode)
		return 1;
	if (tv1->length < tv2->length)
		return -1;
	if (tv1->length > tv2->length)
		return 1;
	if (tv1->value < tv2->value)
		return -1;
	if (tv1->value > tv2->value)
		return 1;

	return 0;
}

/** finds tarval with value/mode or creates new tarval */
static ir_tarval *get_tarval(const void *value, size_t length, ir_mode *mode)
{
	ir_tarval tv;

	tv.kind   = k_tarval;
	tv.mode   = mode;
	tv.length = length;
	if (length > 0) {
		/* if there already is such a value, it is returned, else value
		 * is copied into the set */
		char *temp = (char*) alloca(length);
		memcpy(temp, value, length);
		if (get_mode_arithmetic(mode) == irma_twos_complement) {
			sign_extend(temp, mode);
		}
		tv.value = INSERT_VALUE(temp, length);
	} else {
		tv.value = value;
	}
	/* if there is such a tarval, it is returned, else tv is copied
	 * into the set */
	return INSERT_TARVAL(&tv);
}

/**
 * handle overflow
 */
static ir_tarval *get_tarval_overflow(const void *value, size_t length, ir_mode *mode)
{
	char *temp;

	switch (get_mode_sort(mode)) {
	case irms_reference:
		/* addresses always wrap around */
		temp = (char*) alloca(sc_get_buffer_length());
		memcpy(temp, value, sc_get_buffer_length());
		sc_truncate(get_mode_size_bits(mode), temp);
		/* the sc_ module expects that all bits are set ... */
		sign_extend(temp, mode);
		return get_tarval(temp, length, mode);

	case irms_int_number:
		if (sc_comp(value, get_mode_max(mode)->value) == ir_relation_greater) {
			switch (tarval_get_integer_overflow_mode()) {
			case TV_OVERFLOW_SATURATE:
				return get_mode_max(mode);
			case TV_OVERFLOW_WRAP:
				temp = (char*) alloca(sc_get_buffer_length());
				memcpy(temp, value, sc_get_buffer_length());
				sc_truncate(get_mode_size_bits(mode), temp);
				/* the sc_ module expects that all bits are set ... */
				sign_extend(temp, mode);
				return get_tarval(temp, length, mode);
			case TV_OVERFLOW_BAD:
				return tarval_bad;
			default:
				return get_tarval(value, length, mode);
			}
		}
		if (sc_comp(value, get_mode_min(mode)->value) == ir_relation_less) {
			switch (tarval_get_integer_overflow_mode()) {
			case TV_OVERFLOW_SATURATE:
				return get_mode_min(mode);
			case TV_OVERFLOW_WRAP: {
				temp = (char*) alloca(sc_get_buffer_length());
				memcpy(temp, value, sc_get_buffer_length());
				sc_truncate(get_mode_size_bits(mode), temp);
				return get_tarval(temp, length, mode);
			}
			case TV_OVERFLOW_BAD:
				return tarval_bad;
			default:
				return get_tarval(value, length, mode);
			}
		}
		break;

	case irms_float_number:
		break;

	default:
		break;
	}
	return get_tarval(value, length, mode);
}

static ir_tarval reserved_tv[2];
static ir_tarval nonconst_tvs[4];

ir_tarval *tarval_b_false     = &reserved_tv[0];
ir_tarval *tarval_b_true      = &reserved_tv[1];
ir_tarval *tarval_bad         = &nonconst_tvs[0];
ir_tarval *tarval_undefined   = &nonconst_tvs[1];
ir_tarval *tarval_reachable   = &nonconst_tvs[2];
ir_tarval *tarval_unreachable = &nonconst_tvs[3];

/**
 * get the float descriptor for given mode.
 */
static const float_descriptor_t *get_descriptor(const ir_mode *mode)
{
	return &mode->float_desc;
}

ir_tarval *new_integer_tarval_from_str(const char *str, size_t len, char sign,
                                       unsigned char base, ir_mode *mode)
{
	void *buffer;
	int   ok;

	buffer = alloca(sc_get_buffer_length());

	ok = sc_val_from_str(sign, base, str, len, buffer);
	if (!ok)
		return tarval_bad;

	return get_tarval_overflow(buffer, sc_get_buffer_length(), mode);
}

static ir_tarval *new_tarval_from_str_int(const char *str, size_t len,
                                          ir_mode *mode)
{
	void    *buffer;
	unsigned base = 10;
	char     sign = 1;
	int      ok;

	/* skip leading spaces */
	while (len > 0 && str[0] == ' ') {
		++str;
		--len;
	}
	if (len == 0)
		return tarval_bad;

	/* 1 sign character allowed */
	if (str[0] == '-') {
		sign = -1;
		++str;
		--len;
	} else if (str[0] == '+') {
		++str;
		--len;
	}

	/* a number starting with '0x' is hexadeciaml,
	 * a number starting with '0' (and at least 1 more char) is octal */
	if (len >= 2 && str[0] == '0') {
		if (str[1] == 'x' || str[1] == 'X') {
			str += 2;
			len -= 2;
			base = 16;
		} else if (str[1] == 'b' || str[1] == 'B') {
			str += 2;
			len -= 2;
			base = 2;
		} else {
			++str;
			--len;
			base = 8;
		}
	}
	if (len == 0)
		return tarval_bad;

	buffer = alloca(sc_get_buffer_length());

	ok = sc_val_from_str(sign, base, str, len, buffer);
	if (!ok)
		return tarval_bad;

	return get_tarval_overflow(buffer, sc_get_buffer_length(), mode);
}

ir_tarval *new_tarval_from_str(const char *str, size_t len, ir_mode *mode)
{
	const float_descriptor_t *desc;

	assert(str);
	assert(len);
	assert(mode);

	switch (get_mode_sort(mode)) {
	case irms_internal_boolean:
		/* match [tT][rR][uU][eE]|[fF][aA][lL][sS][eE] */
		if (!strcasecmp(str, "true"))
			return tarval_b_true;
		else if (!strcasecmp(str, "false"))
			return tarval_b_false;
		else
			/* XXX This is C semantics */
			return atoi(str) ? tarval_b_true : tarval_b_false;

	case irms_float_number:
		desc = get_descriptor(mode);
		fc_val_from_str(str, len, desc, NULL);
		return get_tarval(fc_get_buffer(), fc_get_buffer_length(), mode);

	case irms_reference:
		if (!strcasecmp(str, "null"))
			return get_tarval_null(mode);
		/* FALLTHROUGH */
	case irms_int_number:
		return new_tarval_from_str_int(str, len, mode);
	default:
		panic("Unsupported tarval creation with mode %F", mode);
	}
}

ir_tarval *new_tarval_from_long(long l, ir_mode *mode)
{
	assert(mode);

	switch (get_mode_sort(mode))   {
	case irms_internal_boolean:
		/* XXX C semantics ! */
		return l ? tarval_b_true : tarval_b_false ;

	case irms_reference:
		/* same as integer modes */
	case irms_int_number:
		sc_val_from_long(l, NULL);
		return get_tarval(sc_get_buffer(), sc_get_buffer_length(), mode);

	case irms_float_number:
		return new_tarval_from_double((long double)l, mode);

	default:
		panic("unsupported mode sort");
	}
}

int tarval_is_long(ir_tarval *tv)
{
	if (!mode_is_int(tv->mode) && !mode_is_reference(tv->mode))
		return 0;

	if (get_mode_size_bits(tv->mode) > (int) (sizeof(long) << 3)) {
		/* the value might be too big to fit in a long */
		sc_max_from_bits(sizeof(long) << 3, 0, NULL);
		if (sc_comp(sc_get_buffer(), tv->value) == ir_relation_less) {
			/* really doesn't fit */
			return 0;
		}
	}
	return 1;
}

long get_tarval_long(ir_tarval* tv)
{
	assert(tarval_is_long(tv) && "tarval too big to fit in long");

	return sc_val_to_long(tv->value);
}

ir_tarval *new_tarval_from_long_double(long double d, ir_mode *mode)
{
	const float_descriptor_t *desc;

	assert(mode && (get_mode_sort(mode) == irms_float_number));
	desc = get_descriptor(mode);
	fc_val_from_ieee754(d, desc, NULL);
	return get_tarval(fc_get_buffer(), fc_get_buffer_length(), mode);
}

ir_tarval *new_tarval_from_double(double d, ir_mode *mode)
{
	return new_tarval_from_long_double(d, mode);
}

int tarval_is_double(ir_tarval *tv)
{
	assert(tv);

	return (get_mode_sort(tv->mode) == irms_float_number);
}

long double get_tarval_long_double(ir_tarval *tv)
{
	assert(tarval_is_double(tv));

	return fc_val_to_ieee754((const fp_value*) tv->value);
}

double get_tarval_double(ir_tarval *tv)
{
	return get_tarval_long_double(tv);
}

ir_mode *(get_tarval_mode)(const ir_tarval *tv)
{
	return _get_tarval_mode(tv);
}

/*
 * Special value query functions ============================================
 *
 * These functions calculate and return a tarval representing the requested
 * value.
 * The functions get_mode_{Max,Min,...} return tarvals retrieved from these
 * functions, but these are stored on initialization of the irmode module and
 * therefore the irmode functions should be preferred to the functions below.
 */

ir_tarval *(get_tarval_bad)(void)
{
	return _get_tarval_bad();
}

ir_tarval *(get_tarval_undefined)(void)
{
	return _get_tarval_undefined();
}

ir_tarval *(get_tarval_b_false)(void)
{
	return _get_tarval_b_false();
}

ir_tarval *(get_tarval_b_true)(void)
{
	return _get_tarval_b_true();
}

ir_tarval *(get_tarval_reachable)(void)
{
	return _get_tarval_reachable();
}

ir_tarval *(get_tarval_unreachable)(void)
{
	return _get_tarval_unreachable();
}

ir_tarval *get_tarval_max(ir_mode *mode)
{
	const float_descriptor_t *desc;

	switch (get_mode_sort(mode)) {
	case irms_internal_boolean:
		return tarval_b_true;

	case irms_float_number:
		desc = get_descriptor(mode);
		fc_get_max(desc, NULL);
		return get_tarval(fc_get_buffer(), fc_get_buffer_length(), mode);

	case irms_reference:
	case irms_int_number:
		sc_max_from_bits(get_mode_size_bits(mode), mode_is_signed(mode), NULL);
		return get_tarval(sc_get_buffer(), sc_get_buffer_length(), mode);
	default:
		panic("mode %F does not support maximum value", mode);
	}
}

ir_tarval *get_tarval_min(ir_mode *mode)
{
	const float_descriptor_t *desc;

	switch (get_mode_sort(mode)) {
	case irms_internal_boolean:
		return tarval_b_false;

	case irms_float_number:
		desc = get_descriptor(mode);
		fc_get_min(desc, NULL);
		return get_tarval(fc_get_buffer(), fc_get_buffer_length(), mode);

	case irms_reference:
	case irms_int_number:
		sc_min_from_bits(get_mode_size_bits(mode), mode_is_signed(mode), NULL);
		return get_tarval(sc_get_buffer(), sc_get_buffer_length(), mode);
	default:
		panic("mode %F does not support minimum value", mode);
	}
}

/** The bit pattern for the pointer NULL */
static long _null_value = 0;

ir_tarval *get_tarval_null(ir_mode *mode)
{
	switch (get_mode_sort(mode)) {
	case irms_float_number:
		return new_tarval_from_double(0.0, mode);

	case irms_internal_boolean:
	case irms_int_number:
		return new_tarval_from_long(0l,  mode);

	case irms_reference:
		return new_tarval_from_long(_null_value, mode);
	default:
		panic("mode %F does not support null value", mode);
	}
}

ir_tarval *get_tarval_one(ir_mode *mode)
{
	switch (get_mode_sort(mode)) {
	case irms_internal_boolean:
		return tarval_b_true;

	case irms_float_number:
		return new_tarval_from_double(1.0, mode);

	case irms_reference:
	case irms_int_number:
		return new_tarval_from_long(1l, mode);
	default:
		panic("mode %F does not support one value", mode);
	}
}

ir_tarval *get_tarval_all_one(ir_mode *mode)
{
	switch (get_mode_sort(mode)) {
	case irms_int_number:
	case irms_internal_boolean:
	case irms_reference:
		return tarval_not(get_mode_null(mode));

	case irms_float_number:
		return new_tarval_from_double(1.0, mode);

	default:
		panic("mode %F does not support all-one value", mode);
	}
}

int tarval_is_constant(ir_tarval *tv)
{
	size_t const num_res = ARRAY_SIZE(nonconst_tvs);
	return tv < &nonconst_tvs[0] || &nonconst_tvs[num_res] <= tv;
}

ir_tarval *get_tarval_minus_one(ir_mode *mode)
{
	switch (get_mode_sort(mode)) {
	case irms_reference:
		return tarval_bad;

	case irms_float_number:
		return mode_is_signed(mode) ? new_tarval_from_double(-1.0, mode) : tarval_bad;

	case irms_int_number:
		return new_tarval_from_long(-1l, mode);

	default:
		panic("mode %F does not support minus one value", mode);
	}
}

ir_tarval *get_tarval_nan(ir_mode *mode)
{
	const float_descriptor_t *desc;

	if (get_mode_sort(mode) == irms_float_number) {
		desc = get_descriptor(mode);
		fc_get_qnan(desc, NULL);
		return get_tarval(fc_get_buffer(), fc_get_buffer_length(), mode);
	} else
		panic("mode %F does not support NaN value", mode);
}

ir_tarval *get_tarval_plus_inf(ir_mode *mode)
{
	if (get_mode_sort(mode) == irms_float_number) {
		const float_descriptor_t *desc = get_descriptor(mode);
		fc_get_plusinf(desc, NULL);
		return get_tarval(fc_get_buffer(), fc_get_buffer_length(), mode);
	} else
		panic("mode %F does not support +inf value", mode);
}

ir_tarval *get_tarval_minus_inf(ir_mode *mode)
{
	if (get_mode_sort(mode) == irms_float_number) {
		const float_descriptor_t *desc = get_descriptor(mode);
		fc_get_minusinf(desc, NULL);
		return get_tarval(fc_get_buffer(), fc_get_buffer_length(), mode);
	} else
		panic("mode %F does not support -inf value", mode);
}

/*
 * Arithmetic operations on tarvals ========================================
 */

int tarval_is_negative(ir_tarval *a)
{
	switch (get_mode_sort(a->mode)) {
	case irms_int_number:
		if (!mode_is_signed(a->mode)) return 0;
		else
			return sc_comp(a->value, get_mode_null(a->mode)->value) == ir_relation_less ? 1 : 0;

	case irms_float_number:
		return fc_is_negative((const fp_value*) a->value);

	default:
		panic("mode %F does not support negation value", a->mode);
	}
}

int tarval_is_null(ir_tarval *a)
{
	return
		a != tarval_bad &&
		a == get_mode_null(get_tarval_mode(a));
}

int tarval_is_one(ir_tarval *a)
{
	return
		a != tarval_bad &&
		a == get_mode_one(get_tarval_mode(a));
}

int tarval_is_all_one(ir_tarval *tv)
{
	return
		tv != tarval_bad &&
		tv == get_mode_all_one(get_tarval_mode(tv));
}

int tarval_is_minus_one(ir_tarval *a)
{
	return
		a != tarval_bad &&
		a == get_mode_minus_one(get_tarval_mode(a));
}

ir_relation tarval_cmp(ir_tarval *a, ir_tarval *b)
{
	if (a == tarval_bad || b == tarval_bad) {
		panic("Comparison with tarval_bad");
	}

	if (a == tarval_undefined || b == tarval_undefined)
		return ir_relation_false;

	if (a->mode != b->mode)
		return ir_relation_false;

	/* Here the two tarvals are unequal and of the same mode */
	switch (get_mode_sort(a->mode)) {
	case irms_float_number:
		/*
		 * BEWARE: we cannot compare a == b here, because
		 * a NaN is always Unordered to any other value, even to itself!
		 */
		return fc_comp((fp_value const*)a->value, (fp_value const*)b->value);

	case irms_reference:
	case irms_int_number:
		if (a == b)
			return ir_relation_equal;
		return sc_comp(a->value, b->value);

	case irms_internal_boolean:
		if (a == b)
			return ir_relation_equal;
		return a == tarval_b_true ? ir_relation_greater : ir_relation_less;

	default:
		panic("can't compare values of mode %F", a->mode);
	}
}

ir_tarval *tarval_convert_to(ir_tarval *src, ir_mode *dst_mode)
{
	char                    *buffer;
	fp_value                *res = NULL;
	const float_descriptor_t *desc;
	int                      len;

	assert(src);
	assert(dst_mode);

	if (src->mode == dst_mode)
		return src;

	switch (get_mode_sort(src->mode)) {
	/* cast float to something */
	case irms_float_number:
		switch (get_mode_sort(dst_mode)) {
		case irms_float_number:
			desc = get_descriptor(dst_mode);
			fc_cast((const fp_value*) src->value, desc, NULL);
			return get_tarval(fc_get_buffer(), fc_get_buffer_length(), dst_mode);

		case irms_int_number:
			res     = fc_int((const fp_value*) src->value, NULL);
			buffer = (char*) alloca(sc_get_buffer_length());
			if (! fc_flt2int(res, buffer, dst_mode))
				return tarval_bad;
			return get_tarval(buffer, sc_get_buffer_length(), dst_mode);

		default:
			break;
		}
		/* the rest can't be converted */
		return tarval_bad;

	/* cast int/characters to something */
	case irms_int_number:
		switch (get_mode_sort(dst_mode)) {

		case irms_reference:
		case irms_int_number:
			buffer = (char*) alloca(sc_get_buffer_length());
			memcpy(buffer, src->value, sc_get_buffer_length());
			return get_tarval_overflow(buffer, src->length, dst_mode);

		case irms_internal_boolean:
			/* XXX C semantics */
			if (src == get_mode_null(src->mode)) return tarval_b_false;
			else return tarval_b_true;

		case irms_float_number:
			/* XXX floating point unit does not understand internal integer
			 * representation, convert to string first, then create float from
			 * string */
			buffer = (char*) alloca(100);
			/* decimal string representation because hexadecimal output is
			 * interpreted unsigned by fc_val_from_str, so this is a HACK */
			len = snprintf(buffer, 100, "%s",
				sc_print(src->value, get_mode_size_bits(src->mode), SC_DEC, mode_is_signed(src->mode)));
			buffer[100 - 1] = '\0';
			desc = get_descriptor(dst_mode);
			fc_val_from_str(buffer, len, desc, NULL);
			return get_tarval(fc_get_buffer(), fc_get_buffer_length(), dst_mode);

		default:
			break;
		}
		break;

	case irms_internal_boolean:
		/* beware: this is C semantic for the INTERNAL boolean mode */
		if (get_mode_sort(dst_mode) == irms_int_number)
			return src == tarval_b_true ? get_mode_one(dst_mode) : get_mode_null(dst_mode);
		break;

	case irms_reference:
		if (get_mode_sort(dst_mode) == irms_int_number) {
			buffer = (char*) alloca(sc_get_buffer_length());
			memcpy(buffer, src->value, sc_get_buffer_length());
			sign_extend(buffer, src->mode);
			return get_tarval_overflow(buffer, src->length, dst_mode);
		}
		break;
	default:
		return tarval_bad;
	}

	return tarval_bad;
}

ir_tarval *tarval_not(ir_tarval *a)
{
	char *buffer;

	switch (get_mode_sort(a->mode)) {
	case irms_reference:
	case irms_int_number:
		buffer = (char*) alloca(sc_get_buffer_length());
		sc_not(a->value, buffer);
		return get_tarval(buffer, a->length, a->mode);

	case irms_internal_boolean:
		if (a == tarval_b_true)
			return tarval_b_false;
		if (a == tarval_b_false)
			return tarval_b_true;
		return tarval_bad;

	default:
		panic("bitwise negation is only allowed for integer and boolean");
	}
}

ir_tarval *tarval_neg(ir_tarval *a)
{
	char *buffer;

	assert(mode_is_num(a->mode)); /* negation only for numerical values */

	/* note: negation is allowed even for unsigned modes. */

	switch (get_mode_sort(a->mode)) {
	case irms_int_number:
		buffer = (char*) alloca(sc_get_buffer_length());
		sc_neg(a->value, buffer);
		return get_tarval_overflow(buffer, a->length, a->mode);

	case irms_float_number:
		fc_neg((const fp_value*) a->value, NULL);
		return get_tarval_overflow(fc_get_buffer(), fc_get_buffer_length(), a->mode);

	default:
		return tarval_bad;
	}
}

ir_tarval *tarval_add(ir_tarval *a, ir_tarval *b)
{
	char *buffer;

	if (mode_is_reference(a->mode) && a->mode != b->mode) {
		b = tarval_convert_to(b, a->mode);
	} else if (mode_is_reference(b->mode) && b->mode != a->mode) {
		a = tarval_convert_to(a, b->mode);
	}

	assert(a->mode == b->mode);

	switch (get_mode_sort(a->mode)) {
	case irms_reference:
	case irms_int_number:
		/* modes of a,b are equal, so result has mode of a as this might be the character */
		buffer = (char*) alloca(sc_get_buffer_length());
		sc_add(a->value, b->value, buffer);
		return get_tarval_overflow(buffer, a->length, a->mode);

	case irms_float_number:
		fc_add((const fp_value*) a->value, (const fp_value*) b->value, NULL);
		return get_tarval_overflow(fc_get_buffer(), fc_get_buffer_length(), a->mode);

	default:
		return tarval_bad;
	}
}

ir_tarval *tarval_sub(ir_tarval *a, ir_tarval *b, ir_mode *dst_mode)
{
	char    *buffer;

	if (dst_mode != NULL) {
		if (a->mode != dst_mode)
			a = tarval_convert_to(a, dst_mode);
		if (b->mode != dst_mode)
			b = tarval_convert_to(b, dst_mode);
	}
	assert(a->mode == b->mode);

	switch (get_mode_sort(a->mode)) {
	case irms_reference:
	case irms_int_number:
		/* modes of a,b are equal, so result has mode of a as this might be the character */
		buffer = (char*) alloca(sc_get_buffer_length());
		sc_sub(a->value, b->value, buffer);
		return get_tarval_overflow(buffer, a->length, a->mode);

	case irms_float_number:
		fc_sub((const fp_value*) a->value, (const fp_value*) b->value, NULL);
		return get_tarval_overflow(fc_get_buffer(), fc_get_buffer_length(), a->mode);

	default:
		return tarval_bad;
	}
}

ir_tarval *tarval_mul(ir_tarval *a, ir_tarval *b)
{
	char *buffer;

	assert(a->mode == b->mode);

	switch (get_mode_sort(a->mode)) {
	case irms_int_number:
		/* modes of a,b are equal */
		buffer = (char*) alloca(sc_get_buffer_length());
		sc_mul(a->value, b->value, buffer);
		return get_tarval_overflow(buffer, a->length, a->mode);

	case irms_float_number:
		fc_mul((const fp_value*) a->value, (const fp_value*) b->value, NULL);
		return get_tarval_overflow(fc_get_buffer(), fc_get_buffer_length(), a->mode);

	default:
		return tarval_bad;
	}
}

ir_tarval *tarval_div(ir_tarval *a, ir_tarval *b)
{
	ir_mode *mode = a->mode;
	assert(mode == b->mode);

	if (mode_is_int(mode)) {
		/* x/0 error */
		if (b == get_mode_null(mode))
			return tarval_bad;

		/* modes of a,b are equal */
		sc_div(a->value, b->value, NULL);
		return get_tarval(sc_get_buffer(), sc_get_buffer_length(), a->mode);
	} else {
		assert(mode_is_float(mode));
		fc_div((const fp_value*) a->value, (const fp_value*) b->value, NULL);
		return get_tarval_overflow(fc_get_buffer(), fc_get_buffer_length(), mode);
	}
}

ir_tarval *tarval_mod(ir_tarval *a, ir_tarval *b)
{
	assert((a->mode == b->mode) && mode_is_int(a->mode));

	/* x/0 error */
	if (b == get_mode_null(b->mode)) return tarval_bad;
	/* modes of a,b are equal */
	sc_mod(a->value, b->value, NULL);
	return get_tarval(sc_get_buffer(), sc_get_buffer_length(), a->mode);
}

ir_tarval *tarval_divmod(ir_tarval *a, ir_tarval *b, ir_tarval **mod)
{
	int len = sc_get_buffer_length();
	char *div_res = (char*) alloca(len);
	char *mod_res = (char*) alloca(len);

	assert((a->mode == b->mode) && mode_is_int(a->mode));

	/* x/0 error */
	if (b == get_mode_null(b->mode)) return tarval_bad;
	/* modes of a,b are equal */
	sc_divmod(a->value, b->value, div_res, mod_res);
	*mod = get_tarval(mod_res, len, a->mode);
	return get_tarval(div_res, len, a->mode);
}

ir_tarval *tarval_abs(ir_tarval *a)
{
	char *buffer;

	assert(mode_is_num(a->mode));

	switch (get_mode_sort(a->mode)) {
	case irms_int_number:
		if (sc_comp(a->value, get_mode_null(a->mode)->value) == ir_relation_less) {
			buffer = (char*) alloca(sc_get_buffer_length());
			sc_neg(a->value, buffer);
			return get_tarval_overflow(buffer, a->length, a->mode);
		}
		return a;

	case irms_float_number:
		if (fc_is_negative((fp_value const*)a)) {
			fc_neg((const fp_value*) a->value, NULL);
			return get_tarval_overflow(fc_get_buffer(), fc_get_buffer_length(), a->mode);
		}
		return a;

	default:
		break;
	}
	return tarval_bad;
}

ir_tarval *tarval_and(ir_tarval *a, ir_tarval *b)
{
	assert(a->mode == b->mode);

	switch (get_mode_sort(a->mode)) {
	case irms_internal_boolean:
		return (a == tarval_b_false) ? a : b;

	case irms_reference:
	case irms_int_number:
		sc_and(a->value, b->value, NULL);
		return get_tarval(sc_get_buffer(), sc_get_buffer_length(), a->mode);

	default:
		panic("operation not defined on mode");
	}
}

ir_tarval *tarval_andnot(ir_tarval *a, ir_tarval *b)
{
	assert(a->mode == b->mode);

	switch (get_mode_sort(a->mode)) {
	case irms_internal_boolean:
		return a == tarval_b_true && b == tarval_b_false ? tarval_b_true : tarval_b_false;

	case irms_reference:
	case irms_int_number:
		sc_andnot(a->value, b->value, NULL);
		return get_tarval(sc_get_buffer(), sc_get_buffer_length(), a->mode);

	default:
		panic("operation not defined on mode");
	}
}

ir_tarval *tarval_or(ir_tarval *a, ir_tarval *b)
{
	assert(a->mode == b->mode);

	switch (get_mode_sort(a->mode)) {
	case irms_internal_boolean:
		return (a == tarval_b_true) ? a : b;

	case irms_reference:
	case irms_int_number:
		sc_or(a->value, b->value, NULL);
		return get_tarval(sc_get_buffer(), sc_get_buffer_length(), a->mode);

	default:
		panic("operation not defined on mode");
	}
}

ir_tarval *tarval_eor(ir_tarval *a, ir_tarval *b)
{
	assert((a->mode == b->mode));

	switch (get_mode_sort(a->mode)) {
	case irms_internal_boolean:
		return (a == b)? tarval_b_false : tarval_b_true;

	case irms_reference:
	case irms_int_number:
		sc_xor(a->value, b->value, NULL);
		return get_tarval(sc_get_buffer(), sc_get_buffer_length(), a->mode);

	default:
		panic("operation not defined on mode");
	}
}

ir_tarval *tarval_shl(ir_tarval *a, ir_tarval *b)
{
	char *temp_val = NULL;

	assert(mode_is_int(a->mode) && mode_is_int(b->mode));

	if (get_mode_modulo_shift(a->mode) != 0) {
		temp_val = (char*) alloca(sc_get_buffer_length());

		sc_val_from_ulong(get_mode_modulo_shift(a->mode), temp_val);
		sc_mod(b->value, temp_val, temp_val);
	} else
		temp_val = (char*)b->value;

	sc_shl(a->value, temp_val, get_mode_size_bits(a->mode), mode_is_signed(a->mode), NULL);
	return get_tarval(sc_get_buffer(), sc_get_buffer_length(), a->mode);
}

ir_tarval *tarval_shl_unsigned(ir_tarval *a, unsigned b)
{
	ir_mode *mode   = a->mode;
	unsigned modulo = get_mode_modulo_shift(mode);
	if (modulo != 0)
		b %= modulo;
	assert((unsigned)(long)b==b);
	sc_shlI(a->value, (long)b, get_mode_size_bits(mode), mode_is_signed(mode), NULL);
	return get_tarval(sc_get_buffer(), sc_get_buffer_length(), mode);
}

ir_tarval *tarval_shr(ir_tarval *a, ir_tarval *b)
{
	char *temp_val = NULL;

	assert(mode_is_int(a->mode) && mode_is_int(b->mode));

	if (get_mode_modulo_shift(a->mode) != 0) {
		temp_val = (char*) alloca(sc_get_buffer_length());

		sc_val_from_ulong(get_mode_modulo_shift(a->mode), temp_val);
		sc_mod(b->value, temp_val, temp_val);
	} else
		temp_val = (char*)b->value;

	sc_shr(a->value, temp_val, get_mode_size_bits(a->mode), mode_is_signed(a->mode), NULL);
	return get_tarval(sc_get_buffer(), sc_get_buffer_length(), a->mode);
}

ir_tarval *tarval_shr_unsigned(ir_tarval *a, unsigned b)
{
	ir_mode *mode   = a->mode;
	unsigned modulo = get_mode_modulo_shift(mode);
	if (modulo != 0)
		b %= modulo;
	assert((unsigned)(long)b==b);
	sc_shrI(a->value, (long)b, get_mode_size_bits(mode), mode_is_signed(mode), NULL);
	return get_tarval(sc_get_buffer(), sc_get_buffer_length(), mode);
}

ir_tarval *tarval_shrs(ir_tarval *a, ir_tarval *b)
{
	char *temp_val = NULL;

	assert(mode_is_int(a->mode) && mode_is_int(b->mode));

	if (get_mode_modulo_shift(a->mode) != 0) {
		temp_val = (char*) alloca(sc_get_buffer_length());

		sc_val_from_ulong(get_mode_modulo_shift(a->mode), temp_val);
		sc_mod(b->value, temp_val, temp_val);
	} else
		temp_val = (char*)b->value;

	sc_shrs(a->value, temp_val, get_mode_size_bits(a->mode), mode_is_signed(a->mode), NULL);
	return get_tarval(sc_get_buffer(), sc_get_buffer_length(), a->mode);
}

ir_tarval *tarval_shrs_unsigned(ir_tarval *a, unsigned b)
{
	ir_mode *mode   = a->mode;
	unsigned modulo = get_mode_modulo_shift(mode);
	if (modulo != 0)
		b %= modulo;
	assert((unsigned)(long)b==b);
	sc_shrsI(a->value, (long)b, get_mode_size_bits(mode), mode_is_signed(mode), NULL);
	return get_tarval(sc_get_buffer(), sc_get_buffer_length(), mode);
}

ir_tarval *tarval_rotl(ir_tarval *a, ir_tarval *b)
{
	char *temp_val = NULL;

	assert(mode_is_int(a->mode) && mode_is_int(b->mode));

	if (get_mode_modulo_shift(a->mode) != 0) {
		temp_val = (char*) alloca(sc_get_buffer_length());

		sc_val_from_ulong(get_mode_modulo_shift(a->mode), temp_val);
		sc_mod(b->value, temp_val, temp_val);
	} else
		temp_val = (char*)b->value;

	sc_rotl(a->value, temp_val, get_mode_size_bits(a->mode), mode_is_signed(a->mode), NULL);
	return get_tarval(sc_get_buffer(), sc_get_buffer_length(), a->mode);
}

int tarval_snprintf(char *buf, size_t len, ir_tarval *tv)
{
	static const tarval_mode_info default_info = { TVO_NATIVE, NULL, NULL };

	const char *str;
	char tv_buf[100];
	const tarval_mode_info *mode_info;
	const char *prefix, *suffix;

	mode_info = (const tarval_mode_info*) tv->mode->tv_priv;
	if (! mode_info)
		mode_info = &default_info;
	prefix = mode_info->mode_prefix ? mode_info->mode_prefix : "";
	suffix = mode_info->mode_suffix ? mode_info->mode_suffix : "";

	switch (get_mode_sort(tv->mode)) {
	case irms_reference:
		if (tv == tv->mode->null) return snprintf(buf, len, "NULL");
		/* FALLTHROUGH */
	case irms_int_number:
		switch (mode_info->mode_output) {

		case TVO_DECIMAL:
			str = sc_print(tv->value, get_mode_size_bits(tv->mode), SC_DEC, mode_is_signed(tv->mode));
			break;

		case TVO_OCTAL:
			str = sc_print(tv->value, get_mode_size_bits(tv->mode), SC_OCT, 0);
			break;

		case TVO_NATIVE:
			prefix = "0x";
		case TVO_HEX:
		default:
			str = sc_print(tv->value, get_mode_size_bits(tv->mode), SC_HEX, 0);
			break;
		}
		return snprintf(buf, len, "%s%s%s", prefix, str, suffix);

	case irms_float_number:
		switch (mode_info->mode_output) {
		case TVO_HEX:
			return snprintf(buf, len, "%s%s%s", prefix, fc_print((const fp_value*) tv->value, tv_buf, sizeof(tv_buf), FC_PACKED), suffix);

		case TVO_HEXFLOAT:
			return snprintf(buf, len, "%s%s%s", prefix, fc_print((const fp_value*) tv->value, tv_buf, sizeof(tv_buf), FC_HEX), suffix);

		case TVO_FLOAT:
		case TVO_NATIVE:
		default:
			return snprintf(buf, len, "%s%s%s", prefix, fc_print((const fp_value*) tv->value, tv_buf, sizeof(tv_buf), FC_DEC), suffix);
		}

	case irms_internal_boolean:
		switch (mode_info->mode_output) {

		case TVO_DECIMAL:
		case TVO_OCTAL:
		case TVO_HEX:
		case TVO_BINARY:
			return snprintf(buf, len, "%s%c%s", prefix, (tv == tarval_b_true) ? '1' : '0', suffix);

		case TVO_NATIVE:
		default:
			return snprintf(buf, len, "%s%s%s", prefix, (tv == tarval_b_true) ? "true" : "false", suffix);
		}

	default:
		if (tv == tarval_bad)
			return snprintf(buf, len, "<TV_BAD>");
		else if (tv == tarval_undefined)
			return snprintf(buf, len, "<TV_UNDEFINED>");
		else if (tv == tarval_reachable)
			return snprintf(buf, len, "<TV_REACHABLE>");
		else if (tv == tarval_unreachable)
			return snprintf(buf, len, "<TV_UNREACHABLE>");
		else
			return snprintf(buf, len, "<TV_??""?>");
	}
}

int tarval_printf(ir_tarval *tv)
{
	char buf[1024];
	int res;

	res = tarval_snprintf(buf, sizeof(buf), tv);
	assert(res < (int) sizeof(buf) && "buffer to small for tarval_snprintf");
	printf("%s", buf);
	return res;
}

char *get_tarval_bitpattern(ir_tarval *tv)
{
	int i, j, pos = 0;
	int n = get_mode_size_bits(tv->mode);
	int bytes = (n + 7) / 8;
	char *res = XMALLOCN(char, n + 1);
	unsigned char byte;

	for (i = 0; i < bytes; i++) {
		byte = get_tarval_sub_bits(tv, i);
		for (j = 1; j < 256; j <<= 1)
			if (pos < n)
				res[pos++] = j & byte ? '1' : '0';
	}

	res[n] = '\0';

	return res;
}

unsigned char get_tarval_sub_bits(ir_tarval *tv, unsigned byte_ofs)
{
	switch (get_mode_arithmetic(tv->mode)) {
	case irma_twos_complement:
		return sc_sub_bits(tv->value, get_mode_size_bits(tv->mode), byte_ofs);
	case irma_ieee754:
	case irma_x86_extended_float:
		return fc_sub_bits((const fp_value*) tv->value, get_mode_size_bits(tv->mode), byte_ofs);
	default:
		panic("arithmetic mode not supported");
	}
}

int  set_tarval_mode_output_option(ir_mode *mode, const tarval_mode_info *modeinfo)
{
	assert(mode);

	mode->tv_priv = modeinfo;
	return 0;
}

const tarval_mode_info *get_tarval_mode_output_option(ir_mode *mode)
{
	assert(mode);

	return (const tarval_mode_info*) mode->tv_priv;
}

int tarval_is_single_bit(ir_tarval *tv)
{
	int i, l;
	int bits;

	if (!tv || tv == tarval_bad) return 0;
	if (! mode_is_int(tv->mode)) return 0;

	l = get_mode_size_bytes(tv->mode);
	for (bits = 0, i = l - 1; i >= 0; --i) {
		unsigned char v = get_tarval_sub_bits(tv, (unsigned)i);

		/* check for more than one bit in these */
		if (v) {
			if (v & (v-1))
				return 0;
			if (++bits > 1)
				return 0;
		}
	}
	return bits;
}

int get_tarval_popcount(ir_tarval *tv)
{
	int i, l;
	int bits;

	if (!tv || tv == tarval_bad) return -1;
	if (! mode_is_int(tv->mode)) return -1;

	l = get_mode_size_bytes(tv->mode);
	for (bits = 0, i = l - 1; i >= 0; --i) {
		unsigned char v = get_tarval_sub_bits(tv, (unsigned)i);

		bits += popcount(v);
	}
	return bits;
}

int get_tarval_lowest_bit(ir_tarval *tv)
{
	int i, l;

	if (!tv || tv == tarval_bad) return -1;
	if (! mode_is_int(tv->mode)) return -1;

	l = get_mode_size_bytes(tv->mode);
	for (i = 0; i < l; ++i) {
		unsigned char v = get_tarval_sub_bits(tv, (unsigned)i);

		if (v)
			return ntz(v) + i * 8;
	}
	return -1;
}

int get_tarval_highest_bit(ir_tarval *tv)
{
	int i, l;

	if (!tv || tv == tarval_bad) return -1;
	if (! mode_is_int(tv->mode)) return -1;

	l = get_mode_size_bytes(tv->mode);
	for (i = l - 1; i >= 0; --i) {
		unsigned char v = get_tarval_sub_bits(tv, (unsigned)i);

		if (v)
		  return 8*sizeof(unsigned) - nlz(v) + i * 8 - 1;
	}
	return -1;
}

int tarval_zero_mantissa(ir_tarval *tv)
{
	assert(get_mode_arithmetic(tv->mode) == irma_ieee754
	       || get_mode_arithmetic(tv->mode) == irma_x86_extended_float);
	return fc_zero_mantissa((const fp_value*) tv->value);
}

int tarval_get_exponent(ir_tarval *tv)
{
	assert(get_mode_arithmetic(tv->mode) == irma_ieee754
	       || get_mode_arithmetic(tv->mode) == irma_x86_extended_float);
	return fc_get_exponent((const fp_value*) tv->value);
}

int tarval_ieee754_can_conv_lossless(ir_tarval *tv, ir_mode *mode)
{
	const float_descriptor_t *desc = get_descriptor(mode);
	return fc_can_lossless_conv_to((const fp_value*) tv->value, desc);
}

unsigned tarval_ieee754_get_exact(void)
{
	return fc_is_exact();
}

int tarval_is_NaN(ir_tarval *tv)
{
	if (! mode_is_float(tv->mode))
		return 0;
	return fc_is_nan((const fp_value*) tv->value);
}

int tarval_is_plus_inf(ir_tarval *tv)
{
	if (! mode_is_float(tv->mode))
		return 0;
	return fc_is_inf((const fp_value*) tv->value)
		&& !fc_is_negative((const fp_value*) tv->value);
}

int tarval_is_minus_inf(ir_tarval *tv)
{
	if (! mode_is_float(tv->mode))
		return 0;
	return fc_is_inf((const fp_value*) tv->value)
		&& fc_is_negative((const fp_value*) tv->value);
}

int tarval_is_finite(ir_tarval *tv)
{
	if (mode_is_float(tv->mode))
		return !fc_is_nan((const fp_value*) tv->value)
			&& !fc_is_inf((const fp_value*) tv->value);
	return 1;
}

void tarval_set_integer_overflow_mode(tarval_int_overflow_mode_t ov_mode)
{
	int_overflow_mode = ov_mode;
}

tarval_int_overflow_mode_t tarval_get_integer_overflow_mode(void)
{
	return int_overflow_mode;
}

/**
 * default mode_info for output as HEX
 */
static const tarval_mode_info hex_output = {
	TVO_HEX,
	"0x",
	NULL,
};

void init_tarval_1(long null_value, int support_quad_precision)
{
	_null_value = null_value;

	/* initialize the sets holding the tarvals with a comparison function and
	 * an initial size, which is the expected number of constants */
	tarvals = new_set(cmp_tv, N_CONSTANTS);
	values  = new_set(memcmp, N_CONSTANTS);
	/* calls init_strcalc() with needed size */
	init_fltcalc(support_quad_precision ? 112 : 64);
}

void init_tarval_2(void)
{
	tarval_bad->kind          = k_tarval;
	tarval_bad->mode          = mode_BAD;

	tarval_undefined->kind    = k_tarval;
	tarval_undefined->mode    = mode_ANY;

	tarval_b_true->kind       = k_tarval;
	tarval_b_true->mode       = mode_b;

	tarval_b_false->kind      = k_tarval;
	tarval_b_false->mode      = mode_b;

	tarval_unreachable->kind  = k_tarval;
	tarval_unreachable->mode  = mode_X;

	tarval_reachable->kind    = k_tarval;
	tarval_reachable->mode    = mode_X;

	/*
	 * assign output modes that are compatible with the
	 * old implementation: Hex output
	 */
	set_tarval_mode_output_option(mode_Bs, &hex_output);
	set_tarval_mode_output_option(mode_Bu, &hex_output);
	set_tarval_mode_output_option(mode_Hs, &hex_output);
	set_tarval_mode_output_option(mode_Hu, &hex_output);
	set_tarval_mode_output_option(mode_Is, &hex_output);
	set_tarval_mode_output_option(mode_Iu, &hex_output);
	set_tarval_mode_output_option(mode_Ls, &hex_output);
	set_tarval_mode_output_option(mode_Lu, &hex_output);
	set_tarval_mode_output_option(mode_P,  &hex_output);
}

void finish_tarval(void)
{
	finish_strcalc();
	finish_fltcalc();
	del_set(tarvals); tarvals = NULL;
	del_set(values);  values = NULL;
}

int (is_tarval)(const void *thing)
{
	return _is_tarval(thing);
}
