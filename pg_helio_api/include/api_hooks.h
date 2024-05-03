/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * include/api_hooks.h
 *
 * Exports related to hooks for the public API surface that enable distribution.
 *
 *-------------------------------------------------------------------------
 */

#ifndef EXTENSION_API_HOOKS_H
#define EXTENSION_API_HOOKS_H

#include "api_hooks_common.h"
#include "metadata/collection.h"

/* Section: General Extension points */


/*
 * Returns true if the current Postgres server is a Query Coordinator
 * that also owns the metadata management of schema (DDL).
 */
bool IsMetadataCoordinator(void);


/*
 * Runs a command on the MetadataCoordinator if the current node is not a
 * Metadata Coordinator. The response is returned as a "record" struct
 * with the nodeId responding, whether or not the command succeeded and
 * the response datum serialized as a string.
 * If success, then this is the response datum in text format.
 * If failed, then this contains the error string from the failure.
 */
DistributedRunCommandResult RunCommandOnMetadataCoordinator(const char *query);

/*
 * Runs a query via SPI with commutative writes on for distributed scenarios.
 * Returns the Datum returned by the executed query.
 */
Datum RunQueryWithCommutativeWrites(const char *query, int nargs, Oid *argTypes,
									Datum *argValues, char *argNulls,
									int expectedSPIOK, bool *isNull);


/*
 * Sets up the system to allow nested distributed query execution for the current
 * transaction scope.
 * Note: This should be used very cautiously in any place where data correctness is
 * required.
 */
void RunMultiValueQueryWithNestedDistribution(const char *query, int nargs, Oid *argTypes,
											  Datum *argValues, char *argNulls, bool
											  readOnly,
											  int expectedSPIOK, Datum *datums,
											  bool *isNull, int numValues);


/*
 * Whether or not the the base tables have sharding with distribution (true if DistributePostgreTable
 * is run).
 * the documents table name and the substring where the collectionId was found is provided as an input.
 */
bool IsShardTableForMongoTable(const char *relName, const char *numEndPointer);


/* Section: Create Table Extension points */

/*
 * Distributes a given postgres table with the provided distribution column.
 * Optionally supports colocating the distributed table with another distributed table.
 */
void DistributePostgresTable(const char *postgresTable, const char *distributionColumn,
							 const char *colocateWith, bool isUnsharded);

/*
 * Given a current table schema built up to create a postgres table, adds a hook to
 * modify the schema if needed.
 */
void ModifyCreateTableSchema(StringInfo currentSchema, const char *tableName);


/*
 * Handle any post actions after the table is created
 */
void PostProcessCreateTable(const char *postgresTable, uint64_t collectionId,
							text *databaseName, text *collectionName);

/*
 * Handle any post actions after the table is sharded
 */
void PostProcessShardCollection(const char *tableName, uint64_t collectionId,
								text *databaseName, text *collectionName,
								pgbson *shardKey);

/*
 * Handle any post actions after the collection is dropped
 */
void PostProcessCollectionDrop(uint64_t collectionId, text *databaseName,
							   text *collectionName, bool trackChanges);

/*
 * Entrypoint to modify a list of column names for queries
 * For a base RTE (table)
 */
List * ModifyTableColumnNames(List *tableColumns);

/*
 * Hook for handling colocation of tables
 */
void HandleColocation(MongoCollection *collection, const bson_value_t *colocationOptions);


/*
 * Mutate's listCollections query generation for distribution data.
 * This is an optional hook and can manage listCollection to update shardCount
 * and colocation information as required. Noops for single node.
 */
Query * MutateListCollectionsQueryForDistribution(Query *cosmosMetadataQuery);


/*
 * Given a table OID, if the table is not the actual physical shard holding the data (say in a
 * distributed setup), tries to return the full shard name of the actual table if it can be found locally
 * or NULL otherwise (e.g. for ApiDataSchema.documents_1 returns ApiDataSchema.documents_1_12341 or NULL)
 */
const char * TryGetShardNameForUnshardedCollection(Oid relationOid, uint64 collectionId,
												   const char *tableName);

const char * GetDistributedApplicationName(void);

#endif
