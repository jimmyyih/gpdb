/*-------------------------------------------------------------------------
 *
 * thanos.h
 *	  Interface for Thanos
 *
 *-------------------------------------------------------------------------
 */
#ifndef THANOS_H
#define THANOS_H

#include "utils/guc.h"
#include "cdb/cdbutil.h"

extern bool am_thanos;

/*
 * ENUMS
 */

enum infinity_stones
{
	NONE = 0,
	POWER_STONE,
	SPACE_STONE,
	REALITY_STONE,
	SOUL_STONE,
	TIME_STONE,
	MIND_STONE
};

/*
 * Interface for checking if FTS is active
 */
extern bool ThanosIsActive(void);

extern bool ThanosStartRule(Datum main_arg);
extern void ThanosMain (Datum main_arg);
extern void ThanosShmemInit(void);
extern pid_t ThanosPID(void);

#endif   /* THANOS_H */
