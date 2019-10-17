/*-------------------------------------------------------------------------
 *
 * thanos.c
 *	  Brings balance to Greenplum.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

/* These are always necessary for a bgworker */
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "pgstat.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"

#include "catalog/dependency.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "libpq/pqsignal.h"
#include "cdb/cdbvars.h"
#include "libpq-int.h"
#include "postmaster/thanos.h"
#include "postmaster/postmaster.h"
#include "utils/builtins.h"
#include "utils/faultinjector.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/relcache.h"

#include "catalog/gp_configuration_history.h"
#include "catalog/gp_segment_config.h"

#include "tcop/tcopprot.h" /* quickdie() */

bool am_thanos = false;
/*
 * STATIC VARIABLES
 */
static volatile pid_t *shmThanosPID;
static volatile int *infinity_stones_acquired = 0;
static volatile sig_atomic_t got_SIGHUP = false;

/*
 * FUNCTION PROTOTYPES
 */
static void ThanosLoop(void);


/*=========================================================================
 * HELPER FUNCTIONS
 */
/* SIGHUP: set flag to reload config file */
static void
sigHupHandler(SIGNAL_ARGS)
{
	got_SIGHUP = true;

	if (MyProc)
		SetLatch(&MyProc->procLatch);
}

/* SIGINT: set flag to indicate a Thanos increment is requested? */
static void
sigIntHandler(SIGNAL_ARGS)
{
	if (MyProc)
		SetLatch(&MyProc->procLatch);
}

void
ThanosShmemInit(void)
{
	if (IsUnderPostmaster)
		return;

	shmThanosPID = (volatile pid_t*)ShmemAlloc(sizeof(*shmThanosPID));
	*shmThanosPID = 0;

	infinity_stones_acquired = (volatile int*)ShmemAlloc(sizeof(int));
	*infinity_stones_acquired = 0;
}

pid_t
ThanosPID(void)
{
	return *shmThanosPID;
}

bool
ThanosStartRule(Datum main_arg)
{
	if (IsUnderMasterDispatchMode())
		return true;

	return false;
}

/*
 * FtsProbeMain
 */
void
ThanosMain(Datum main_arg)
{
	*shmThanosPID = MyProcPid;
	am_thanos = true;

	/*
	 * reread postgresql.conf if requested
	 */
	pqsignal(SIGHUP, sigHupHandler);
	pqsignal(SIGINT, sigIntHandler);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/* Connect to our database */
	BackgroundWorkerInitializeConnection(DB_FOR_COMMON_ACCESS, NULL);

	/* main loop */
	ThanosLoop();

	/* One iteration done, go away */
	proc_exit(0);
}

static
void ThanosLoop()
{
	MemoryContext thanosContext = NULL;
	//MemoryContext oldContext = NULL;
	thanosContext = AllocSetContextCreate(TopMemoryContext,
										 "ThanosMemCtxt",
										 ALLOCSET_DEFAULT_INITSIZE,	/* always have some memory */
										 ALLOCSET_DEFAULT_INITSIZE,
										 ALLOCSET_DEFAULT_MAXSIZE);

	bool do_snap = false;
	while (true)
	{
		int			rc;

		if (got_SIGHUP)
		{
			got_SIGHUP = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		CHECK_FOR_INTERRUPTS();

		/* Once Thanos has acquired the Mind Stone, initiate snap */
		if (*infinity_stones_acquired == MIND_STONE)
		{
			do_snap = true;
			*infinity_stones_acquired = 0;
		}

		if (do_snap)
		{
			elog(LOG, "Thanos Snap has been executed.");

			/* Need a transaction to access the catalogs */
			StartTransactionCommand();

			/* Terminate half the queries */
			List *query_pids = pg_stat_give_half_active_pids();
			ListCell *cell;
			foreach(cell, query_pids)
			{
				int pid2term = lfirst_int(cell);
				DirectFunctionCall2(pg_terminate_backend_msg,
									Int32GetDatum(pid2term),
									CStringGetTextDatum("This universe is finite, its resources, finite. If life is left unchecked, life will cease to exist. It needs correcting."));
			}

			/* Terminate half the user tables */
			List	   *relations_list = NIL;
			ScanKeyData key[2];
			Relation	rel;
			HeapScanDesc scan;
			HeapTuple	tuple;

			ScanKeyInit(&key[0],
						Anum_pg_class_relnamespace,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(2200));
			ScanKeyInit(&key[1],
						Anum_pg_class_relkind,
						BTEqualStrategyNumber, F_CHAREQ,
						CharGetDatum('r'));

			rel = heap_open(RelationRelationId, AccessShareLock);
			scan = heap_beginscan_catalog(rel, 2, key);
			while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
			{
				relations_list = lappend_oid(relations_list, HeapTupleGetOid(tuple));
			}

			heap_endscan(scan);
			heap_close(rel, AccessShareLock);

			List *random_reloid_list = NIL;
			if (relations_list != NIL)
			{
				for (int i = relations_list->length - 1; i >= 0; i--)
				{
					int tmp = list_nth_oid(relations_list, i);
					int rand_idx = random() % (i+1);
					list_nth_replace_oid(relations_list, i, list_nth_oid(relations_list, rand_idx));
					list_nth_replace_oid(relations_list, rand_idx, tmp);
				}

				for(int i = 0; i < (relations_list->length/2) + 1; i++){
					random_reloid_list = lappend_oid(random_reloid_list, list_nth_oid(relations_list, i));
				}
			}

			ObjectAddresses *objects;
			objects = new_object_addresses();
			foreach(cell, random_reloid_list)
			{
				ObjectAddress obj;
				obj.classId = RelationRelationId;
				obj.objectId = lfirst_oid(cell);
				obj.objectSubId = 0;
				add_exact_object_address(&obj, objects);
			}

			performMultipleDeletions(objects, DROP_RESTRICT, 0);

			/* close the transaction we started above */
			CommitTransactionCommand();

			elog(LOG, "Your GPDB cluster was on the brink of collapse. I, Thanos, am the one who stopped that. You know what's happened since then? The end-users have known nothing but fast queries and plentiful disk space.");
			do_snap = false;
		}

		rc = WaitLatch(&MyProc->procLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   5 * 1000L);

		ResetLatch(&MyProc->procLatch);

		/* emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);
	}

	return;
}

/*
 * Check if Thanos is active
 */
bool
ThanosIsActive(void)
{
	return true;
}

Datum
gp_acquire_stone(PG_FUNCTION_ARGS)
{
	if (Gp_role != GP_ROLE_DISPATCH)
	{
		ereport(ERROR,
				(errmsg("this function can only be called by master (without utility mode)")));
		PG_RETURN_BOOL(false);
	}

	(*infinity_stones_acquired)++;

	switch(*infinity_stones_acquired)
	{
		case MIND_STONE:
			elog(LOG, "Thanos has acquired the Mind Stone");
			break;
		case POWER_STONE:
			elog(LOG, "Thanos has acquired the Power Stone");
			break;
		case REALITY_STONE:
			elog(LOG, "Thanos has acquired the Reality Stone");
			break;
		case SOUL_STONE:
			elog(LOG, "Thanos has acquired the Soul Stone");
			break;
		case SPACE_STONE:
			elog(LOG, "Thanos has acquired the Space Stone");
			break;
		case TIME_STONE:
			elog(LOG, "Thanos has acquired the Time Stone");
			break;
		default:
			elog(LOG, "Current stone at %d", *infinity_stones_acquired);
			break;
	}

	PG_RETURN_BOOL(true);
}
