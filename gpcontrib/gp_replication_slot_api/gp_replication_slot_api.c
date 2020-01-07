#include "postgres.h"

#include "replication/slot.h"
#include "access/xlog.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"

PG_MODULE_MAGIC;

Datum update_replication_slot_location(PG_FUNCTION_ARGS);
void _PG_init(void);

void
_PG_init(void)
{
}

/*
 * Remember that a walreceiver just confirmed receipt of lsn `lsn`.
 */
PG_FUNCTION_INFO_V1(update_replication_slot_location);
Datum
update_replication_slot_location(PG_FUNCTION_ARGS)
{
	bool		changed = false;
	ReplicationSlot *slot = NULL;
	XLogRecPtr lsn = PG_GETARG_LSN(1);

	ReplicationSlotAcquire(text_to_cstring(PG_GETARG_TEXT_P(0)));

	slot = MyReplicationSlot;

	Assert(lsn != InvalidXLogRecPtr);
	SpinLockAcquire(&slot->mutex);
	if (slot->data.restart_lsn != lsn)
	{
		changed = true;
		slot->data.restart_lsn = lsn;
	}
	SpinLockRelease(&slot->mutex);

	if (changed)
	{
		ReplicationSlotMarkDirty();
		ReplicationSlotsComputeRequiredLSN();
	}

	ReplicationSlotRelease();

	PG_RETURN_VOID();
}
