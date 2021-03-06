/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "pxf_filter.h"
#include "pxf_header.h"

#if PG_VERSION_NUM >= 90600
#include "access/external.h"
#include "access/url.h"
#include "common/md5.h"
#else
#include "access/fileam.h"
#include "catalog/pg_exttable.h"
#include "libpq/md5.h"
#endif
#include "cdb/cdbvars.h"
#include "commands/defrem.h"
#include "catalog/pg_namespace.h"
#include "utils/builtins.h"
#include "utils/formatting.h"
#include "utils/timestamp.h"
#include "utils/syscache.h"

/* helper function declarations */
static void AddAlignmentSizeHttpHeader(CHURL_HEADERS headers);
static void AddTupleDescriptionToHttpHeader(CHURL_HEADERS headers, Relation rel);
static void AddOptionsToHttpHeader(CHURL_HEADERS headers, List *options);
static void AddProjectionDescHttpHeader(CHURL_HEADERS headers, List *retrieved_attrs);
static void AddProjectionIndexHeader(CHURL_HEADERS headers, int attno, char *long_number);
static char *NormalizeKeyName(const char *key);
static char *TypeOidGetTypename(Oid typid);
static char *GetTraceId(char* xid, char* filter, char* relnamespace, const char* relname, char* user);
static char *GetSpanId(char* traceId, char* segmentId);
static char *GetNamespaceName(Oid nsp_oid);

/*
 * Add key/value pairs to connection header.
 * These values are the context of the query and used
 * by the remote component.
 */
void
BuildHttpHeaders(CHURL_HEADERS headers,
				 PxfOptions *options,
				 Relation relation,
				 char *filter_string,
				 List *retrieved_attrs)
{
	extvar_t	 ev;
	char		 pxfPortString[sizeof(int32) * 8];
	char		 long_number[sizeof(int32) * 8];
	char		*traceId;
	const char	*relname = NULL;
	char		*relnamespace = NULL;

	if (relation != NULL)
	{
		/* Record fields - name and type of each field */
		AddTupleDescriptionToHttpHeader(headers, relation);
		relname = RelationGetRelationName(relation);
		relnamespace = GetNamespaceName(RelationGetNamespace(relation));
	}

	if (retrieved_attrs != NULL)
	{
		/* add the list of attrs to the projection desc http headers */
		AddProjectionDescHttpHeader(headers, retrieved_attrs);
	}

	/* GP cluster configuration */
	external_set_env_vars(&ev, "pxf_fdw", false, NULL, NULL, false, 0);

	/*
	 * make sure that user identity is known and set, otherwise impersonation
	 * by PXF will be impossible
	 */
	if (!ev.GP_USER || !ev.GP_USER[0])
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("user identity is unknown")));
	churl_headers_append(headers, "X-GP-ENCODED-HEADER-VALUES", "true");
	churl_headers_append(headers, "X-GP-USER", ev.GP_USER);

	churl_headers_append(headers, "X-GP-SEGMENT-ID", ev.GP_SEGMENT_ID);
	churl_headers_append(headers, "X-GP-SEGMENT-COUNT", ev.GP_SEGMENT_COUNT);
	churl_headers_append(headers, "X-GP-XID", ev.GP_XID);

	pg_ltoa(gp_session_id, long_number);
	churl_headers_append(headers, "X-GP-SESSION-ID", long_number);
	pg_ltoa(gp_command_count, long_number);
	churl_headers_append(headers, "X-GP-COMMAND-COUNT", long_number);

	AddAlignmentSizeHttpHeader(headers);

	/* Convert the number of attributes to a string */
	pg_ltoa(options->pxf_port, pxfPortString);

	/* headers for uri data */
	churl_headers_append(headers, "X-GP-URL-HOST", options->pxf_host);
	churl_headers_append(headers, "X-GP-URL-PORT", pxfPortString);

	churl_headers_append(headers, "X-GP-OPTIONS-PROFILE", options->profile);
	/* only text format is supported for FDW */
	churl_headers_append(headers, "X-GP-FORMAT", "TEXT");
	churl_headers_append(headers, "X-GP-DATA-DIR", options->resource);
	churl_headers_append(headers, "X-GP-OPTIONS-SERVER", options->server);
	churl_headers_append(headers, "X-GP-TABLE-NAME", relname);
	churl_headers_append(headers, "X-GP-SCHEMA-NAME", relnamespace);

	/* encoding options */
	churl_headers_append(headers, "X-GP-DATA-ENCODING", options->data_encoding);
	churl_headers_append(headers, "X-GP-DATABASE-ENCODING", options->database_encoding);

	/* extra options */
	AddOptionsToHttpHeader(headers, options->options);

	/* copy options */
	AddOptionsToHttpHeader(headers, options->copy_options);

	/* filters */
	if (filter_string && strcmp(filter_string, "") != 0)
	{
		churl_headers_append(headers, "X-GP-FILTER", filter_string);
		churl_headers_append(headers, "X-GP-HAS-FILTER", "1");
	}
	else
		churl_headers_append(headers, "X-GP-HAS-FILTER", "0");

	// Add trace id = xid : filterstr : schema : tablename : user
	traceId = GetTraceId(ev.GP_XID, filter_string, relnamespace, relname, ev.GP_USER);
	churl_headers_append(headers, "X-B3-TraceId", traceId);

	// Add span id = traceId : segId
	churl_headers_append(headers, "X-B3-SpanId", GetSpanId(traceId, ev.GP_SEGMENT_ID));

	churl_headers_override(headers, "Connection", "close");
}

/* Report alignment size to remote component
 * GPDBWritable uses alignment that has to be the same as
 * in the C code.
 * Since the C code can be compiled for both 32 and 64 bits,
 * the alignment can be either 4 or 8.
 */
static void
AddAlignmentSizeHttpHeader(CHURL_HEADERS headers)
{
	char		tmp[sizeof(char *)];

	pg_ltoa(sizeof(char *), tmp);
	churl_headers_append(headers, "X-GP-ALIGNMENT", tmp);
}

/*
 * Report tuple description to remote component
 * Currently, number of attributes, attributes names, types and types modifiers
 * Each attribute has a pair of key/value
 * where X is the number of the attribute
 * X-GP-ATTR-NAMEX - attribute X's name
 * X-GP-ATTR-TYPECODEX - attribute X's type OID (e.g, 16)
 * X-GP-ATTR-TYPENAMEX - attribute X's type name (e.g, "boolean")
 * optional - X-GP-ATTR-TYPEMODX-COUNT - total number of modifier for attribute X
 * optional - X-GP-ATTR-TYPEMODX-Y - attribute X's modifiers Y (types which have precision info, like numeric(p,s))
 */
static void
AddTupleDescriptionToHttpHeader(CHURL_HEADERS headers, Relation rel)
{
	char		long_number[sizeof(int32) * 8];
	StringInfoData formatter;
	TupleDesc	tuple;

	initStringInfo(&formatter);

	/* Get tuple description itself */
	tuple = RelationGetDescr(rel);

	/* Convert the number of attributes to a string */
	pg_ltoa(tuple->natts, long_number);
	churl_headers_append(headers, "X-GP-ATTRS", long_number);

	/* Iterate attributes */
	for (int i = 0; i < tuple->natts; ++i)
	{
		Form_pg_attribute attr = TupleDescAttr(tuple, i);

		/* Add a key/value pair for attribute name */
		resetStringInfo(&formatter);
		appendStringInfo(&formatter, "X-GP-ATTR-NAME%u", i);
		churl_headers_append(headers, formatter.data, attr->attname.data);

		/* Add a key/value pair for attribute type */
		resetStringInfo(&formatter);
		appendStringInfo(&formatter, "X-GP-ATTR-TYPECODE%u", i);
		pg_ltoa(attr->atttypid, long_number);
		churl_headers_append(headers, formatter.data, long_number);

		/* Add a key/value pair for attribute type name */
		resetStringInfo(&formatter);
		appendStringInfo(&formatter, "X-GP-ATTR-TYPENAME%u", i);
		churl_headers_append(headers, formatter.data, TypeOidGetTypename(attr->atttypid));

		/* Add attribute type modifiers if any */
		if (attr->atttypmod > -1)
		{
			switch (attr->atttypid)
			{
				case NUMERICOID:
					{
						resetStringInfo(&formatter);
						appendStringInfo(&formatter, "X-GP-ATTR-TYPEMOD%u-COUNT", i);
						pg_ltoa(2, long_number);
						churl_headers_append(headers, formatter.data, long_number);


						/* precision */
						resetStringInfo(&formatter);
						appendStringInfo(&formatter, "X-GP-ATTR-TYPEMOD%u-%u", i, 0);
						pg_ltoa((attr->atttypmod >> 16) & 0xffff, long_number);
						churl_headers_append(headers, formatter.data, long_number);

						/* scale */
						resetStringInfo(&formatter);
						appendStringInfo(&formatter, "X-GP-ATTR-TYPEMOD%u-%u", i, 1);
						pg_ltoa((attr->atttypmod - VARHDRSZ) & 0xffff, long_number);
						churl_headers_append(headers, formatter.data, long_number);
						break;
					}
				case CHAROID:
				case BPCHAROID:
				case VARCHAROID:
					{
						resetStringInfo(&formatter);
						appendStringInfo(&formatter, "X-GP-ATTR-TYPEMOD%u-COUNT", i);
						pg_ltoa(1, long_number);
						churl_headers_append(headers, formatter.data, long_number);

						resetStringInfo(&formatter);
						appendStringInfo(&formatter, "X-GP-ATTR-TYPEMOD%u-%u", i, 0);
						pg_ltoa((attr->atttypmod - VARHDRSZ), long_number);
						churl_headers_append(headers, formatter.data, long_number);
						break;
					}
				case VARBITOID:
				case BITOID:
				case TIMESTAMPOID:
				case TIMESTAMPTZOID:
				case TIMEOID:
				case TIMETZOID:
					{
						resetStringInfo(&formatter);
						appendStringInfo(&formatter, "X-GP-ATTR-TYPEMOD%u-COUNT", i);
						pg_ltoa(1, long_number);
						churl_headers_append(headers, formatter.data, long_number);

						resetStringInfo(&formatter);
						appendStringInfo(&formatter, "X-GP-ATTR-TYPEMOD%u-%u", i, 0);
						pg_ltoa((attr->atttypmod), long_number);
						churl_headers_append(headers, formatter.data, long_number);
						break;
					}
				case INTERVALOID:
					{
						resetStringInfo(&formatter);
						appendStringInfo(&formatter, "X-GP-ATTR-TYPEMOD%u-COUNT", i);
						pg_ltoa(1, long_number);
						churl_headers_append(headers, formatter.data, long_number);

						resetStringInfo(&formatter);
						appendStringInfo(&formatter, "X-GP-ATTR-TYPEMOD%u-%u", i, 0);
						pg_ltoa(INTERVAL_PRECISION(attr->atttypmod), long_number);
						churl_headers_append(headers, formatter.data, long_number);
						break;
					}
				default:
					elog(DEBUG5, "addTupleDescriptionToHttpHeader: unsupported type %d ", attr->atttypid);
					break;
			}
		}
	}

	pfree(formatter.data);
}

/*
 * Report projection description to the remote component
 */
static void
AddProjectionDescHttpHeader(CHURL_HEADERS headers, List *retrieved_attrs)
{
	ListCell   *lc1 = NULL;
	char		long_number[sizeof(int32) * 8];

	foreach(lc1, retrieved_attrs)
	{
		int			attno = lfirst_int(lc1);

		/* zero-based index in the server side */
		AddProjectionIndexHeader(headers, attno - 1, long_number);
	}

	if (retrieved_attrs->length == 0)
		return;

	/* Convert the number of projection columns to a string */
	pg_ltoa(retrieved_attrs->length, long_number);
	churl_headers_append(headers, "X-GP-ATTRS-PROJ", long_number);
}

/*
 * Adds the projection index header for the given attno
 */
static void
AddProjectionIndexHeader(CHURL_HEADERS headers,
						 int attno,
						 char *long_number)
{
	pg_ltoa(attno, long_number);
	churl_headers_append(headers, "X-GP-ATTRS-PROJ-IDX", long_number);
}

/*
 * Add all the FDW options in the list to the curl headers
 */
static void
AddOptionsToHttpHeader(CHURL_HEADERS headers, List *options)
{
	ListCell   *cell;

	foreach(cell, options)
	{
		DefElem    *def = (DefElem *) lfirst(cell);
		char	   *x_gp_key = NormalizeKeyName(def->defname);

		churl_headers_append(headers, x_gp_key, defGetString(def));
		pfree(x_gp_key);
	}
}

/*
 * Full name of the HEADER KEY expected by the PXF service
 * Converts input string to upper case and prepends "X-GP-OPTIONS-" string
 * This will be used for all user defined parameters to be isolate from internal parameters
 */
static char *
NormalizeKeyName(const char *key)
{
	if (!key || strlen(key) == 0)
		elog(ERROR, "internal error in pxfutils.c:normalize_key_name, parameter key is null or empty");

	return psprintf("X-GP-OPTIONS-%s", asc_toupper(pstrdup(key), strlen(key)));
}

/*
 * TypeOidGetTypename
 * Get the name of the type, given the OID
 */
static char *
TypeOidGetTypename(Oid typid)
{

	Assert(OidIsValid(typid));

	HeapTuple	typtup = SearchSysCache(TYPEOID,
										ObjectIdGetDatum(typid),
										0, 0, 0);

	if (!HeapTupleIsValid(typtup))
		elog(ERROR, "cache lookup failed for type %u", typid);

	Form_pg_type typform = (Form_pg_type) GETSTRUCT(typtup);
	char	   *typname = psprintf("%s", NameStr(typform->typname));

	ReleaseSysCache(typtup);

	return typname;
}

/* Returns the 128-bit trace id to be propagated
 * to the PXF Service
 */
static char *
GetTraceId(char* xid, char* filter, char* relnamespace, const char* relname, char* user)
{
	char	   *traceId,
			*md5Hash;

	traceId = psprintf("%s:%s:%s:%s:%s", xid, filter, relnamespace, relname, user);
	elog(DEBUG3, "GetTraceId: generated traceId %s", traceId);

	md5Hash = palloc0(33);

	if (!pg_md5_hash(traceId, strlen(traceId), md5Hash))
	{
		elog(DEBUG3, "GetTraceId: Unable to calculate pg_md5_hash for traceId '%s'", traceId);
		return NULL;
	}

	elog(DEBUG3, "GetTraceId: generated md5 hash for traceId %s", md5Hash);

	return md5Hash;
}

/* Returns the 64-bit span id to be propagated
 * to the PXF Service
 */
static char *
GetSpanId(char* traceId, char* segmentId)
{
	char	   *spanId,
			*md5Hash,
			*res;

	spanId = psprintf("%s:%s", traceId, segmentId);
	elog(DEBUG3, "GetSpanId: generated spanId %s", spanId);

	md5Hash = palloc0(33);
	res = palloc0(17);

	if (!pg_md5_hash(spanId, strlen(spanId), md5Hash))
	{
		elog(DEBUG3, "GetSpanId: Unable to calculate pg_md5_hash for spanId '%s'", spanId);
		return NULL;
	}

	strncpy(res, md5Hash, 16);
	elog(DEBUG3, "GetSpanId: generated md5 hash for spanId %s", res);
	pfree(md5Hash);

	return res;
}

/* Returns the namespace (schema) name for a given namespace oid */
static char *
GetNamespaceName(Oid nsp_oid)
{
	HeapTuple	tuple;
	Datum		nspnameDatum;
	bool		isNull;

	tuple = SearchSysCache1(NAMESPACEOID, ObjectIdGetDatum(nsp_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
						errmsg("schema with OID %u does not exist", nsp_oid)));

	nspnameDatum = SysCacheGetAttr(NAMESPACEOID, tuple, Anum_pg_namespace_nspname,
								   &isNull);

	ReleaseSysCache(tuple);

	return DatumGetCString(nspnameDatum);
}
