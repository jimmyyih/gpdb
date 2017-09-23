#include <ftw.h>

#include "postgres.h"
#include "fmgr.h"
#include "libpq/md5.h"
#include "utils/builtins.h"
#include "executor/spi.h"
#include "catalog/gp_segment_config.h"

#include "access/heapam.h"

PG_MODULE_MAGIC;

static bool compare_files(char* primaryfilepath, char* mirrorfilepath);

typedef struct fspair
{
	char* primaryfslocation;
	char* mirrorfslocation;
} fspair;

static bool
compare_files(char* primaryfilepath, char* mirrorfilepath)
{
	File primaryFile = 0;
	File mirrorFile = 0;
	char primaryFileBuf[BLCKSZ * 32];
	char mirrorFileBuf[BLCKSZ * 32];
	int primaryFileBytesRead;
	int mirrorFileBytesRead;

	char primaryfilechecksum[33] = {0};
	char mirrorfilechecksum[33] = {0};

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

	elog(NOTICE, "file checksums do not match, doing block by block with masking on file %s", primaryfilepath);
	while (blockno * BLCKSZ < primaryFileBytesRead)
	{
		heap_mask(primaryFileBuf + blockno * BLCKSZ, blockno);
		heap_mask(mirrorFileBuf + blockno * BLCKSZ, blockno);

		memcpy(maskedPrimaryBuf, primaryFileBuf + blockno * BLCKSZ, BLCKSZ);
		memcpy(maskedMirrorBuf, mirrorFileBuf + blockno * BLCKSZ, BLCKSZ);

		pg_md5_hash(maskedPrimaryBuf, BLCKSZ, primaryfilechecksum);
		pg_md5_hash(maskedMirrorBuf, BLCKSZ, mirrorfilechecksum);

		if (strcmp(primaryfilechecksum, mirrorfilechecksum) != 0)
			return false;

		blockno += 1;
	}

	return true;
}

PG_FUNCTION_INFO_V1(gp_replica_check);

Datum
gp_replica_check(PG_FUNCTION_ARGS)
{

	List  *fslist = NIL;
	int 			proc;
	int 			ret;
	StringInfoData 	sqlstmt;
	MemoryContext 	oldcontext = CurrentMemoryContext;

	if (!gp_segment_config_has_mirrors())
	{
		elog(NOTICE, "No mirrors exist in gp_segment_configuration. Nothing to verify.");
		PG_RETURN_BOOL(false);
	}

	appendStringInfo(&sqlstmt,
					 "SELECT c.content, c.role, c.mode, f.fselocation "
					 "FROM gp_segment_configuration c, pg_filespace_entry f "
					 "WHERE c.dbid = f.fsedbid and c.content != -1 "
					 "ORDER by c.content");

	if (SPI_OK_CONNECT != SPI_connect())
	{
		ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
						errmsg("Write me later."),
						errdetail("SPI_connect failed in gp_replica_check")));
	}

	ret = SPI_execute(sqlstmt.data, false, 0);
	proc = SPI_processed;

	if (ret > 0 && SPI_tuptable != NULL)
	{
		TupleDesc tupdesc = SPI_tuptable->tupdesc;
		SPITupleTable *tuptable = SPI_tuptable;
		int i;

		/* We should not have any primary or mirror without a peer */
		Assert(proc %2 == 0);

		/*
		 * Iterate through each result tuple
		 */
		for (i = 0; i < proc; i+=2)
		{
			fspair *map = (fspair *) malloc(sizeof(fspair));
			HeapTuple 	tuple1 = tuptable->vals[i];
			HeapTuple 	tuple2 = tuptable->vals[i+1];
			char* role;
			char* mode;
			MemoryContext cxt_save;

			/* use our own context so that SPI won't free our stuff later */
			cxt_save = MemoryContextSwitchTo(oldcontext);

			/* Assert that the content ids are equal */
			Assert(*(SPI_getvalue(tuple1, tupdesc, 1)) == *(SPI_getvalue(tuple2, tupdesc, 1)));

			role = SPI_getvalue(tuple1, tupdesc, 2);
			mode = SPI_getvalue(tuple1, tupdesc, 3);

			if (*mode != 's')
			{
				elog(ERROR, "Primary and its mirror are not in sync.");
				PG_RETURN_BOOL(false);
			}

			if (*role == 'p')
			{
				map->primaryfslocation = pstrdup(SPI_getvalue(tuple1, tupdesc, 4));
				map->mirrorfslocation = pstrdup(SPI_getvalue(tuple2, tupdesc, 4));
			}
			else
			{
				map->primaryfslocation = pstrdup(SPI_getvalue(tuple2, tupdesc, 4));
				map->mirrorfslocation = pstrdup(SPI_getvalue(tuple1, tupdesc, 4));
			}

			fslist = lappend(fslist, map);

			MemoryContextSwitchTo(cxt_save);
		}
	}

	SPI_finish();
	pfree(sqlstmt.data);

	ListCell *l = NULL;
	foreach(l, fslist)
	{
		fspair *t = (fspair *)lfirst(l);
		elog(NOTICE, "Primary: %s | Mirror: %s", t->primaryfslocation, t->mirrorfslocation);

		// Go after the base/../[0-9]+ and global/[0-9]+
		char primaryfilepath[2000];
		char mirrorfilepath[2000];

		sprintf(primaryfilepath, "%s/base/16404", t->primaryfslocation);
		sprintf(mirrorfilepath, "%s/base/16404", t->mirrorfslocation);

		DIR *primarydir = AllocateDir(primaryfilepath);

		struct dirent *dent = NULL;
		bool equivalent = true;

		while ((dent = ReadDir(primarydir, primaryfilepath)) != NULL)
		{
			char primaryfilename[2000] = {'\0'};
			char primarychecksum[33] = {0};
			bool eq = false;
			sprintf(primaryfilename, "%s/%s", primaryfilepath, dent->d_name);

			char mirrorfilename[2000] = {'\0'};
			char mirrorchecksum[33] = {0};
			sprintf(mirrorfilename, "%s/%s", mirrorfilepath, dent->d_name);

			eq = compare_files(primaryfilename, mirrorfilename);

			if (!eq)
				elog(NOTICE, "Files %s and %s differ", primaryfilename, mirrorfilename);
			equivalent &= eq;
		}

		elog(NOTICE, "equivalent? %d", equivalent);

		FreeDir(primarydir);
	}

	PG_RETURN_BOOL(true);
}
