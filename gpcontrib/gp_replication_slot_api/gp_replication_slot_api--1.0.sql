-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION gp_replication_slot_api" to load this file. \quit

CREATE FUNCTION update_replication_slot_location(
  replication_slot_name text,
  lsn pg_lsn)
RETURNS VOID
AS 'MODULE_PATHNAME'
LANGUAGE C;
