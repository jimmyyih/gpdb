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

typedef struct WalSenderInfo
{
	XLogRecPtr sent;
	XLogRecPtr write;
	XLogRecPtr flush;
	XLogRecPtr apply;
	WalSndState state;
} WalSenderInfo;

static void mask_block(char *pagedata, BlockNumber blkno, Oid relam);
static bool compare_files(char* primaryfilepath, char* mirrorfilepath, char *relfilenode);
static void get_replication_info(WalSenderInfo **walsndinfo);
static bool check_walsender_synced();
static XLogRecPtr* get_last_sent_lsn();
static bool compare_last_sent_lsns(XLogRecPtr *start_sent_lsns, XLogRecPtr *end_sent_lsns);

#define SLRU_PAGES_PER_SEGMENT 32

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
compare_files(char* primaryfilepath, char* mirrorfilepath, char *relfilenode)
{
	File primaryFile = 0;
	File mirrorFile = 0;
	char primaryFileBuf[BLCKSZ * SLRU_PAGES_PER_SEGMENT];
	char mirrorFileBuf[BLCKSZ * SLRU_PAGES_PER_SEGMENT];
	int primaryFileBytesRead;
	int mirrorFileBytesRead;

	char primaryfilechecksum[SLRU_MD5_BUFLEN] = {0};
	char mirrorfilechecksum[SLRU_MD5_BUFLEN] = {0};

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

	char maskedPrimaryBuf[BLCKSZ];
	char maskedMirrorBuf[BLCKSZ];
	int blockno = 0;

	/* Check the type of object */
	Relation pg_class = heap_open(RelationRelationId, AccessShareLock);
	HeapScanDesc scan = heap_beginscan(pg_class, SnapshotNow, 0, NULL);
	HeapTuple tup = NULL;
	char relstorage = 0;
	Oid relam;
	while((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classtuple = (Form_pg_class) GETSTRUCT(tup);

		if (classtuple->relfilenode != atoi(relfilenode)) /* change to strtoul later */
			continue;

		if (classtuple->relkind == RELKIND_INDEX)
			relam = classtuple->relam;
		else if (classtuple->relkind == RELKIND_RELATION)
			relam = 0;

		relstorage = classtuple->relstorage;
		break;
	}
	heap_endscan(scan);
	heap_close(pg_class, AccessShareLock);

	if (!relstorage)
	{
		elog(WARNING, "Did not get a valid relstorage for %s", relfilenode);
		return false;
	}

	if (relstorage == 'a')
	{
		// Use appendonly access methods to do block by block comparison
	}
	else if (relstorage == 'h')
	{
		while (blockno * BLCKSZ < primaryFileBytesRead)
		{
			mask_block(primaryFileBuf + blockno * BLCKSZ, blockno, relam);
			mask_block(mirrorFileBuf + blockno * BLCKSZ, blockno, relam);

			memcpy(maskedPrimaryBuf, primaryFileBuf + blockno * BLCKSZ, BLCKSZ);
			memcpy(maskedMirrorBuf, mirrorFileBuf + blockno * BLCKSZ, BLCKSZ);

			pg_md5_hash(maskedPrimaryBuf, BLCKSZ, primaryfilechecksum);
			pg_md5_hash(maskedMirrorBuf, BLCKSZ, mirrorfilechecksum);

			if (strcmp(primaryfilechecksum, mirrorfilechecksum) != 0)
			{
				elog(WARNING, "relfilenode %s blockno %d is mismatched", relfilenode, blockno);
				return false;
			}

			blockno += 1;
		}
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
		/* use volatile pointer to prevent code rearrangement */
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
		{
			elog(NOTICE, "start_sent_lsn = %X, end_sent_lsn = %X",
				 start_sent_lsns[i].xrecoff, end_sent_lsns[i].xrecoff);
			return false;
		}
	}

	return true;
}


typedef struct primaryfileentry
{
	char name[MAXPGPATH];
} primaryfileentry;

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
	primaryfiles.entrysize = sizeof(primaryfileentry);
	primaryfiles.hash = string_hash;
	hash_flags = (HASH_ELEM | HASH_FUNCTION);

	primaryfileshash = hash_create("primary files hash table", 50000, &primaryfiles, hash_flags);

	/* Verify LSN values are correct through pg_stat_replication */
	if (!check_walsender_synced())
		PG_RETURN_BOOL(false);

	/* Store the last commit to compare at the end */
	XLogRecPtr *start_sent_lsns = get_last_sent_lsn();

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

		file_eq = compare_files(primaryfilename, mirrorfilename, dent->d_name);

		if (!file_eq)
			elog(WARNING, "Files %s and %s differ", primaryfilename, mirrorfilename);

		dir_equal = dir_equal && file_eq;

		// Store each dent->d_name into a hash table
		bool found;
		hash_search(primaryfileshash, dent->d_name, HASH_ENTER, &found);
	}

	FreeDir(primarydir);

	// Open up mirrordirpath and verify each mirror file exist in the primary hashmap as well
	bool found = false;
	while ((dent = ReadDir(mirrordir, mirrordirpath)) != NULL)
	{
		if (strncmp(dent->d_name, "pg", 2) == 0
			|| strncmp(dent->d_name, ".", 1) == 0)
			continue;

		hash_search(primaryfileshash, dent->d_name, HASH_FIND, &found);
		if (!found)
			elog(WARNING, "Found extra file on mirror: %s", dent->d_name);
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


