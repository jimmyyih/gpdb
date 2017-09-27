#include "postgres.h"

#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/gist_private.h"
#include "access/gin.h"
#include "access/slru.h"
#include "libpq/md5.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

extern Datum gp_replica_check(PG_FUNCTION_ARGS);

static void mask_block(char *pagedata, BlockNumber blkno, Oid relam);
static bool compare_files(char* primaryfilepath, char* mirrorfilepath, char *relfilenode);

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

PG_FUNCTION_INFO_V1(gp_replica_check);

Datum
gp_replica_check(PG_FUNCTION_ARGS)
{
	char *primarydirpath = TextDatumGetCString(PG_GETARG_DATUM(0));
	char *mirrordirpath = TextDatumGetCString(PG_GETARG_DATUM(1));
	DIR *primarydir = AllocateDir(primarydirpath);
	struct dirent *dent = NULL;
	bool dir_equal = true;

	while ((dent = ReadDir(primarydir, primarydirpath)) != NULL)
	{
		char primaryfilename[MAXPGPATH] = {'\0'};
		char mirrorfilename[MAXPGPATH] = {'\0'};
		bool file_eq = false;

		if ((strncmp(dent->d_name, "pg", 2)) == 0)
			continue;

		sprintf(primaryfilename, "%s/%s", primarydirpath, dent->d_name);
		sprintf(mirrorfilename, "%s/%s", mirrordirpath, dent->d_name);

		file_eq = compare_files(primaryfilename, mirrorfilename, dent->d_name);

		if (!file_eq)
			elog(WARNING, "Files %s and %s differ", primaryfilename, mirrorfilename);

		dir_equal &= file_eq;
	}

	FreeDir(primarydir);

	PG_RETURN_BOOL(dir_equal);
}
