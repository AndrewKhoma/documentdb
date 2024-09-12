/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/helio_distributed.c
 *
 * Initialization of the shared library.
 *-------------------------------------------------------------------------
 */
#include <postgres.h>
#include <fmgr.h>
#include <miscadmin.h>
#include <bson.h>
#include <access/xact.h>
#include <utils/version_utils.h>
#include "distributed_hooks.h"

PG_MODULE_MAGIC;

void _PG_init(void);
void _PG_fini(void);


/*
 * _PG_init gets called when the extension is loaded.
 */
void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
	{
		ereport(ERROR, (errmsg(
							"pg_helio_distributed can only be loaded via shared_preload_libraries. "
							"Add pg_helio_distributed to shared_preload_libraries configuration "
							"variable in postgresql.conf in coordinator and workers. "
							"Note that pg_helio_distributed should be placed right after citus and pg_helio_api.")));
	}

	InitializeHelioDistributedHooks();
}


/*
 * _PG_fini is called before the extension is reloaded.
 */
void
_PG_fini(void)
{ }
