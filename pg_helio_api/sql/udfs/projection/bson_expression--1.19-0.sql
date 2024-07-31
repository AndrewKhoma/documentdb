
CREATE OR REPLACE FUNCTION __API_CATALOG_SCHEMA__.bson_expression_get(document __CORE_SCHEMA__.bson, expressionSpec __CORE_SCHEMA__.bson, isNullOnEmpty bool default false)
 RETURNS __CORE_SCHEMA__.bson
 LANGUAGE c
 IMMUTABLE PARALLEL SAFE STRICT
AS 'MODULE_PATHNAME', $function$bson_expression_get$function$;

CREATE OR REPLACE FUNCTION helio_api_internal.bson_expression_get(document helio_core.bson, expressionSpec helio_core.bson, isNullOnEmpty bool, variableSpec helio_core.bson)
 RETURNS helio_core.bson
 LANGUAGE c
 IMMUTABLE PARALLEL SAFE STRICT
AS 'MODULE_PATHNAME', $function$bson_expression_get$function$;


CREATE OR REPLACE FUNCTION __API_CATALOG_SCHEMA__.bson_expression_map(document __CORE_SCHEMA__.bson, sourceArrayName text, expressionSpec __CORE_SCHEMA__.bson, isNullOnEmpty bool default false)
 RETURNS __CORE_SCHEMA__.bson
 LANGUAGE c
 IMMUTABLE PARALLEL SAFE STRICT
AS 'MODULE_PATHNAME', $function$bson_expression_map$function$;

CREATE OR REPLACE FUNCTION helio_api_internal.bson_expression_map(document helio_core.bson, sourceArrayName text, expressionSpec helio_core.bson, isNullOnEmpty bool, variableSpec helio_core.bson)
 RETURNS helio_core.bson
 LANGUAGE c
 IMMUTABLE PARALLEL SAFE STRICT
AS 'MODULE_PATHNAME', $function$bson_expression_map$function$;

CREATE OR REPLACE FUNCTION helio_api_internal.bson_expression_partition_get(document __CORE_SCHEMA__.bson, expressionSpec __CORE_SCHEMA__.bson, isNullOnEmpty bool default false)
 RETURNS __CORE_SCHEMA__.bson
 LANGUAGE c
 IMMUTABLE PARALLEL SAFE STRICT
AS 'MODULE_PATHNAME', $function$bson_expression_partition_get$function$;

CREATE OR REPLACE FUNCTION helio_api_internal.bson_expression_partition_get(document helio_core.bson, expressionSpec helio_core.bson, isNullOnEmpty bool, variableSpec helio_core.bson)
 RETURNS helio_core.bson
 LANGUAGE c
 IMMUTABLE PARALLEL SAFE STRICT
AS 'MODULE_PATHNAME', $function$bson_expression_partition_get$function$;
