#include "postgres.h"

#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/gist_private.h"
#include "access/gin.h"
#include "access/slru.h"
#include "libpq/md5.h"
#include "replication/walsender_private.h"
#include "replication/walsender.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"

PG_MODULE_MAGIC;

extern Datum gp_replica_check(PG_FUNCTION_ARGS);

typedef struct relfilenodentry
{
	Oid relfilenode;
	int relam;
	char relstorage;
} relfilenodeentry;

typedef struct WalSenderInfo
{
	XLogRecPtr sent;
	XLogRecPtr write;
	XLogRecPtr flush;
	XLogRecPtr apply;
	WalSndState state;
} WalSenderInfo;

static void init_check_relation_type(char *newval);
static int get_relation_type(int relam, char relstorage, int relkind);
static char* relam_to_string(Oid relam);
static void mask_block(char *pagedata, BlockNumber blkno, Oid relam);
static bool compare_files(char* primaryfilepath, char* mirrorfilepath, char *relfilenode, HTAB *relfilenode_map);
static void get_replication_info(WalSenderInfo **walsndinfo);
static bool check_walsender_synced();
static XLogRecPtr* get_last_sent_lsn();
static bool compare_last_sent_lsns(XLogRecPtr *start_sent_lsns, XLogRecPtr *end_sent_lsns);

#define SLRU_PAGES_PER_SEGMENT 32

static bool check_relation_type[8];

static void
init_check_relation_type(char *newval)
{
	List *elemlist;
	ListCell *l;

	/* Initialize the array */
	MemSet(check_relation_type, 0, sizeof(check_relation_type) * sizeof(bool));

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(newval, ',', &elemlist))
	{
		list_free(elemlist);

		/* syntax error in list */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("List syntax is invalid.")));
	}

	foreach(l, elemlist)
	{
		char *tok = (char *) lfirst(l);
		int type;

		/* Check for 'all'. */
		if (pg_strcasecmp(tok, "all") == 0)
		{
			for (type = 0; type < sizeof(check_relation_type); type++)
					check_relation_type[type] = true;
		}
		else
		{
			if (pg_strcasecmp(tok, "btree") == 0)
				check_relation_type[0] = true;
			else if (pg_strcasecmp(tok, "hash") == 0)
				check_relation_type[1] = true;
			else if (pg_strcasecmp(tok, "gist") == 0)
				check_relation_type[2] = true;
			else if (pg_strcasecmp(tok, "gin") == 0)
				check_relation_type[3] = true;
			else if (pg_strcasecmp(tok, "bitmap") == 0)
				check_relation_type[4] = true;
			else if (pg_strcasecmp(tok, "heap") == 0)
				check_relation_type[5] = true;
			else if (pg_strcasecmp(tok, "ao") == 0)
				check_relation_type[6] = true;
			else if (pg_strcasecmp(tok, "sequence") == 0)
				check_relation_type[7] = true;
			else
			{
				list_free(elemlist);
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("Unrecognized key word: \"%s\".", tok)));
			}

		}
	}

	list_free(elemlist);
}

static int
get_relation_type(int relam, char relstorage, int relkind)
{
	switch(relam)
	{
		case BTREE_AM_OID:
			return 0;
		case HASH_AM_OID:
			return 1;
		case GIST_AM_OID:
			return 2;
		case GIN_AM_OID:
			return 3;
		case BITMAP_AM_OID:
			return 4;
		default:
			if (relstorage == RELSTORAGE_HEAP)
				if (relkind == RELKIND_SEQUENCE)
					return 7;
				else
					return 5;
			else if (relstorage_is_ao(relstorage))
				return 6;
			else
				elog(ERROR, "bad relam or relstorage %d %c", relam, relstorage);
	}
}

static char*
relam_to_string(Oid relam)
{
	switch(relam)
	{
		case BTREE_AM_OID:
			return "b-tree index";
		case HASH_AM_OID:
			return "hash index";
		case GIST_AM_OID:
			return "GiST index";
		case GIN_AM_OID:
			return "GIN index";
		case BITMAP_AM_OID:
			return "bitmap index";
		default:
			return "heap table";
	}
}

static void
mask_block(char *pagedata, BlockNumber blockno, Oid relam)
{
	switch(relam)
	{
		case BTREE_AM_OID:
			btree_mask(pagedata, blockno);
			break;

		case GIST_AM_OID:
			gist_mask(pagedata, blockno);
			break;

		case GIN_AM_OID:
			gin_mask(pagedata, blockno);
			break;

		/* heap table */
		default:
			heap_mask(pagedata, blockno);
			break;
	}
}

static bool
compare_files(char* primaryfilepath, char* mirrorfilepath, char *relfilenode, HTAB *relfilenode_map)
{
	File primaryFile = 0;
	File mirrorFile = 0;
	char primaryFileBuf[BLCKSZ * SLRU_PAGES_PER_SEGMENT];
	char mirrorFileBuf[BLCKSZ * SLRU_PAGES_PER_SEGMENT];
	char primaryfilechecksum[SLRU_MD5_BUFLEN] = {0};
	char mirrorfilechecksum[SLRU_MD5_BUFLEN] = {0};
	char maskedPrimaryBuf[BLCKSZ];
	char maskedMirrorBuf[BLCKSZ];
	int primaryFileBytesRead;
	int mirrorFileBytesRead;
	int blockno = 0;

	primaryFile = PathNameOpenFile(primaryfilepath, O_RDONLY | PG_BINARY, S_IRUSR);
	if (primaryFile < 0)
		return false;

	mirrorFile = PathNameOpenFile(mirrorfilepath, O_RDONLY | PG_BINARY, S_IRUSR);
	if (mirrorFile < 0)
		return false;

	primaryFileBytesRead = FileRead(primaryFile, primaryFileBuf, sizeof(primaryFileBuf));
	if (primaryFileBytesRead >= 0)
		pg_md5_hash(primaryFileBuf, primaryFileBytesRead, primaryfilechecksum);

	mirrorFileBytesRead = FileRead(mirrorFile, mirrorFileBuf, sizeof(mirrorFileBuf));
	if (mirrorFileBytesRead >= 0)
		pg_md5_hash(mirrorFileBuf, mirrorFileBytesRead, mirrorfilechecksum);

	FileClose(primaryFile);
	FileClose(mirrorFile);

 	if (strcmp(primaryfilechecksum, mirrorfilechecksum) == 0)
		return true;

	int rnode = atoi(relfilenode);
	bool found = false;
	relfilenodeentry *entry = (relfilenodeentry *)hash_search(relfilenode_map, (void *)&rnode, HASH_FIND, &found);
	if (relstorage_is_ao(entry->relstorage))
	{
		/*
		 * We do not expect the AO files to ever have more data on the primary
		 * than on the mirror so the above md5 checksum checks should suffice.
		 */
		elog(WARNING, "Skipping AO file block check");
		return false;
	}
	else if (entry->relstorage == 'h')
	{
		while (blockno * BLCKSZ < primaryFileBytesRead)
		{
			mask_block(primaryFileBuf + blockno * BLCKSZ, blockno, entry->relam);
			mask_block(mirrorFileBuf + blockno * BLCKSZ, blockno, entry->relam);

			memcpy(maskedPrimaryBuf, primaryFileBuf + blockno * BLCKSZ, BLCKSZ);
			memcpy(maskedMirrorBuf, mirrorFileBuf + blockno * BLCKSZ, BLCKSZ);

			pg_md5_hash(maskedPrimaryBuf, BLCKSZ, primaryfilechecksum);
			pg_md5_hash(maskedMirrorBuf, BLCKSZ, mirrorfilechecksum);

			if (strcmp(primaryfilechecksum, mirrorfilechecksum) != 0)
			{
				elog(WARNING, "%s relfilenode %s blockno %d is mismatched",
					 relam_to_string(entry->relam), relfilenode, blockno);
				return false;
			}

			blockno += 1;
		}
	}
	else
	{
		elog(WARNING, "skipping compare somehow... %s", relfilenode);
	}
	return true;
}

static void
get_replication_info(WalSenderInfo **walsndinfo)
{
	int i;

	LWLockAcquire(SyncRepLock, LW_SHARED);
	for (i = 0; i < max_wal_senders; i++)
	{
		volatile WalSnd *walsnd = &WalSndCtl->walsnds[i];

		if (walsnd->pid != 0)
		{
			walsndinfo[i] = (WalSenderInfo *)palloc(sizeof(WalSenderInfo));
			walsndinfo[i]->sent = walsnd->sentPtr;
			walsndinfo[i]->write = walsnd->write;
			walsndinfo[i]->flush = walsnd->flush;
			walsndinfo[i]->apply = walsnd->apply;
			walsndinfo[i]->state = walsnd->state;
		}
	}
	LWLockRelease(SyncRepLock);
}

static bool
check_walsender_synced()
{
	int			i;
	bool		synced;
	int			max_retry = 60;
	int			retry = 0;

	WalSenderInfo *walsndinfo[max_wal_senders];

	while (retry < max_retry)
	{
		synced = true;
		memset(walsndinfo, 0, sizeof(walsndinfo));

		get_replication_info(walsndinfo);
		for (i = 0; i < max_wal_senders; i++)
		{
			synced = synced && (walsndinfo[i]->state == WALSNDSTATE_STREAMING &&
								XLByteEQ(walsndinfo[i]->flush, walsndinfo[i]->apply));
		}

		if (synced)
			break;

		pg_usleep(1000000);
		retry++;
	}

	return synced;
}

static XLogRecPtr*
get_last_sent_lsn()
{
	int i;
	XLogRecPtr *sent_lsns;
	WalSenderInfo *walsndinfo[max_wal_senders];

	get_replication_info(walsndinfo);

	sent_lsns = (XLogRecPtr *)palloc(max_wal_senders * sizeof(WalSenderInfo));
	for (i = 0; i < max_wal_senders; ++i)
	{
		sent_lsns[i] = walsndinfo[i]->sent;
	}

	return sent_lsns;
}

static bool
compare_last_sent_lsns(XLogRecPtr *start_sent_lsns, XLogRecPtr *end_sent_lsns)
{
	int i;

	for (i = 0; i < max_wal_senders; ++i)
	{
		if (!XLByteEQ(start_sent_lsns[i], end_sent_lsns[i]))
			return false;
	}

	return true;
}

static
HTAB* get_relfilenode_map()
{
	Relation pg_class;
	HeapScanDesc scan;
	HeapTuple tup = NULL;

	HTAB *relfilenodemap;
	HASHCTL relfilenodectl;
	int hash_flags;
	MemSet(&relfilenodectl, 0, sizeof(relfilenodectl));
	relfilenodectl.keysize = sizeof(Oid);
	relfilenodectl.entrysize = sizeof(relfilenodeentry);
	relfilenodectl.hash = oid_hash;
	hash_flags = (HASH_ELEM | HASH_FUNCTION);

	relfilenodemap = hash_create("relfilenode map", 50000, &relfilenodectl, hash_flags);

	pg_class = heap_open(RelationRelationId, AccessShareLock);
	scan = heap_beginscan(pg_class, SnapshotNow, 0, NULL);
	while((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classtuple = (Form_pg_class) GETSTRUCT(tup);
		if ((classtuple->relkind != RELKIND_INDEX
			&& classtuple->relkind != RELKIND_RELATION
			&& classtuple->relkind != RELKIND_SEQUENCE)
			|| (classtuple->relstorage != RELSTORAGE_HEAP
				&& classtuple->relstorage != RELSTORAGE_AOROWS
				&& classtuple->relstorage != RELSTORAGE_AOCOLS))
			continue;

		if (!check_relation_type[get_relation_type(classtuple->relam, classtuple->relstorage, classtuple->relkind)])
			continue;

		relfilenodeentry *rent;
		int rnode = classtuple->relfilenode;
		rent = hash_search(relfilenodemap, (void *)&rnode, HASH_ENTER, NULL);
		rent->relfilenode = classtuple->relfilenode;
		rent->relam = classtuple->relam;
		rent->relstorage = classtuple->relstorage;
	}
	heap_endscan(scan);
	heap_close(pg_class, AccessShareLock);

	return relfilenodemap;
}

PG_FUNCTION_INFO_V1(gp_replica_check);

Datum
gp_replica_check(PG_FUNCTION_ARGS)
{
	char *primarydirpath = TextDatumGetCString(PG_GETARG_DATUM(0));
	char *mirrordirpath = TextDatumGetCString(PG_GETARG_DATUM(1));
	DIR *primarydir = AllocateDir(primarydirpath);
	DIR *mirrordir = AllocateDir(mirrordirpath);
	struct dirent *dent = NULL;
	bool dir_equal = true;

	HTAB *primaryfileshash;
	HASHCTL primaryfiles;
	int hash_flags;
	MemSet(&primaryfiles, 0, sizeof(primaryfiles));
	primaryfiles.keysize = MAXPGPATH;
	primaryfiles.entrysize = MAXPGPATH;
	primaryfiles.hash = string_hash;
	hash_flags = (HASH_ELEM | HASH_FUNCTION);

	primaryfileshash = hash_create("primary files hash table", 50000, &primaryfiles, hash_flags);

	init_check_relation_type(TextDatumGetCString(PG_GETARG_DATUM(2)));

	/* Verify LSN values are correct through pg_stat_replication */
	if (!check_walsender_synced())
		PG_RETURN_BOOL(false);

	/* Store the last commit to compare at the end */
	XLogRecPtr *start_sent_lsns = get_last_sent_lsn();

	HTAB *relfilenode_map = get_relfilenode_map();

	while ((dent = ReadDir(primarydir, primarydirpath)) != NULL)
	{
		char primaryfilename[MAXPGPATH] = {'\0'};
		char mirrorfilename[MAXPGPATH] = {'\0'};
		bool file_eq = false;

		if (strncmp(dent->d_name, "pg", 2) == 0
			|| strncmp(dent->d_name, ".", 1) == 0)
			continue;

		sprintf(primaryfilename, "%s/%s", primarydirpath, dent->d_name);
		sprintf(mirrorfilename, "%s/%s", mirrordirpath, dent->d_name);

		char *d_name_copy = strdup(dent->d_name);
		char *relfilenode = strtok(d_name_copy, ".");
		bool include_found;

		int rnode = atoi(relfilenode);
		hash_search(relfilenode_map, (void *)&rnode, HASH_FIND, &include_found);
		if (!include_found || d_name_copy == NULL)
			continue;

		file_eq = compare_files(primaryfilename, mirrorfilename, relfilenode, relfilenode_map);

		if (!file_eq)
			elog(WARNING, "Files %s and %s differ", primaryfilename, mirrorfilename);

		dir_equal = dir_equal && file_eq;

		/* Store each filename for fileset comparison later */
		hash_search(primaryfileshash, dent->d_name, HASH_ENTER, NULL);
	}

	FreeDir(primarydir);

	/* Open up mirrordirpath and verify each mirror file exist in the primary hash table */
	bool found = false;
	while ((dent = ReadDir(mirrordir, mirrordirpath)) != NULL)
	{
		if (strncmp(dent->d_name, "pg", 2) == 0
			|| strncmp(dent->d_name, ".", 1) == 0)
			continue;

		char *d_name_copy = strdup(dent->d_name);
		char *relfilenode = strtok(d_name_copy, ".");
		bool include_found;

		int rnode = atoi(relfilenode);
		hash_search(relfilenode_map, (void *)&rnode, HASH_FIND, &include_found);
		if (!include_found || d_name_copy == NULL)
			continue;

		hash_search(primaryfileshash, dent->d_name, HASH_FIND, &found);
		if (!found)
			elog(WARNING, "Found extra file on mirror: %s/%s", mirrordirpath, dent->d_name);
	}
	FreeDir(mirrordir);

	/* Compare stored last commit with now */
	XLogRecPtr *end_sent_lsns = get_last_sent_lsn();

	if (!compare_last_sent_lsns(start_sent_lsns, end_sent_lsns))
	{
		elog(WARNING, "IO may have been performed during the check. Results may not be correct.");
		PG_RETURN_BOOL(false);
	}

	PG_RETURN_BOOL(dir_equal);
}
