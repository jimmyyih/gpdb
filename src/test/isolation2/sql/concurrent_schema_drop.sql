-- Test that dropping a new empty schema while concurrently creating
-- objects in that schema blocks the DROP SCHEMA. Each DROP SCHEMA in
-- this test should block trying to obtain AccessExclusiveLock on its
-- corresponding pg_namespace entry.

CREATE SCHEMA concurrent_schema_drop1;
CREATE SCHEMA concurrent_schema_drop2;
CREATE SCHEMA concurrent_schema_drop3;
CREATE SCHEMA concurrent_schema_drop4;
CREATE SCHEMA concurrent_schema_drop5;
CREATE SCHEMA concurrent_schema_drop6;

-- Test on CREATE TABLE
1: BEGIN;
1: CREATE TABLE concurrent_schema_drop1.concurrent_schema_drop1_table(a int);
2&: DROP SCHEMA concurrent_schema_drop1;
1: COMMIT;
2<:
1: SELECT * FROM concurrent_schema_drop1.concurrent_schema_drop1_table;

-- Test on CREATE TYPE
1: BEGIN;
1: CREATE TYPE concurrent_schema_drop2.concurrent_schema_drop2_type;
2&: DROP SCHEMA concurrent_schema_drop2;
1: COMMIT;
2<:
1: SELECT typname FROM pg_type WHERE typname = 'concurrent_schema_drop2_type';

1: BEGIN;
1: CREATE TYPE concurrent_schema_drop3.concurrent_schema_drop3_composite_type
       AS (r double precision, i double precision);
2&: DROP SCHEMA concurrent_schema_drop3;
1: COMMIT;
2<:
1: SELECT typname FROM pg_type WHERE typname = 'concurrent_schema_drop3_composite_type';

-- Test on CREATE VIEW
1: BEGIN;
1: CREATE VIEW concurrent_schema_drop4.concurrent_schema_drop4_view AS SELECT 1;
2&: DROP SCHEMA concurrent_schema_drop4;
1: COMMIT;
2<:
1: SELECT * FROM concurrent_schema_drop4.concurrent_schema_drop4_view;

-- Test on CREATE FUNCTION
1: BEGIN;
1: CREATE FUNCTION concurrent_schema_drop5.concurrent_schema_drop5_func() RETURNS bool LANGUAGE SQL AS 'SELECT true';
2&: DROP SCHEMA concurrent_schema_drop5;
1: COMMIT;
2<:
1: SELECT concurrent_schema_drop5.concurrent_schema_drop5_func();

-- Test on ALTER TABLE .. SET SCHEMA
1: CREATE TABLE concurrent_schema_drop6_table(a int);
1: BEGIN;
1: ALTER TABLE concurrent_schema_drop6_table SET SCHEMA concurrent_schema_drop6;
2&: DROP SCHEMA concurrent_schema_drop6;
1: COMMIT;
2<:
1: SELECT * FROM concurrent_schema_drop6.concurrent_schema_drop6_table;
