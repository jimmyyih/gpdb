#include "postgres.h"

#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/gist_private.h"
#include "access/gin.h"
#include "libpq/md5.h"
#include "replication/walsender_private.h"
#include "replication/walsender.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"

#define atooid(x) ((Oid) strtoul((x), NULL, 10))
#define MD5_BUFLEN 33 /* MD5 is 32 bytes + 1 null-terminator */
#define MAX_INCLUDE_RELATION_TYPES 8

PG_MODULE_MAGIC;

extern Datum gp_replica_check(PG_FUNCTION_ARGS);

typedef struct RelfilenodeEntry
{
	Oid relfilenode;
	int relam;
	int relkind;
	char relstorage;
} RelfilenodeEntry;

typedef struct WalSenderInfo
{
	XLogRecPtr sent;
	XLogRecPtr flush;
	XLogRecPtr apply;
	WalSndState state;
} WalSenderInfo;

typedef struct RelationTypeData
{
	char *name;
	bool include;
} RelationTypeData;

static RelationTypeData relation_types[MAX_INCLUDE_RELATION_TYPES] = {
	{"btree", false},
	{"hash", false},
	{"gist", false},
	{"gin", false},
	{"bitmap", false},
	{"heap", false},
	{"sequence", false},
	{"ao", false}
};

static void init_relation_types(char *include_relation_types);
static RelationTypeData get_relation_type_data(int relam, char relstorage, int relkind);
static void mask_block(char *pagedata, BlockNumber blkno, Oid relam);
static bool compare_files(char* primaryfilepath, char* mirrorfilepath, char *relfilenode, HTAB *relfilenode_map);
static void get_walsender_info(WalSenderInfo **walsndinfo);
static bool check_walsender_synced();
static XLogRecPtr* get_sent_lsns();
static bool compare_sent_lsns(XLogRecPtr *start_sent_lsns, XLogRecPtr *end_sent_lsns);

static void
init_relation_types(char *include_relation_types)
{
	List *elemlist;
	ListCell *l;

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(include_relation_types, ',', &elemlist))
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
		bool found = false;
		int type;

		/* Check for 'all'. */
		if (pg_strcasecmp(tok, "all") == 0)
		{
			for (type = 0; type < MAX_INCLUDE_RELATION_TYPES; type++)
				relation_types[type].include = true;

			found = true;
		}
		else
		{
			for (type = 0; type < MAX_INCLUDE_RELATION_TYPES; type++)
			{
				if (pg_strcasecmp(tok, relation_types[type].name) == 0)
				{
					relation_types[type].include = true;
					found = true;
					break;
				}
			}
		}

		if (!found)
		{
			list_free(elemlist);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Unrecognized key word: \"%s\".", tok)));
		}
	}

	list_free(elemlist);
}

static RelationTypeData
get_relation_type_data(int relam, char relstorage, int relkind)
{
	switch(relam)
	{
		case BTREE_AM_OID:
			return relation_types[0];
		case HASH_AM_OID:
			return relation_types[1];
		case GIST_AM_OID:
			return relation_types[2];
		case GIN_AM_OID:
			return relation_types[3];
		case BITMAP_AM_OID:
			return relation_types[4];
		default:
			if (relstorage == RELSTORAGE_HEAP)
				if (relkind == RELKIND_SEQUENCE)
					return relation_types[6];
				else
					return relation_types[5];
			else if (relstorage_is_ao(relstorage))
				return relation_types[7];
			else
				elog(ERROR, "invalid relam %d or relstorage %c", relam, relstorage);
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
	char primaryFileBuf[BLCKSZ];
	char mirrorFileBuf[BLCKSZ];
	char primaryfilechecksum[MD5_BUFLEN] = {0};
	char mirrorfilechecksum[MD5_BUFLEN] = {0};
	int primaryFileBytesRead;
	int mirrorFileBytesRead;
	int blockno = 0;
	bool match = false;

	primaryFile = PathNameOpenFile(primaryfilepath, O_RDONLY | PG_BINARY, S_IRUSR);
	if (primaryFile < 0)
		return false;

	mirrorFile = PathNameOpenFile(mirrorfilepath, O_RDONLY | PG_BINARY, S_IRUSR);
	if (mirrorFile < 0)
	{
		FileClose(primaryFile);
		return false;
	}

	int rnode = atooid(relfilenode);
	bool found = false;
	RelfilenodeEntry *entry = (RelfilenodeEntry *)hash_search(relfilenode_map, (void *)&rnode, HASH_FIND, &found);

	while (true)
	{
		primaryFileBytesRead = FileRead(primaryFile, primaryFileBuf, sizeof(primaryFileBuf));
		mirrorFileBytesRead = FileRead(mirrorFile, mirrorFileBuf, sizeof(mirrorFileBuf));

		if (primaryFileBytesRead != mirrorFileBytesRead
			|| primaryFileBytesRead < 0)
		{
			elog(WARNING, "%s relfilenode %s blockno %d is mismatched",
				 get_relation_type_data(entry->relam, entry->relstorage, entry->relkind).name,
				 relfilenode, blockno);
			break;
		}

		if (primaryFileBytesRead == 0)
		{
			match = true;
			break; /* reached EOF */
		}

		if (entry->relstorage == RELSTORAGE_HEAP)
		{
			mask_block(primaryFileBuf, blockno, entry->relam);
			mask_block(mirrorFileBuf, blockno, entry->relam);
		}

		pg_md5_hash(primaryFileBuf, BLCKSZ, primaryfilechecksum);
		pg_md5_hash(mirrorFileBuf, BLCKSZ, mirrorfilechecksum);

		if (strcmp(primaryfilechecksum, mirrorfilechecksum) != 0)
		{
			elog(WARNING, "%s relfilenode %s blockno %d is mismatched",
				 get_relation_type_data(entry->relam, entry->relstorage, entry->relkind).name,
				 relfilenode, blockno);
			break;
		}

		blockno++;
	}

	FileClose(primaryFile);
	FileClose(mirrorFile);

	return match;
}

static void
get_walsender_info(WalSenderInfo **walsndinfo)
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

		get_walsender_info(walsndinfo);
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
get_sent_lsns()
{
	int i;
	XLogRecPtr *sent_lsns;
	WalSenderInfo *walsndinfo[max_wal_senders];

	get_walsender_info(walsndinfo);

	sent_lsns = (XLogRecPtr *)palloc(max_wal_senders * sizeof(WalSenderInfo));
	for (i = 0; i < max_wal_senders; ++i)
	{
		sent_lsns[i] = walsndinfo[i]->sent;
	}

	return sent_lsns;
}

static bool
compare_sent_lsns(XLogRecPtr *start_sent_lsns, XLogRecPtr *end_sent_lsns)
{
	int i;

	for (i = 0; i < max_wal_senders; ++i)
	{
		if (!XLByteEQ(start_sent_lsns[i], end_sent_lsns[i]))
			return false;
	}

	return true;
}

static HTAB*
get_relfilenode_map()
{
	Relation pg_class;
	HeapScanDesc scan;
	HeapTuple tup = NULL;

	HTAB *relfilenodemap;
	HASHCTL relfilenodectl;
	int hash_flags;
	MemSet(&relfilenodectl, 0, sizeof(relfilenodectl));
	relfilenodectl.keysize = sizeof(Oid);
	relfilenodectl.entrysize = sizeof(RelfilenodeEntry);
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
				&& !relstorage_is_ao(classtuple->relstorage)))
			continue;

		if (!get_relation_type_data(classtuple->relam, classtuple->relstorage, classtuple->relkind).include)
			continue;

		RelfilenodeEntry *rent;
		int rnode = classtuple->relfilenode;
		rent = hash_search(relfilenodemap, (void *)&rnode, HASH_ENTER, NULL);
		rent->relfilenode = classtuple->relfilenode;
		rent->relam = classtuple->relam;
		rent->relkind = classtuple->relkind;
		rent->relstorage = classtuple->relstorage;
	}
	heap_endscan(scan);
	heap_close(pg_class, AccessShareLock);

	return relfilenodemap;
}

static HTAB*
init_primary_file_set()
{
	HASHCTL primary_file_ctl;
	int hash_flags;

	MemSet(&primary_file_ctl, 0, sizeof(primary_file_ctl));
	primary_file_ctl.keysize = MAXPGPATH;
	primary_file_ctl.entrysize = MAXPGPATH;
	primary_file_ctl.hash = string_hash;
	hash_flags = (HASH_ELEM | HASH_FUNCTION);

	return hash_create("primary files set", 50000, &primary_file_ctl, hash_flags);
}

PG_FUNCTION_INFO_V1(gp_replica_check);

Datum
gp_replica_check(PG_FUNCTION_ARGS)
{
	char *primarydirpath = TextDatumGetCString(PG_GETARG_DATUM(0));
	char *mirrordirpath = TextDatumGetCString(PG_GETARG_DATUM(1));
	struct dirent *dent = NULL;
	bool dir_equal = true;

	/* Verify LSN values are correct through pg_stat_replication */
	if (!check_walsender_synced())
		PG_RETURN_BOOL(false);

	init_relation_types(TextDatumGetCString(PG_GETARG_DATUM(2)));

	DIR *primarydir = AllocateDir(primarydirpath);
	DIR *mirrordir = AllocateDir(mirrordirpath);

	/* Store the last commit to compare at the end */
	XLogRecPtr *start_sent_lsns = get_sent_lsns();

	HTAB *relfilenode_map = get_relfilenode_map();
	HTAB *primary_file_set = init_primary_file_set();

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

		char *d_name_copy = pstrdup(dent->d_name);
		char *relfilenode = strtok(d_name_copy, ".");
		bool include_found;

		int rnode = atooid(relfilenode);
		hash_search(relfilenode_map, (void *)&rnode, HASH_FIND, &include_found);
		if (!include_found || d_name_copy == NULL)
			continue;

		file_eq = compare_files(primaryfilename, mirrorfilename, relfilenode, relfilenode_map);

		if (!file_eq)
			elog(WARNING, "Files %s and %s differ", primaryfilename, mirrorfilename);

		dir_equal = dir_equal && file_eq;

		/* Store each filename for fileset comparison later */
		hash_search(primary_file_set, dent->d_name, HASH_ENTER, NULL);
	}
	FreeDir(primarydir);

	/* Open up mirrordirpath and verify each mirror file exist in the primary hash table */
	bool found = false;
	while ((dent = ReadDir(mirrordir, mirrordirpath)) != NULL)
	{
		if (strncmp(dent->d_name, "pg", 2) == 0
			|| strncmp(dent->d_name, ".", 1) == 0)
			continue;

		char *d_name_copy = pstrdup(dent->d_name);
		char *relfilenode = strtok(d_name_copy, ".");
		bool include_found;

		int rnode = atooid(relfilenode);
		hash_search(relfilenode_map, (void *)&rnode, HASH_FIND, &include_found);
		if (!include_found || d_name_copy == NULL)
			continue;

		hash_search(primary_file_set, dent->d_name, HASH_FIND, &found);
		if (!found)
			elog(WARNING, "Found extra file on mirror: %s/%s", mirrordirpath, dent->d_name);
	}
	FreeDir(mirrordir);
	hash_destroy(relfilenode_map);
	hash_destroy(primary_file_set);

	/* Compare stored last commit with now */
	XLogRecPtr *end_sent_lsns = get_sent_lsns();

	if (!compare_sent_lsns(start_sent_lsns, end_sent_lsns))
	{
		elog(WARNING, "IO may have been performed during the check. Results may not be correct.");
		PG_RETURN_BOOL(false);
	}

	PG_RETURN_BOOL(dir_equal);
}
