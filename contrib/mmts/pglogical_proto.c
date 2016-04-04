#include "postgres.h"

#include "miscadmin.h"

#include "pglogical_output.h"
#include "replication/origin.h"

#include "access/sysattr.h"
#include "access/tuptoaster.h"
#include "access/xact.h"
#include "access/clog.h"

#include "catalog/catversion.h"
#include "catalog/index.h"

#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"

#include "commands/dbcommands.h"

#include "executor/spi.h"

#include "libpq/pqformat.h"

#include "mb/pg_wchar.h"

#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"

#include "multimaster.h"

static bool MtmIsFilteredTxn;

static void pglogical_write_rel(StringInfo out, PGLogicalOutputData *data, Relation rel, struct PGLRelMetaCacheEntry *cache_entry));

static void pglogical_write_begin(StringInfo out, PGLogicalOutputData *data,
							ReorderBufferTXN *txn);
static void pglogical_write_commit(StringInfo out,PGLogicalOutputData *data,
							ReorderBufferTXN *txn, XLogRecPtr commit_lsn);

static void pglogical_write_insert(StringInfo out, PGLogicalOutputData *data,
							Relation rel, HeapTuple newtuple);
static void pglogical_write_update(StringInfo out, PGLogicalOutputData *data,
							Relation rel, HeapTuple oldtuple,
							HeapTuple newtuple);
static void pglogical_write_delete(StringInfo out, PGLogicalOutputData *data,
							Relation rel, HeapTuple oldtuple);

static void pglogical_write_attrs(StringInfo out, Relation rel);
static void pglogical_write_tuple(StringInfo out, PGLogicalOutputData *data,
								   Relation rel, HeapTuple tuple);
static char decide_datum_transfer(Form_pg_attribute att,
								  Form_pg_type typclass,
								  bool allow_internal_basetypes,
								  bool allow_binary_basetypes);

/*
 * Write relation description to the output stream.
 */
static void
pglogical_write_rel(StringInfo out, PGLogicalOutputData *data, Relation rel,
					struct PGLRelMetaCacheEntry *cache_entry)
{
	const char *nspname;
	uint8		nspnamelen;
	const char *relname;
	uint8		relnamelen;
	uint8		flags = 0;
		
    if (MtmIsFilteredTxn) { 
		return;
	}
	
	/* must not have cache entry if metacache off; must have entry if on */
	Assert( (data->relmeta_cache_size == 0) == (cache_entry == NULL) );
	/* if cache enabled must never be called with an already-cached rel */
	Assert(cache_entry == NULL || !cache_entry->is_cached);

	pq_sendbyte(out, 'R');		/* sending RELATION */
    
	/* send the flags field */
	pq_sendbyte(out, flags);

	/* use Oid as relation identifier */
	pq_sendint(out, RelationGetRelid(rel), 4);

	nspname = get_namespace_name(rel->rd_rel->relnamespace);
	if (nspname == NULL)
		elog(ERROR, "cache lookup failed for namespace %u",
			 rel->rd_rel->relnamespace);
	nspnamelen = strlen(nspname) + 1;
	
	relname = NameStr(rel->rd_rel->relname);
	relnamelen = strlen(relname) + 1;
    
	pq_sendbyte(out, nspnamelen);		/* schema name length */
	pq_sendbytes(out, nspname, nspnamelen);
    
	pq_sendbyte(out, relnamelen);		/* table name length */
	pq_sendbytes(out, relname, relnamelen);

	/* send the attribute info */
	pglogical_write_attrs(out, rel);

	/*
	 * Since we've sent the whole relation metadata not just the columns for
	 * the coming row(s), we can omit sending it again. The client will cache
	 * it. If the relation changes the cached flag is cleared by
	 * pglogical_output and we'll be called again next time it's touched.
	 *
	 * We don't care about the cache size here, the size management is done
	 * in the generic cache code.
	 */
	if (cache_entry != NULL)
		cache_entry->is_cached = true;
}

/*
 * Write relation attributes to the outputstream.
 */
static void
pglogical_write_attrs(StringInfo out, Relation rel)
{
	TupleDesc	desc;
	int			i;
	uint16		nliveatts = 0;
	Bitmapset  *idattrs;

	desc = RelationGetDescr(rel);

	pq_sendbyte(out, 'A');			/* sending ATTRS */

	/* send number of live attributes */
	for (i = 0; i < desc->natts; i++)
	{
		if (desc->attrs[i]->attisdropped)
			continue;
		nliveatts++;
	}
	pq_sendint(out, nliveatts, 2);

	/* fetch bitmap of REPLICATION IDENTITY attributes */
	idattrs = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_IDENTITY_KEY);

	/* send the attributes */
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = desc->attrs[i];
		uint8			flags = 0;
		uint16			len;
		const char	   *attname;

		if (att->attisdropped)
			continue;

		if (bms_is_member(att->attnum - FirstLowInvalidHeapAttributeNumber,
						  idattrs))
			flags |= IS_REPLICA_IDENTITY;

		pq_sendbyte(out, 'C');		/* column definition follows */
		pq_sendbyte(out, flags);

		pq_sendbyte(out, 'N');		/* column name block follows */
		attname = NameStr(att->attname);
		len = strlen(attname) + 1;
		pq_sendint(out, len, 2);
		pq_sendbytes(out, attname, len); /* data */
	}
}

/*
 * Write BEGIN to the output stream.
 */
static void
pglogical_write_begin(StringInfo out, PGLogicalOutputData *data,
					  ReorderBufferTXN *txn)
{
	bool isRecovery = MtmIsRecoveredNode(MtmReplicationNodeId);
	csn_t csn = MtmTransactionSnapshot(txn->xid);
	MTM_INFO("%d: pglogical_write_begin XID=%d node=%d CSN=%ld recovery=%d\n", MyProcPid, txn->xid, MtmReplicationNodeId, csn, isRecovery);
	
	if (csn == INVALID_CSN && !isRecovery) { 
		MtmIsFilteredTxn = true;
	} else { 
		pq_sendbyte(out, 'B');		/* BEGIN */
		pq_sendint(out, MtmNodeId, 4);
		pq_sendint(out, isRecovery ? InvalidTransactionId : txn->xid, 4);
		pq_sendint64(out, csn);
		MtmIsFilteredTxn = false;
	}
}

/*
 * Write COMMIT to the output stream.
 */
static void
pglogical_write_commit(StringInfo out, PGLogicalOutputData *data,
					   ReorderBufferTXN *txn, XLogRecPtr commit_lsn)
{
    uint8 flags = 0;
	
    if (txn->xact_action == XLOG_XACT_COMMIT) 
    	flags = PGLOGICAL_COMMIT;
	else if (txn->xact_action == XLOG_XACT_PREPARE)
    	flags = PGLOGICAL_PREPARE;
	else if (txn->xact_action == XLOG_XACT_COMMIT_PREPARED)
    	flags = PGLOGICAL_COMMIT_PREPARED;
	else if (txn->xact_action == XLOG_XACT_ABORT_PREPARED)
    	flags = PGLOGICAL_ABORT_PREPARED;
	else
    	Assert(false);

	if (flags == PGLOGICAL_COMMIT || flags == PGLOGICAL_PREPARE) { 
		if (MtmIsFilteredTxn) { 
			return;
		}
	} else { 
		csn_t csn = MtmTransactionSnapshot(txn->xid);
		bool isRecovery = MtmIsRecoveredNode(MtmReplicationNodeId);
		if (csn == INVALID_CSN && !isRecovery) {
			return;
		}
		if (MtmRecoveryCaughtUp(MtmReplicationNodeId, txn->end_lsn)) { 
			flags |= PGLOGICAL_CAUGHT_UP;
		}
	}
    pq_sendbyte(out, 'C');		/* sending COMMIT */

	MTM_INFO("PGLOGICAL_SEND commit: event=%d, gid=%s, commit_lsn=%lx, txn->end_lsn=%lx, xlog=%lx\n", flags, txn->gid, commit_lsn, txn->end_lsn, GetXLogInsertRecPtr());

    /* send the flags field */
    pq_sendbyte(out, flags);
    pq_sendbyte(out, MtmNodeId);

    /* send fixed fields */
    pq_sendint64(out, commit_lsn);
    pq_sendint64(out, txn->end_lsn);
    pq_sendint64(out, txn->commit_time);

	if (txn->xact_action == XLOG_XACT_COMMIT_PREPARED) { 
		pq_sendint64(out, MtmGetTransactionCSN(txn->xid));
	}
    if (txn->xact_action != XLOG_XACT_COMMIT) { 
    	pq_sendstring(out, txn->gid);
	}
}

/*
 * Write INSERT to the output stream.
 */
static void
pglogical_write_insert(StringInfo out, PGLogicalOutputData *data,
						Relation rel, HeapTuple newtuple)
{
    if (!MtmIsFilteredTxn) { 
		uint8 flags = 0;

		pq_sendbyte(out, 'I');		/* action INSERT */
		/* send the flags field */
		pq_sendbyte(out, flags);
		
		/* use Oid as relation identifier */
		pq_sendint(out, RelationGetRelid(rel), 4);

		pq_sendbyte(out, 'N');		/* new tuple follows */
		pglogical_write_tuple(out, data, rel, newtuple);
	}
}

/*
 * Write UPDATE to the output stream.
 */
static void
pglogical_write_update(StringInfo out, PGLogicalOutputData *data,
						Relation rel, HeapTuple oldtuple, HeapTuple newtuple)
{
    if (!MtmIsFilteredTxn) { 
		uint8 flags = 0;

		pq_sendbyte(out, 'U');		/* action UPDATE */

		/* send the flags field */
		pq_sendbyte(out, flags);
		
		/* use Oid as relation identifier */
		pq_sendint(out, RelationGetRelid(rel), 4);
		
		/* FIXME support whole tuple (O tuple type) */
		if (oldtuple != NULL)
		{
			pq_sendbyte(out, 'K');	/* old key follows */
			pglogical_write_tuple(out, data, rel, oldtuple);
		}
		
		pq_sendbyte(out, 'N');		/* new tuple follows */
		pglogical_write_tuple(out, data, rel, newtuple);
	}
}
	
/*
 * Write DELETE to the output stream.
 */
static void
pglogical_write_delete(StringInfo out, PGLogicalOutputData *data,
						Relation rel, HeapTuple oldtuple)
{
    if (!MtmIsFilteredTxn) {
		uint8 flags = 0;
		
		pq_sendbyte(out, 'D');		/* action DELETE */
		
		/* send the flags field */
		pq_sendbyte(out, flags);
		
		/* use Oid as relation identifier */
		pq_sendint(out, RelationGetRelid(rel), 4);
		
		/*
		 * TODO support whole tuple ('O' tuple type)
		 *
		 * See notes on update for details
		 */
		pq_sendbyte(out, 'K');	/* old key follows */
		pglogical_write_tuple(out, data, rel, oldtuple);
	}
}

/*
 * Most of the brains for startup message creation lives in
 * pglogical_config.c, so this presently just sends the set of key/value pairs.
 */
static void
write_startup_message(StringInfo out, List *msg)
{
}

/*
 * Write a tuple to the outputstream, in the most efficient format possible.
 */
static void
pglogical_write_tuple(StringInfo out, PGLogicalOutputData *data,
					   Relation rel, HeapTuple tuple)
{
	TupleDesc	desc;
	Datum		values[MaxTupleAttributeNumber];
	bool		isnull[MaxTupleAttributeNumber];
	int			i;
	uint16		nliveatts = 0;

	desc = RelationGetDescr(rel);

	pq_sendbyte(out, 'T');			/* sending TUPLE */

	for (i = 0; i < desc->natts; i++)
	{
		if (desc->attrs[i]->attisdropped)
			continue;
		nliveatts++;
	}
	pq_sendint(out, nliveatts, 2);

	/* try to allocate enough memory from the get go */
	enlargeStringInfo(out, tuple->t_len +
					  nliveatts * (1 + 4));

	/*
	 * XXX: should this prove to be a relevant bottleneck, it might be
	 * interesting to inline heap_deform_tuple() here, we don't actually need
	 * the information in the form we get from it.
	 */
	heap_deform_tuple(tuple, desc, values, isnull);

	for (i = 0; i < desc->natts; i++)
	{
		HeapTuple	typtup;
		Form_pg_type typclass;
		Form_pg_attribute att = desc->attrs[i];
		char		transfer_type;

		/* skip dropped columns */
		if (att->attisdropped)
			continue;

		if (isnull[i])
		{
			pq_sendbyte(out, 'n');	/* null column */
			continue;
		}
		else if (att->attlen == -1 && VARATT_IS_EXTERNAL_ONDISK(values[i]))
		{
			pq_sendbyte(out, 'u');	/* unchanged toast column */
			continue;
		}

		typtup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(att->atttypid));
		if (!HeapTupleIsValid(typtup))
			elog(ERROR, "cache lookup failed for type %u", att->atttypid);
		typclass = (Form_pg_type) GETSTRUCT(typtup);

		transfer_type = decide_datum_transfer(att, typclass,
											  data->allow_internal_basetypes,
											  data->allow_binary_basetypes);
        pq_sendbyte(out, transfer_type);
		switch (transfer_type)
		{
			case 'b':	/* internal-format binary data follows */

				/* pass by value */
				if (att->attbyval)
				{
					pq_sendint(out, att->attlen, 4); /* length */

					enlargeStringInfo(out, att->attlen);
					store_att_byval(out->data + out->len, values[i],
									att->attlen);
					out->len += att->attlen;
					out->data[out->len] = '\0';
				}
				/* fixed length non-varlena pass-by-reference type */
				else if (att->attlen > 0)
				{
					pq_sendint(out, att->attlen, 4); /* length */

					appendBinaryStringInfo(out, DatumGetPointer(values[i]),
										   att->attlen);
				}
				/* varlena type */
				else if (att->attlen == -1)
				{
					char *data = DatumGetPointer(values[i]);

					/* send indirect datums inline */
					if (VARATT_IS_EXTERNAL_INDIRECT(values[i]))
					{
						struct varatt_indirect redirect;
						VARATT_EXTERNAL_GET_POINTER(redirect, data);
						data = (char *) redirect.pointer;
					}

					Assert(!VARATT_IS_EXTERNAL(data));

					pq_sendint(out, VARSIZE_ANY(data), 4); /* length */

					appendBinaryStringInfo(out, data, VARSIZE_ANY(data));
				}
				else
					elog(ERROR, "unsupported tuple type");

				break;

			case 's': /* binary send/recv data follows */
				{
					bytea	   *outputbytes;
					int			len;

					outputbytes = OidSendFunctionCall(typclass->typsend,
													  values[i]);

					len = VARSIZE(outputbytes) - VARHDRSZ;
					pq_sendint(out, len, 4); /* length */
					pq_sendbytes(out, VARDATA(outputbytes), len); /* data */
					pfree(outputbytes);
				}
				break;

			default:
				{
					char   	   *outputstr;
					int			len;

					outputstr =	OidOutputFunctionCall(typclass->typoutput,
													  values[i]);
					len = strlen(outputstr) + 1;
					pq_sendint(out, len, 4); /* length */
					appendBinaryStringInfo(out, outputstr, len); /* data */
					pfree(outputstr);
				}
		}

		ReleaseSysCache(typtup);
	}
}

/*
 * Make the executive decision about which protocol to use.
 */
static char
decide_datum_transfer(Form_pg_attribute att, Form_pg_type typclass,
					  bool allow_internal_basetypes,
					  bool allow_binary_basetypes)
{
	/*
	 * Use the binary protocol, if allowed, for builtin & plain datatypes.
	 */
	if (allow_internal_basetypes &&
		typclass->typtype == 'b' &&
		att->atttypid < FirstNormalObjectId &&
		typclass->typelem == InvalidOid)
	{
		return 'b';
	}
	/*
	 * Use send/recv, if allowed, if the type is plain or builtin.
	 *
	 * XXX: we can't use send/recv for array or composite types for now due to
	 * the embedded oids.
	 */
	else if (allow_binary_basetypes &&
			 OidIsValid(typclass->typreceive) &&
			 (att->atttypid < FirstNormalObjectId || typclass->typtype != 'c') &&
			 (att->atttypid < FirstNormalObjectId || typclass->typelem == InvalidOid))
	{
		return 's';
	}

	return 't';
}


PGLogicalProtoAPI *
pglogical_init_api(PGLogicalProtoType typ)
{
    PGLogicalProtoAPI* res = palloc0(sizeof(PGLogicalProtoAPI));
	sscanf(MyReplicationSlot->data.name.data, MULTIMASTER_SLOT_PATTERN, &MtmReplicationNodeId);
	elog(WARNING, "%d: PRGLOGICAL init API for slot %s node %d", MyProcPid, MyReplicationSlot->data.name.data, MtmReplicationNodeId);
    res->write_rel = pglogical_write_rel;
    res->write_begin = pglogical_write_begin;
    res->write_commit = pglogical_write_commit;
    res->write_insert = pglogical_write_insert;
    res->write_update = pglogical_write_update;
    res->write_delete = pglogical_write_delete;
	res->setup_hooks = MtmSetupReplicationHooks;
    res->write_startup_message = write_startup_message;
    return res;
}
