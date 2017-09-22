#include "postgres.h"
#include "fmgr.h"
#include "libpq/md5.h"
#include "utils/builtins.h"
#include "executor/spi.h"
#include "catalog/gp_segment_config.h"

PG_MODULE_MAGIC;

static bool calculate_checksum(char* filepath, char* md5);

typedef struct fspair
{
	char* primaryfslocation;
	char* mirrorfslocation;
} fspair;

bool
calculate_checksum(char* filepath, char* md5)
{
	File file = 0;
	char buf[BLCKSZ * 32];
	int  bytesRead;

	elog(LOG, "filepath is %s", filepath);
	file = PathNameOpenFile(filepath, O_RDONLY | PG_BINARY, S_IRUSR);

	if (file < 0)
		return false;

	bytesRead = FileRead(file, buf, sizeof(buf));
	if (bytesRead >= 0)
		pg_md5_hash(buf, bytesRead, md5);

	FileClose(file);
	return true;
}

/* get_filespace_locations(int content, char *primary_loc, char *mirror_loc) */
/* { */
/* 	// get the dbids corresponding to the content For e.g content 0 -> dbids 1, 4 */
/*     // Lookup filespace_entry for the dbids */
/*     // Return the fselocation from filespace entry as the filespace locations */
/* } */

/* static void */
/* get_filespace_map() */
/* { */
	
/* } */

PG_FUNCTION_INFO_V1(gp_replica_check);

Datum
gp_replica_check(PG_FUNCTION_ARGS)
{
//	elog(NOTICE, "Inside gp_replica_check");

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

	/* Using SPI, get the necessary information */

	/*
	 * assemble our query string
	 */
	initStringInfo(&sqlstmt);

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

	/* Do the query. */
	ret = SPI_execute(sqlstmt.data, false, 0);
	proc = SPI_processed;

	elog(NOTICE, "proc is %d", proc);
	elog(NOTICE, "ret is %d", ret);

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

			elog(NOTICE, "Inside loop 1 -- Primary: %s | Mirror: %s", map->primaryfslocation, map->mirrorfslocation);
			fslist = lappend(fslist, map);

			MemoryContextSwitchTo(cxt_save);
		}
	}

	SPI_finish();
	pfree(sqlstmt.data);

	elog(NOTICE, "the fslist length is %d", list_length(fslist));
	ListCell *l = NULL;
	foreach(l, fslist)
	{
		fspair *t = (fspair *)lfirst(l);
		elog(NOTICE, "Primary: %s | Mirror: %s", t->primaryfslocation, t->mirrorfslocation);

		/* Go after the base/../[0-9]+ and global/[0-9]+*/
		char primaryfilepath[2000];
		char mirrorfilepath[2000];

		sprintf(primaryfilepath, "%s/global", t->primaryfslocation);
		sprintf(mirrorfilepath, "%s/global", t->mirrorfslocation);

		DIR *primarydir = AllocateDir(primaryfilepath);
		/* DIR *mirrordir = AllocateDir(mirrorfilepath); */

		struct dirent *dent = NULL;
		bool equivalent = true;

		while ((dent = ReadDir(primarydir, primaryfilepath)) != NULL)
		{
			char primaryfilename[2000] = {'\0'};
			char primarychecksum[33] = {0};
			bool eq = false;
			sprintf(primaryfilename, "%s/%s", primaryfilepath, dent->d_name);
			calculate_checksum(primaryfilename, primarychecksum);
			elog(NOTICE, "primary checksum for file %s is %s", primaryfilename, primarychecksum);

			char mirrorfilename[2000] = {'\0'};
			char mirrorchecksum[33] = {0};
			sprintf(mirrorfilename, "%s/%s", mirrorfilepath, dent->d_name);
			calculate_checksum(mirrorfilename, mirrorchecksum);
			elog(NOTICE, "mirror checksum for file %s is %s", mirrorfilename, mirrorchecksum);

			eq = (strcmp(primarychecksum, mirrorchecksum) == 0);
			if (!eq)
				elog(NOTICE, "Files %s and %s differ", primaryfilename, mirrorfilename);
			equivalent &= eq;
		}

		elog(NOTICE, "equivalent? %d", equivalent);

		FreeDir(primarydir);


		/* while ((dent = ReadDir(dir, mirrorfilepath)) != NULL) */
		/* { */
 		/* 	char mirrorfilename[2000] = {'\0'}; */
		/* 	char mirrorchecksum[33] = {0}; */
		/* 	sprintf(mirrorfilename, "%s/%s", mirrorfilepath, dent->d_name); */
		/* 	calculate_checksum(mirrorfilename, mirrorchecksum); */
		/* 	elog(NOTICE, "mirror checksum for file %s is %s", mirrorfilename, mirrorchecksum); */
		/* } */
		/* FreeDir(dir); */


	}


	/* char* primarybasedir = "/Users/jyih/gitdir/github/gpdb/gpAux/gpdemo/datadirs/dbfast1/demoDataDir0/base/12086/"; */
	/* char* mirrorbasedir = "/Users/jyih/gitdir/github/gpdb/gpAux/gpdemo/datadirs/dbfast_mirror1/demoDataDir0/base/12086/"; */

	/* char relfilenode[64]; */

	/* snprintf(relfilenode, sizeof(relfilenode), "%d", rd_rel->relfilenode); */

	/* char primaryfilepath[2000]; */
	/* char mirrorfilepath[2000]; */

	/* strcpy(primaryfilepath, primarybasedir); */
	/* strcat(primaryfilepath, relfilenode); */

	/* strcpy(mirrorfilepath, mirrorbasedir); */
	/* strcat(mirrorfilepath, relfilenode); */

	/* char primarychecksum[33] = {0}; */
	/* char mirrorchecksum[33] = {0}; */

	/* bool presult = calculate_checksum(primaryfilepath, primarychecksum); */
	/* bool mresult = calculate_checksum(mirrorfilepath, mirrorchecksum); */

	/* if (!presult || !mresult) */
	/* 	continue; */
	/* else */
	/* { */
	/* 	elog(LOG, "primarychecksum: %s | mirrorchecksum: %s", primarychecksum, mirrorchecksum); */
	/* 	i++; */
	/* } */

	PG_RETURN_BOOL(true);
}
