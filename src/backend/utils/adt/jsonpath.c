/*-------------------------------------------------------------------------
 *
 * jsonpath.c
 *	 Input/output and supporting routines for jsonpath
 *
 * jsonpath expression is a chain of path items.  First path item is $, $var,
 * literal or arithmetic expression.  Subsequent path items are accessors
 * (.key, .*, [subscripts], [*]), filters (? (predicate)) and methods (.type(),
 * .size() etc).
 *
 * For instance, structure of path items for simple expression:
 *
 *		$.a[*].type()
 *
 * is pretty evident:
 *
 *		$ => .a => [*] => .type()
 *
 * Some path items such as arithmetic operations, predicates or array
 * subscripts may comprise subtrees.  For instance, more complex expression
 *
 *		($.a + $[1 to 5, 7] ? (@ > 3).double()).type()
 *
 * have following structure of path items:
 *
 *			  +  =>  .type()
 *		  ___/ \___
 *		 /		   \
 *		$ => .a 	$  =>  []  =>	?  =>  .double()
 *						  _||_		|
 *						 /	  \ 	>
 *						to	  to   / \
 *					   / \	  /   @   3
 *					  1   5  7
 *
 * Binary encoding of jsonpath constitutes a sequence of 4-bytes aligned
 * variable-length path items connected by links.  Every item has a header
 * consisting of item type (enum JsonPathItemType) and offset of next item
 * (zero means no next item).  After the header, item may have payload
 * depending on item type.  For instance, payload of '.key' accessor item is
 * length of key name and key name itself.  Payload of '>' arithmetic operator
 * item is offsets of right and left operands.
 *
 * So, binary representation of sample expression above is:
 * (bottom arrows are next links, top lines are argument links)
 *
 *								  _____
 *		 _____				  ___/____ \				__
 *	  _ /_	  \ 		_____/__/____ \ \	   __    _ /_ \
 *	 / /  \    \	   /	/  /	 \ \ \ 	  /  \  / /  \ \
 * +(LR)  $ .a	$  [](* to *, * to *) 1 5 7 ?(A)  >(LR)   @ 3 .double() .type()
 * |	  |  ^	|  ^|						 ^|					  ^		   ^
 * |	  |__|	|__||________________________||___________________|		   |
 * |_______________________________________________________________________|
 *
 * Copyright (c) 2019-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	src/backend/utils/adt/jsonpath.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "funcapi.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "utils/builtins.h"
#include "utils/formatting.h"
#include "utils/json.h"
#include "utils/jsonpath.h"


/* Context for jsonpath encoding. */
typedef struct JsonPathEncodingContext
{
	StringInfo	buf;		/* output buffer */
	bool		ext;		/* PG extensions are enabled? */
} JsonPathEncodingContext;

static Datum jsonPathFromCstring(char *in, int len);
static char *jsonPathToCstring(StringInfo out, JsonPath *in,
							   int estimated_len);
static int	flattenJsonPathParseItem(JsonPathEncodingContext *cxt,
									 JsonPathParseItem *item,
									 int nestingLevel,
									 bool insideArraySubscript);
static void alignStringInfoInt(StringInfo buf);
static int32 reserveSpaceForItemPointer(StringInfo buf);
static void printJsonPathItem(StringInfo buf, JsonPathItem *v, bool inKey,
							  bool printBracketes);
static int	operationPriority(JsonPathItemType op);


/**************************** INPUT/OUTPUT ********************************/

/*
 * jsonpath type input function
 */
Datum
jsonpath_in(PG_FUNCTION_ARGS)
{
	char	   *in = PG_GETARG_CSTRING(0);
	int			len = strlen(in);

	return jsonPathFromCstring(in, len);
}

/*
 * jsonpath type recv function
 *
 * The type is sent as text in binary mode, so this is almost the same
 * as the input function, but it's prefixed with a version number so we
 * can change the binary format sent in future if necessary. For now,
 * only version 1 is supported.
 */
Datum
jsonpath_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	int			version = pq_getmsgint(buf, 1);
	char	   *str;
	int			nbytes;

	if (version == JSONPATH_VERSION)
		str = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);
	else
		elog(ERROR, "unsupported jsonpath version number: %d", version);

	return jsonPathFromCstring(str, nbytes);
}

/*
 * jsonpath type output function
 */
Datum
jsonpath_out(PG_FUNCTION_ARGS)
{
	JsonPath   *in = PG_GETARG_JSONPATH_P(0);

	PG_RETURN_CSTRING(jsonPathToCstring(NULL, in, VARSIZE(in)));
}

/*
 * jsonpath type send function
 *
 * Just send jsonpath as a version number, then a string of text
 */
Datum
jsonpath_send(PG_FUNCTION_ARGS)
{
	JsonPath   *in = PG_GETARG_JSONPATH_P(0);
	StringInfoData buf;
	StringInfoData jtext;
	int			version = JSONPATH_VERSION;

	initStringInfo(&jtext);
	(void) jsonPathToCstring(&jtext, in, VARSIZE(in));

	pq_begintypsend(&buf);
	pq_sendint8(&buf, version);
	pq_sendtext(&buf, jtext.data, jtext.len);
	pfree(jtext.data);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * Converts C-string to a jsonpath value.
 *
 * Uses jsonpath parser to turn string into an AST, then
 * flattenJsonPathParseItem() does second pass turning AST into binary
 * representation of jsonpath.
 */
static Datum
jsonPathFromCstring(char *in, int len)
{
	JsonPathEncodingContext cxt;
	JsonPathParseResult *jsonpath = parsejsonpath(in, len);
	JsonPath   *res;
	StringInfoData buf;

	initStringInfo(&buf);
	enlargeStringInfo(&buf, 4 * len /* estimation */ );

	appendStringInfoSpaces(&buf, JSONPATH_HDRSZ);

	if (!jsonpath)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"", "jsonpath",
						in)));

	cxt.buf = &buf;
	cxt.ext = jsonpath->ext;

	flattenJsonPathParseItem(&cxt, jsonpath->expr, 0, false);

	res = (JsonPath *) buf.data;
	SET_VARSIZE(res, buf.len);
	res->header = JSONPATH_VERSION;
	if (jsonpath->lax)
		res->header |= JSONPATH_LAX;
	if (jsonpath->ext)
		res->header |= JSONPATH_EXT;

	PG_RETURN_JSONPATH_P(res);
}

/*
 * Converts jsonpath value to a C-string.
 *
 * If 'out' argument is non-null, the resulting C-string is stored inside the
 * StringBuffer.  The resulting string is always returned.
 */
static char *
jsonPathToCstring(StringInfo out, JsonPath *in, int estimated_len)
{
	StringInfoData buf;
	JsonPathItem v;

	if (!out)
	{
		out = &buf;
		initStringInfo(out);
	}
	enlargeStringInfo(out, estimated_len);

	if (in->header & JSONPATH_EXT)
		appendBinaryStringInfo(out, "pg ", 3);
	if (!(in->header & JSONPATH_LAX))
		appendBinaryStringInfo(out, "strict ", 7);

	jspInit(&v, in);
	printJsonPathItem(out, &v, false, v.type != jpiSequence);

	return out->data;
}

static void
checkJsonPathExtensionsEnabled(JsonPathEncodingContext *cxt,
							   JsonPathItemType type)
{
	if (!cxt->ext)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("%s contains extended operators that were not enabled", "jsonpath"),
				 errhint("use \"%s\" modifier at the start of %s string to enable extensions",
						 "pg", "jsonpath")));
}

/*
 * Recursive function converting given jsonpath parse item and all its
 * children into a binary representation.
 */
static int
flattenJsonPathParseItem(JsonPathEncodingContext *cxt, JsonPathParseItem *item,
						 int nestingLevel, bool insideArraySubscript)
{
	StringInfo	buf = cxt->buf;
	/* position from beginning of jsonpath data */
	int32		pos = buf->len - JSONPATH_HDRSZ;
	int32		chld;
	int32		next;
	int			argNestingLevel = 0;

	check_stack_depth();
	CHECK_FOR_INTERRUPTS();

	appendStringInfoChar(buf, (char) (item->type));

	/*
	 * We align buffer to int32 because a series of int32 values often goes
	 * after the header, and we want to read them directly by dereferencing
	 * int32 pointer (see jspInitByBuffer()).
	 */
	alignStringInfoInt(buf);

	/*
	 * Reserve space for next item pointer.  Actual value will be recorded
	 * later, after next and children items processing.
	 */
	next = reserveSpaceForItemPointer(buf);

	switch (item->type)
	{
		case jpiString:
		case jpiVariable:
		case jpiKey:
			appendBinaryStringInfo(buf, (char *) &item->value.string.len,
								   sizeof(item->value.string.len));
			appendBinaryStringInfo(buf, item->value.string.val,
								   item->value.string.len);
			appendStringInfoChar(buf, '\0');
			break;
		case jpiNumeric:
			appendBinaryStringInfo(buf, (char *) item->value.numeric,
								   VARSIZE(item->value.numeric));
			break;
		case jpiBool:
			appendBinaryStringInfo(buf, (char *) &item->value.boolean,
								   sizeof(item->value.boolean));
			break;
		case jpiAnd:
		case jpiOr:
		case jpiEqual:
		case jpiNotEqual:
		case jpiLess:
		case jpiGreater:
		case jpiLessOrEqual:
		case jpiGreaterOrEqual:
		case jpiAdd:
		case jpiSub:
		case jpiMul:
		case jpiDiv:
		case jpiMod:
		case jpiStartsWith:
			{
				/*
				 * First, reserve place for left/right arg's positions, then
				 * record both args and sets actual position in reserved
				 * places.
				 */
				int32		left = reserveSpaceForItemPointer(buf);
				int32		right = reserveSpaceForItemPointer(buf);

				chld = !item->value.args.left ? pos :
					flattenJsonPathParseItem(cxt, item->value.args.left,
											 nestingLevel + argNestingLevel,
											 insideArraySubscript);
				*(int32 *) (buf->data + left) = chld - pos;

				chld = !item->value.args.right ? pos :
					flattenJsonPathParseItem(cxt, item->value.args.right,
											 nestingLevel + argNestingLevel,
											 insideArraySubscript);
				*(int32 *) (buf->data + right) = chld - pos;
			}
			break;
		case jpiLikeRegex:
			{
				int32		offs;

				appendBinaryStringInfo(buf,
									   (char *) &item->value.like_regex.flags,
									   sizeof(item->value.like_regex.flags));
				offs = reserveSpaceForItemPointer(buf);
				appendBinaryStringInfo(buf,
									   (char *) &item->value.like_regex.patternlen,
									   sizeof(item->value.like_regex.patternlen));
				appendBinaryStringInfo(buf, item->value.like_regex.pattern,
									   item->value.like_regex.patternlen);
				appendStringInfoChar(buf, '\0');

				chld = flattenJsonPathParseItem(cxt, item->value.like_regex.expr,
												nestingLevel,
												insideArraySubscript);
				*(int32 *) (buf->data + offs) = chld - pos;
			}
			break;
		case jpiFilter:
			argNestingLevel++;
			/* FALLTHROUGH */
		case jpiIsUnknown:
		case jpiNot:
		case jpiPlus:
		case jpiMinus:
		case jpiExists:
		case jpiDatetime:
		case jpiArray:
			{
				int32		arg = reserveSpaceForItemPointer(buf);

				if (item->type == jpiArray)
					checkJsonPathExtensionsEnabled(cxt, item->type);

				if (!item->value.arg)
					break;

				chld = flattenJsonPathParseItem(cxt, item->value.arg,
												nestingLevel + argNestingLevel,
												insideArraySubscript);
				*(int32 *) (buf->data + arg) = chld - pos;
			}
			break;
		case jpiNull:
			break;
		case jpiRoot:
			break;
		case jpiAnyArray:
		case jpiAnyKey:
			break;
		case jpiCurrent:
			if (nestingLevel <= 0)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("@ is not allowed in root expressions")));
			break;
		case jpiLast:
			if (!insideArraySubscript)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("LAST is allowed only in array subscripts")));
			break;
		case jpiIndexArray:
			{
				int32		nelems = item->value.array.nelems;
				int			offset;
				int			i;

				appendBinaryStringInfo(buf, (char *) &nelems, sizeof(nelems));

				offset = buf->len;

				appendStringInfoSpaces(buf, sizeof(int32) * 2 * nelems);

				for (i = 0; i < nelems; i++)
				{
					int32	   *ppos;
					int32		topos;
					int32		frompos =
					flattenJsonPathParseItem(cxt,
											 item->value.array.elems[i].from,
											 nestingLevel, true) - pos;

					if (item->value.array.elems[i].to)
						topos = flattenJsonPathParseItem(cxt,
														 item->value.array.elems[i].to,
														 nestingLevel, true) - pos;
					else
						topos = 0;

					ppos = (int32 *) &buf->data[offset + i * 2 * sizeof(int32)];

					ppos[0] = frompos;
					ppos[1] = topos;
				}
			}
			break;
		case jpiAny:
			appendBinaryStringInfo(buf,
								   (char *) &item->value.anybounds.first,
								   sizeof(item->value.anybounds.first));
			appendBinaryStringInfo(buf,
								   (char *) &item->value.anybounds.last,
								   sizeof(item->value.anybounds.last));
			break;
		case jpiType:
		case jpiSize:
		case jpiAbs:
		case jpiFloor:
		case jpiCeiling:
		case jpiDouble:
		case jpiKeyValue:
			break;
		case jpiSequence:
			{
				int32		nelems = list_length(item->value.sequence.elems);
				ListCell   *lc;
				int			offset;

				checkJsonPathExtensionsEnabled(cxt, item->type);

				appendBinaryStringInfo(buf, (char *) &nelems, sizeof(nelems));

				offset = buf->len;

				appendStringInfoSpaces(buf, sizeof(int32) * nelems);

				foreach(lc, item->value.sequence.elems)
				{
					int32		elempos =
						flattenJsonPathParseItem(cxt, lfirst(lc), nestingLevel,
												 insideArraySubscript);

					*(int32 *) &buf->data[offset] = elempos - pos;
					offset += sizeof(int32);
				}
			}
			break;
		case jpiObject:
			{
				int32		nfields = list_length(item->value.object.fields);
				ListCell   *lc;
				int			offset;

				checkJsonPathExtensionsEnabled(cxt, item->type);

				appendBinaryStringInfo(buf, (char *) &nfields, sizeof(nfields));

				offset = buf->len;

				appendStringInfoSpaces(buf, sizeof(int32) * 2 * nfields);

				foreach(lc, item->value.object.fields)
				{
					JsonPathParseItem *field = lfirst(lc);
					int32		keypos =
						flattenJsonPathParseItem(cxt, field->value.args.left,
												 nestingLevel,
												 insideArraySubscript);
					int32		valpos =
						flattenJsonPathParseItem(cxt, field->value.args.right,
												 nestingLevel,
												 insideArraySubscript);
					int32	   *ppos = (int32 *) &buf->data[offset];

					ppos[0] = keypos - pos;
					ppos[1] = valpos - pos;

					offset += 2 * sizeof(int32);
				}
			}
			break;
		default:
			elog(ERROR, "unrecognized jsonpath item type: %d", item->type);
	}

	if (item->next)
	{
		chld = flattenJsonPathParseItem(cxt, item->next, nestingLevel,
										insideArraySubscript) - pos;
		*(int32 *) (buf->data + next) = chld;
	}

	return pos;
}

/*
 * Align StringInfo to int by adding zero padding bytes
 */
static void
alignStringInfoInt(StringInfo buf)
{
	switch (INTALIGN(buf->len) - buf->len)
	{
		case 3:
			appendStringInfoCharMacro(buf, 0);
			/* FALLTHROUGH */
		case 2:
			appendStringInfoCharMacro(buf, 0);
			/* FALLTHROUGH */
		case 1:
			appendStringInfoCharMacro(buf, 0);
			/* FALLTHROUGH */
		default:
			break;
	}
}

/*
 * Reserve space for int32 JsonPathItem pointer.  Now zero pointer is written,
 * actual value will be recorded at '(int32 *) &buf->data[pos]' later.
 */
static int32
reserveSpaceForItemPointer(StringInfo buf)
{
	int32		pos = buf->len;
	int32		ptr = 0;

	appendBinaryStringInfo(buf, (char *) &ptr, sizeof(ptr));

	return pos;
}

/*
 * Prints text representation of given jsonpath item and all its children.
 */
static void
printJsonPathItem(StringInfo buf, JsonPathItem *v, bool inKey,
				  bool printBracketes)
{
	JsonPathItem elem;
	int			i;

	check_stack_depth();
	CHECK_FOR_INTERRUPTS();

	switch (v->type)
	{
		case jpiNull:
			appendStringInfoString(buf, "null");
			break;
		case jpiKey:
			if (inKey)
				appendStringInfoChar(buf, '.');
			escape_json(buf, jspGetString(v, NULL));
			break;
		case jpiString:
			escape_json(buf, jspGetString(v, NULL));
			break;
		case jpiVariable:
			appendStringInfoChar(buf, '$');
			escape_json(buf, jspGetString(v, NULL));
			break;
		case jpiNumeric:
			if (jspHasNext(v))
				appendStringInfoChar(buf, '(');
			appendStringInfoString(buf,
								   DatumGetCString(DirectFunctionCall1(numeric_out,
																	   NumericGetDatum(jspGetNumeric(v)))));
			if (jspHasNext(v))
				appendStringInfoChar(buf, ')');
			break;
		case jpiBool:
			if (jspGetBool(v))
				appendBinaryStringInfo(buf, "true", 4);
			else
				appendBinaryStringInfo(buf, "false", 5);
			break;
		case jpiAnd:
		case jpiOr:
		case jpiEqual:
		case jpiNotEqual:
		case jpiLess:
		case jpiGreater:
		case jpiLessOrEqual:
		case jpiGreaterOrEqual:
		case jpiAdd:
		case jpiSub:
		case jpiMul:
		case jpiDiv:
		case jpiMod:
		case jpiStartsWith:
			if (printBracketes)
				appendStringInfoChar(buf, '(');
			jspGetLeftArg(v, &elem);
			printJsonPathItem(buf, &elem, false,
							  operationPriority(elem.type) <=
							  operationPriority(v->type));
			appendStringInfoChar(buf, ' ');
			appendStringInfoString(buf, jspOperationName(v->type));
			appendStringInfoChar(buf, ' ');
			jspGetRightArg(v, &elem);
			printJsonPathItem(buf, &elem, false,
							  operationPriority(elem.type) <=
							  operationPriority(v->type));
			if (printBracketes)
				appendStringInfoChar(buf, ')');
			break;
		case jpiLikeRegex:
			if (printBracketes)
				appendStringInfoChar(buf, '(');

			jspInitByBuffer(&elem, v->base, v->content.like_regex.expr);
			printJsonPathItem(buf, &elem, false,
							  operationPriority(elem.type) <=
							  operationPriority(v->type));

			appendBinaryStringInfo(buf, " like_regex ", 12);

			escape_json(buf, v->content.like_regex.pattern);

			if (v->content.like_regex.flags)
			{
				appendBinaryStringInfo(buf, " flag \"", 7);

				if (v->content.like_regex.flags & JSP_REGEX_ICASE)
					appendStringInfoChar(buf, 'i');
				if (v->content.like_regex.flags & JSP_REGEX_DOTALL)
					appendStringInfoChar(buf, 's');
				if (v->content.like_regex.flags & JSP_REGEX_MLINE)
					appendStringInfoChar(buf, 'm');
				if (v->content.like_regex.flags & JSP_REGEX_WSPACE)
					appendStringInfoChar(buf, 'x');
				if (v->content.like_regex.flags & JSP_REGEX_QUOTE)
					appendStringInfoChar(buf, 'q');

				appendStringInfoChar(buf, '"');
			}

			if (printBracketes)
				appendStringInfoChar(buf, ')');
			break;
		case jpiPlus:
		case jpiMinus:
			if (printBracketes)
				appendStringInfoChar(buf, '(');
			appendStringInfoChar(buf, v->type == jpiPlus ? '+' : '-');
			jspGetArg(v, &elem);
			printJsonPathItem(buf, &elem, false,
							  operationPriority(elem.type) <=
							  operationPriority(v->type));
			if (printBracketes)
				appendStringInfoChar(buf, ')');
			break;
		case jpiFilter:
			appendBinaryStringInfo(buf, "?(", 2);
			jspGetArg(v, &elem);
			printJsonPathItem(buf, &elem, false, false);
			appendStringInfoChar(buf, ')');
			break;
		case jpiNot:
			appendBinaryStringInfo(buf, "!(", 2);
			jspGetArg(v, &elem);
			printJsonPathItem(buf, &elem, false, false);
			appendStringInfoChar(buf, ')');
			break;
		case jpiIsUnknown:
			appendStringInfoChar(buf, '(');
			jspGetArg(v, &elem);
			printJsonPathItem(buf, &elem, false, false);
			appendBinaryStringInfo(buf, ") is unknown", 12);
			break;
		case jpiExists:
			appendBinaryStringInfo(buf, "exists (", 8);
			jspGetArg(v, &elem);
			printJsonPathItem(buf, &elem, false, false);
			appendStringInfoChar(buf, ')');
			break;
		case jpiCurrent:
			Assert(!inKey);
			appendStringInfoChar(buf, '@');
			break;
		case jpiRoot:
			Assert(!inKey);
			appendStringInfoChar(buf, '$');
			break;
		case jpiLast:
			appendBinaryStringInfo(buf, "last", 4);
			break;
		case jpiAnyArray:
			appendBinaryStringInfo(buf, "[*]", 3);
			break;
		case jpiAnyKey:
			if (inKey)
				appendStringInfoChar(buf, '.');
			appendStringInfoChar(buf, '*');
			break;
		case jpiIndexArray:
			appendStringInfoChar(buf, '[');
			for (i = 0; i < v->content.array.nelems; i++)
			{
				JsonPathItem from;
				JsonPathItem to;
				bool		range = jspGetArraySubscript(v, &from, &to, i);

				if (i)
					appendStringInfoChar(buf, ',');

				printJsonPathItem(buf, &from, false, from.type == jpiSequence);

				if (range)
				{
					appendBinaryStringInfo(buf, " to ", 4);
					printJsonPathItem(buf, &to, false, to.type == jpiSequence);
				}
			}
			appendStringInfoChar(buf, ']');
			break;
		case jpiAny:
			if (inKey)
				appendStringInfoChar(buf, '.');

			if (v->content.anybounds.first == 0 &&
				v->content.anybounds.last == PG_UINT32_MAX)
				appendBinaryStringInfo(buf, "**", 2);
			else if (v->content.anybounds.first == v->content.anybounds.last)
			{
				if (v->content.anybounds.first == PG_UINT32_MAX)
					appendStringInfoString(buf, "**{last}");
				else
					appendStringInfo(buf, "**{%u}",
									 v->content.anybounds.first);
			}
			else if (v->content.anybounds.first == PG_UINT32_MAX)
				appendStringInfo(buf, "**{last to %u}",
								 v->content.anybounds.last);
			else if (v->content.anybounds.last == PG_UINT32_MAX)
				appendStringInfo(buf, "**{%u to last}",
								 v->content.anybounds.first);
			else
				appendStringInfo(buf, "**{%u to %u}",
								 v->content.anybounds.first,
								 v->content.anybounds.last);
			break;
		case jpiType:
			appendBinaryStringInfo(buf, ".type()", 7);
			break;
		case jpiSize:
			appendBinaryStringInfo(buf, ".size()", 7);
			break;
		case jpiAbs:
			appendBinaryStringInfo(buf, ".abs()", 6);
			break;
		case jpiFloor:
			appendBinaryStringInfo(buf, ".floor()", 8);
			break;
		case jpiCeiling:
			appendBinaryStringInfo(buf, ".ceiling()", 10);
			break;
		case jpiDouble:
			appendBinaryStringInfo(buf, ".double()", 9);
			break;
		case jpiDatetime:
			appendBinaryStringInfo(buf, ".datetime(", 10);
			if (v->content.arg)
			{
				jspGetArg(v, &elem);
				printJsonPathItem(buf, &elem, false, false);
			}
			appendStringInfoChar(buf, ')');
			break;
		case jpiKeyValue:
			appendBinaryStringInfo(buf, ".keyvalue()", 11);
			break;
		case jpiSequence:
			if (printBracketes || jspHasNext(v))
				appendStringInfoChar(buf, '(');

			for (i = 0; i < v->content.sequence.nelems; i++)
			{
				JsonPathItem elem;

				if (i)
					appendBinaryStringInfo(buf, ", ", 2);

				jspGetSequenceElement(v, i, &elem);

				printJsonPathItem(buf, &elem, false, elem.type == jpiSequence);
			}

			if (printBracketes || jspHasNext(v))
				appendStringInfoChar(buf, ')');
			break;
		case jpiArray:
			appendStringInfoChar(buf, '[');
			if (v->content.arg)
			{
				jspGetArg(v, &elem);
				printJsonPathItem(buf, &elem, false, false);
			}
			appendStringInfoChar(buf, ']');
			break;
		case jpiObject:
			appendStringInfoChar(buf, '{');

			for (i = 0; i < v->content.object.nfields; i++)
			{
				JsonPathItem key;
				JsonPathItem val;

				jspGetObjectField(v, i, &key, &val);

				if (i)
					appendBinaryStringInfo(buf, ", ", 2);

				printJsonPathItem(buf, &key, false, false);
				appendBinaryStringInfo(buf, ": ", 2);
				printJsonPathItem(buf, &val, false, val.type == jpiSequence);
			}

			appendStringInfoChar(buf, '}');
			break;
		default:
			elog(ERROR, "unrecognized jsonpath item type: %d", v->type);
	}

	if (jspGetNext(v, &elem))
		printJsonPathItem(buf, &elem, true, true);
}

const char *
jspOperationName(JsonPathItemType type)
{
	switch (type)
	{
		case jpiAnd:
			return "&&";
		case jpiOr:
			return "||";
		case jpiEqual:
			return "==";
		case jpiNotEqual:
			return "!=";
		case jpiLess:
			return "<";
		case jpiGreater:
			return ">";
		case jpiLessOrEqual:
			return "<=";
		case jpiGreaterOrEqual:
			return ">=";
		case jpiPlus:
		case jpiAdd:
			return "+";
		case jpiMinus:
		case jpiSub:
			return "-";
		case jpiMul:
			return "*";
		case jpiDiv:
			return "/";
		case jpiMod:
			return "%";
		case jpiStartsWith:
			return "starts with";
		case jpiLikeRegex:
			return "like_regex";
		case jpiType:
			return "type";
		case jpiSize:
			return "size";
		case jpiKeyValue:
			return "keyvalue";
		case jpiDouble:
			return "double";
		case jpiAbs:
			return "abs";
		case jpiFloor:
			return "floor";
		case jpiCeiling:
			return "ceiling";
		case jpiDatetime:
			return "datetime";
		default:
			elog(ERROR, "unrecognized jsonpath item type: %d", type);
			return NULL;
	}
}

static int
operationPriority(JsonPathItemType op)
{
	switch (op)
	{
		case jpiSequence:
			return -1;
		case jpiOr:
			return 0;
		case jpiAnd:
			return 1;
		case jpiEqual:
		case jpiNotEqual:
		case jpiLess:
		case jpiGreater:
		case jpiLessOrEqual:
		case jpiGreaterOrEqual:
		case jpiStartsWith:
			return 2;
		case jpiAdd:
		case jpiSub:
			return 3;
		case jpiMul:
		case jpiDiv:
		case jpiMod:
			return 4;
		case jpiPlus:
		case jpiMinus:
			return 5;
		default:
			return 6;
	}
}

/******************* Support functions for JsonPath *************************/

/*
 * Support macros to read stored values
 */

#define read_byte(v, b, p) do {			\
	(v) = *(uint8*)((b) + (p));			\
	(p) += 1;							\
} while(0)								\

#define read_int32(v, b, p) do {		\
	(v) = *(uint32*)((b) + (p));		\
	(p) += sizeof(int32);				\
} while(0)								\

#define read_int32_n(v, b, p, n) do {	\
	(v) = (void *)((b) + (p));			\
	(p) += sizeof(int32) * (n);			\
} while(0)								\

/*
 * Read root node and fill root node representation
 */
void
jspInit(JsonPathItem *v, JsonPath *js)
{
	Assert((js->header & JSONPATH_VERSION_MASK) == JSONPATH_VERSION);
	jspInitByBuffer(v, js->data, 0);
}

/*
 * Read node from buffer and fill its representation
 */
void
jspInitByBuffer(JsonPathItem *v, char *base, int32 pos)
{
	v->base = base + pos;

	read_byte(v->type, base, pos);
	pos = INTALIGN((uintptr_t) (base + pos)) - (uintptr_t) base;
	read_int32(v->nextPos, base, pos);

	switch (v->type)
	{
		case jpiNull:
		case jpiRoot:
		case jpiCurrent:
		case jpiAnyArray:
		case jpiAnyKey:
		case jpiType:
		case jpiSize:
		case jpiAbs:
		case jpiFloor:
		case jpiCeiling:
		case jpiDouble:
		case jpiKeyValue:
		case jpiLast:
			break;
		case jpiKey:
		case jpiString:
		case jpiVariable:
			read_int32(v->content.value.datalen, base, pos);
			/* FALLTHROUGH */
		case jpiNumeric:
		case jpiBool:
			v->content.value.data = base + pos;
			break;
		case jpiAnd:
		case jpiOr:
		case jpiAdd:
		case jpiSub:
		case jpiMul:
		case jpiDiv:
		case jpiMod:
		case jpiEqual:
		case jpiNotEqual:
		case jpiLess:
		case jpiGreater:
		case jpiLessOrEqual:
		case jpiGreaterOrEqual:
		case jpiStartsWith:
			read_int32(v->content.args.left, base, pos);
			read_int32(v->content.args.right, base, pos);
			break;
		case jpiLikeRegex:
			read_int32(v->content.like_regex.flags, base, pos);
			read_int32(v->content.like_regex.expr, base, pos);
			read_int32(v->content.like_regex.patternlen, base, pos);
			v->content.like_regex.pattern = base + pos;
			break;
		case jpiNot:
		case jpiExists:
		case jpiIsUnknown:
		case jpiPlus:
		case jpiMinus:
		case jpiFilter:
		case jpiDatetime:
		case jpiArray:
			read_int32(v->content.arg, base, pos);
			break;
		case jpiIndexArray:
			read_int32(v->content.array.nelems, base, pos);
			read_int32_n(v->content.array.elems, base, pos,
						 v->content.array.nelems * 2);
			break;
		case jpiAny:
			read_int32(v->content.anybounds.first, base, pos);
			read_int32(v->content.anybounds.last, base, pos);
			break;
		case jpiSequence:
			read_int32(v->content.sequence.nelems, base, pos);
			read_int32_n(v->content.sequence.elems, base, pos,
						 v->content.sequence.nelems);
			break;
		case jpiObject:
			read_int32(v->content.object.nfields, base, pos);
			read_int32_n(v->content.object.fields, base, pos,
						 v->content.object.nfields * 2);
			break;
		default:
			elog(ERROR, "unrecognized jsonpath item type: %d", v->type);
	}
}

void
jspGetArg(JsonPathItem *v, JsonPathItem *a)
{
	Assert(v->type == jpiFilter ||
		   v->type == jpiNot ||
		   v->type == jpiIsUnknown ||
		   v->type == jpiExists ||
		   v->type == jpiPlus ||
		   v->type == jpiMinus ||
		   v->type == jpiDatetime ||
		   v->type == jpiArray);

	jspInitByBuffer(a, v->base, v->content.arg);
}

bool
jspGetNext(JsonPathItem *v, JsonPathItem *a)
{
	if (jspHasNext(v))
	{
		Assert(v->type == jpiString ||
			   v->type == jpiNumeric ||
			   v->type == jpiBool ||
			   v->type == jpiNull ||
			   v->type == jpiKey ||
			   v->type == jpiAny ||
			   v->type == jpiAnyArray ||
			   v->type == jpiAnyKey ||
			   v->type == jpiIndexArray ||
			   v->type == jpiFilter ||
			   v->type == jpiCurrent ||
			   v->type == jpiExists ||
			   v->type == jpiRoot ||
			   v->type == jpiVariable ||
			   v->type == jpiLast ||
			   v->type == jpiAdd ||
			   v->type == jpiSub ||
			   v->type == jpiMul ||
			   v->type == jpiDiv ||
			   v->type == jpiMod ||
			   v->type == jpiPlus ||
			   v->type == jpiMinus ||
			   v->type == jpiEqual ||
			   v->type == jpiNotEqual ||
			   v->type == jpiGreater ||
			   v->type == jpiGreaterOrEqual ||
			   v->type == jpiLess ||
			   v->type == jpiLessOrEqual ||
			   v->type == jpiAnd ||
			   v->type == jpiOr ||
			   v->type == jpiNot ||
			   v->type == jpiIsUnknown ||
			   v->type == jpiType ||
			   v->type == jpiSize ||
			   v->type == jpiAbs ||
			   v->type == jpiFloor ||
			   v->type == jpiCeiling ||
			   v->type == jpiDouble ||
			   v->type == jpiDatetime ||
			   v->type == jpiKeyValue ||
			   v->type == jpiStartsWith ||
			   v->type == jpiSequence ||
			   v->type == jpiArray ||
			   v->type == jpiObject);

		if (a)
			jspInitByBuffer(a, v->base, v->nextPos);
		return true;
	}

	return false;
}

void
jspGetLeftArg(JsonPathItem *v, JsonPathItem *a)
{
	Assert(v->type == jpiAnd ||
		   v->type == jpiOr ||
		   v->type == jpiEqual ||
		   v->type == jpiNotEqual ||
		   v->type == jpiLess ||
		   v->type == jpiGreater ||
		   v->type == jpiLessOrEqual ||
		   v->type == jpiGreaterOrEqual ||
		   v->type == jpiAdd ||
		   v->type == jpiSub ||
		   v->type == jpiMul ||
		   v->type == jpiDiv ||
		   v->type == jpiMod ||
		   v->type == jpiStartsWith);

	jspInitByBuffer(a, v->base, v->content.args.left);
}

void
jspGetRightArg(JsonPathItem *v, JsonPathItem *a)
{
	Assert(v->type == jpiAnd ||
		   v->type == jpiOr ||
		   v->type == jpiEqual ||
		   v->type == jpiNotEqual ||
		   v->type == jpiLess ||
		   v->type == jpiGreater ||
		   v->type == jpiLessOrEqual ||
		   v->type == jpiGreaterOrEqual ||
		   v->type == jpiAdd ||
		   v->type == jpiSub ||
		   v->type == jpiMul ||
		   v->type == jpiDiv ||
		   v->type == jpiMod ||
		   v->type == jpiStartsWith);

	jspInitByBuffer(a, v->base, v->content.args.right);
}

bool
jspGetBool(JsonPathItem *v)
{
	Assert(v->type == jpiBool);

	return (bool) *v->content.value.data;
}

Numeric
jspGetNumeric(JsonPathItem *v)
{
	Assert(v->type == jpiNumeric);

	return (Numeric) v->content.value.data;
}

char *
jspGetString(JsonPathItem *v, int32 *len)
{
	Assert(v->type == jpiKey ||
		   v->type == jpiString ||
		   v->type == jpiVariable);

	if (len)
		*len = v->content.value.datalen;
	return v->content.value.data;
}

bool
jspGetArraySubscript(JsonPathItem *v, JsonPathItem *from, JsonPathItem *to,
					 int i)
{
	Assert(v->type == jpiIndexArray);

	jspInitByBuffer(from, v->base, v->content.array.elems[i].from);

	if (!v->content.array.elems[i].to)
		return false;

	jspInitByBuffer(to, v->base, v->content.array.elems[i].to);

	return true;
}

void
jspGetSequenceElement(JsonPathItem *v, int i, JsonPathItem *elem)
{
	Assert(v->type == jpiSequence);

	jspInitByBuffer(elem, v->base, v->content.sequence.elems[i]);
}

void
jspGetObjectField(JsonPathItem *v, int i, JsonPathItem *key, JsonPathItem *val)
{
	Assert(v->type == jpiObject);
	jspInitByBuffer(key, v->base, v->content.object.fields[i].key);
	jspInitByBuffer(val, v->base, v->content.object.fields[i].val);
}

/* SQL/JSON datatype status: */
typedef enum JsonPathDatatypeStatus
{
	jpdsNonDateTime,			/* null, bool, numeric, string, array, object */
	jpdsUnknownDateTime,		/* unknown datetime type */
	jpdsDateTimeZoned,			/* timetz, timestamptz */
	jpdsDateTimeNonZoned		/* time, timestamp, date */
} JsonPathDatatypeStatus;

/* Context for jspIsMutableWalker() */
typedef struct JsonPathMutableContext
{
	List	   *varnames;		/* list of variable names */
	List	   *varexprs;		/* list of variable expressions */
	JsonPathDatatypeStatus current; /* status of @ item */
	bool		lax;			/* jsonpath is lax or strict */
	bool		mutable;		/* resulting mutability status */
} JsonPathMutableContext;

/*
 * Recursive walker for jspIsMutable()
 */
static JsonPathDatatypeStatus
jspIsMutableWalker(JsonPathItem *jpi, JsonPathMutableContext *cxt)
{
	JsonPathItem next;
	JsonPathDatatypeStatus status = jpdsNonDateTime;

	while (!cxt->mutable)
	{
		JsonPathItem arg;
		JsonPathDatatypeStatus leftStatus;
		JsonPathDatatypeStatus rightStatus;

		switch (jpi->type)
		{
			case jpiRoot:
				Assert(status == jpdsNonDateTime);
				break;

			case jpiCurrent:
				Assert(status == jpdsNonDateTime);
				status = cxt->current;
				break;

			case jpiFilter:
				{
					JsonPathDatatypeStatus prevStatus = cxt->current;

					cxt->current = status;
					jspGetArg(jpi, &arg);
					jspIsMutableWalker(&arg, cxt);

					cxt->current = prevStatus;
					break;
				}

			case jpiVariable:
				{
					int32		len;
					const char *name = jspGetString(jpi, &len);
					ListCell   *lc1;
					ListCell   *lc2;

					Assert(status == jpdsNonDateTime);

					forboth(lc1, cxt->varnames, lc2, cxt->varexprs)
					{
						String	   *varname = lfirst_node(String, lc1);
						Node	   *varexpr = lfirst(lc2);

						if (strncmp(varname->sval, name, len))
							continue;

						switch (exprType(varexpr))
						{
							case DATEOID:
							case TIMEOID:
							case TIMESTAMPOID:
								status = jpdsDateTimeNonZoned;
								break;

							case TIMETZOID:
							case TIMESTAMPTZOID:
								status = jpdsDateTimeZoned;
								break;

							default:
								status = jpdsNonDateTime;
								break;
						}

						break;
					}
					break;
				}

			case jpiEqual:
			case jpiNotEqual:
			case jpiLess:
			case jpiGreater:
			case jpiLessOrEqual:
			case jpiGreaterOrEqual:
				Assert(status == jpdsNonDateTime);
				jspGetLeftArg(jpi, &arg);
				leftStatus = jspIsMutableWalker(&arg, cxt);

				jspGetRightArg(jpi, &arg);
				rightStatus = jspIsMutableWalker(&arg, cxt);

				/*
				 * Comparison of datetime type with different timezone status
				 * is mutable.
				 */
				if (leftStatus != jpdsNonDateTime &&
					rightStatus != jpdsNonDateTime &&
					(leftStatus == jpdsUnknownDateTime ||
					 rightStatus == jpdsUnknownDateTime ||
					 leftStatus != rightStatus))
					cxt->mutable = true;
				break;

			case jpiNot:
			case jpiIsUnknown:
			case jpiExists:
			case jpiPlus:
			case jpiMinus:
				Assert(status == jpdsNonDateTime);
				jspGetArg(jpi, &arg);
				jspIsMutableWalker(&arg, cxt);
				break;

			case jpiAnd:
			case jpiOr:
			case jpiAdd:
			case jpiSub:
			case jpiMul:
			case jpiDiv:
			case jpiMod:
			case jpiStartsWith:
				Assert(status == jpdsNonDateTime);
				jspGetLeftArg(jpi, &arg);
				jspIsMutableWalker(&arg, cxt);
				jspGetRightArg(jpi, &arg);
				jspIsMutableWalker(&arg, cxt);
				break;

			case jpiIndexArray:
				for (int i = 0; i < jpi->content.array.nelems; i++)
				{
					JsonPathItem from;
					JsonPathItem to;

					if (jspGetArraySubscript(jpi, &from, &to, i))
						jspIsMutableWalker(&to, cxt);

					jspIsMutableWalker(&from, cxt);
				}
				/* FALLTHROUGH */

			case jpiAnyArray:
				if (!cxt->lax)
					status = jpdsNonDateTime;
				break;

			case jpiAny:
				if (jpi->content.anybounds.first > 0)
					status = jpdsNonDateTime;
				break;

			case jpiDatetime:
				if (jpi->content.arg)
				{
					char	   *template;
					int			flags;

					jspGetArg(jpi, &arg);
					if (arg.type != jpiString)
					{
						status = jpdsNonDateTime;
						break;	/* there will be runtime error */
					}

					template = jspGetString(&arg, NULL);
					flags = datetime_format_flags(template, NULL);
					if (flags & DCH_ZONED)
						status = jpdsDateTimeZoned;
					else
						status = jpdsDateTimeNonZoned;
				}
				else
				{
					status = jpdsUnknownDateTime;
				}
				break;

			case jpiLikeRegex:
				Assert(status == jpdsNonDateTime);
				jspInitByBuffer(&arg, jpi->base, jpi->content.like_regex.expr);
				jspIsMutableWalker(&arg, cxt);
				break;

				/* literals */
			case jpiNull:
			case jpiString:
			case jpiNumeric:
			case jpiBool:
				/* accessors */
			case jpiKey:
			case jpiAnyKey:
				/* special items */
			case jpiSubscript:
			case jpiLast:
				/* item methods */
			case jpiType:
			case jpiSize:
			case jpiAbs:
			case jpiFloor:
			case jpiCeiling:
			case jpiDouble:
			case jpiKeyValue:
				status = jpdsNonDateTime;
				break;
		}

		if (!jspGetNext(jpi, &next))
			break;

		jpi = &next;
	}

	return status;
}

/*
 * Check whether jsonpath expression is immutable or not.
 */
bool
jspIsMutable(JsonPath *path, List *varnames, List *varexprs)
{
	JsonPathMutableContext cxt;
	JsonPathItem jpi;

	cxt.varnames = varnames;
	cxt.varexprs = varexprs;
	cxt.current = jpdsNonDateTime;
	cxt.lax = (path->header & JSONPATH_LAX) != 0;
	cxt.mutable = false;

	jspInit(&jpi, path);
	jspIsMutableWalker(&jpi, &cxt);

	return cxt.mutable;
}
