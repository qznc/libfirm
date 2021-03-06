/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief       Interface for assembler output.
 * @author      Matthias Braun
 * @date        12.03.2007
 */
#include "bedwarf.h"
#include "beemitter.h"
#include "be_t.h"
#include "error.h"
#include "irnode_t.h"
#include "irprintf.h"
#include "ident.h"
#include "tv.h"
#include "dbginfo.h"

FILE           *emit_file;
struct obstack  emit_obst;

void be_emit_init(FILE *file)
{
	emit_file       = file;
	obstack_init(&emit_obst);
}

void be_emit_exit(void)
{
	obstack_free(&emit_obst, NULL);
}

void be_emit_irvprintf(const char *fmt, va_list args)
{
	ir_obst_vprintf(&emit_obst, fmt, args);
}

void be_emit_irprintf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	be_emit_irvprintf(fmt, ap);
	va_end(ap);
}

void be_emit_write_line(void)
{
	size_t  len  = obstack_object_size(&emit_obst);
	char   *line = (char*)obstack_finish(&emit_obst);

	fwrite(line, 1, len, emit_file);
	obstack_free(&emit_obst, line);
}

void be_emit_pad_comment(void)
{
	size_t len = obstack_object_size(&emit_obst);
	if (len > 30)
		len = 30;
	/* 34 spaces */
	be_emit_string_len("                                  ", 34 - len);
}

void be_emit_finish_line_gas(const ir_node *node)
{
	dbg_info  *dbg;
	src_loc_t  loc;

	if (node == NULL || !be_options.verbose_asm) {
		be_emit_char('\n');
		be_emit_write_line();
		return;
	}

	be_emit_pad_comment();
	be_emit_cstring("/* ");
	be_emit_irprintf("%+F ", node);

	dbg = get_irn_dbg_info(node);
	loc = ir_retrieve_dbg_info(dbg);
	if (loc.file) {
		be_emit_string(loc.file);
		if (loc.line != 0) {
			be_emit_irprintf(":%u", loc.line);
			if (loc.column != 0) {
				be_emit_irprintf(":%u", loc.column);
			}
		}
	}
	be_emit_cstring(" */\n");
	be_emit_write_line();
}

void be_emit_nothing(ir_node const *const node)
{
	(void)node;
}

void be_emit_node(ir_node const *const node)
{
	be_dwarf_location(get_irn_dbg_info(node));
	ir_op     *const op   = get_irn_op(node);
	emit_func *const emit = get_generic_function_ptr(emit_func, op);
	DEBUG_ONLY(if (!emit) panic("no emit handler for node %+F (%+G, graph %+F)\n", node, node, get_irn_irg(node));)
	emit(node);
}
