/*-------------------------------------------------------------------------
 *
 * parse_oper.h
 *		handle operator things for parser
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/parser/parse_oper.h,v 1.36 2004/12/31 22:03:38 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_OPER_H
#define PARSE_OPER_H

#include "access/htup.h"
#include "parser/parse_node.h"


typedef HeapTuple Operator;

/* Routines to look up an operator given name and exact input type(s) */
extern Oid LookupOperName(List *opername, Oid oprleft, Oid oprright,
			   bool noError);
extern Oid LookupOperNameTypeNames(List *opername, TypeName *oprleft,
						TypeName *oprright, bool noError);

/* Routines to find operators matching a name and given input types */
/* NB: the selected operator may require coercion of the input types! */
extern Operator oper(List *op, Oid arg1, Oid arg2, bool noError);
extern Operator right_oper(List *op, Oid arg, bool noError);
extern Operator left_oper(List *op, Oid arg, bool noError);

/* Routines to find operators that DO NOT require coercion --- ie, their */
/* input types are either exactly as given, or binary-compatible */
extern Operator compatible_oper(List *op, Oid arg1, Oid arg2, bool noError);

/* currently no need for compatible_left_oper/compatible_right_oper */

/* Routines for identifying "=", "<", ">" operators for a type */
extern Operator equality_oper(Oid argtype, bool noError);
extern Operator ordering_oper(Oid argtype, bool noError);
extern Operator reverse_ordering_oper(Oid argtype, bool noError);

/* Convenience routines for common calls on the above */
extern Oid	compatible_oper_opid(List *op, Oid arg1, Oid arg2, bool noError);
extern Oid	equality_oper_funcid(Oid argtype);
extern Oid	ordering_oper_opid(Oid argtype);
extern Oid	reverse_ordering_oper_opid(Oid argtype);

/* Extract operator OID or underlying-function OID from an Operator tuple */
extern Oid	oprid(Operator op);
extern Oid	oprfuncid(Operator op);

/* Build expression tree for an operator invocation */
extern Expr *make_op(ParseState *pstate, List *opname,
		Node *ltree, Node *rtree);
extern Expr *make_scalar_array_op(ParseState *pstate, List *opname,
					 bool useOr,
					 Node *ltree, Node *rtree);
extern Expr *make_op_expr(ParseState *pstate, Operator op,
			 Node *ltree, Node *rtree,
			 Oid ltypeId, Oid rtypeId);

#endif   /* PARSE_OPER_H */
