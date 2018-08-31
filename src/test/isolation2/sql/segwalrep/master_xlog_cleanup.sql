-- Test for verifying if xlog seg created while basebackup
-- dumps out data does not get cleaned

CREATE EXTENSION IF NOT EXISTS gp_inject_fault;

-- start_ignore
CREATE LANGUAGE plpythonu;
-- end_ignore
CREATE OR REPLACE FUNCTION pg_basebackup()
RETURNS text AS $$
    import shutil
    import subprocess
    shutil.rmtree('/tmp/master_xlog_cleanup_test', True)
    cmd = 'pg_basebackup -x -R -D /tmp/master_xlog_cleanup_test'
    return subprocess.check_output(cmd, stderr=subprocess.STDOUT, shell=True)
$$ LANGUAGE plpythonu;

-- Inject fault after checkpoint creation in basebackup
SELECT gp_inject_fault('base_backup_post_create_checkpoint', 'suspend', 1);

-- Run pg_basebackup which should trigger and suspend at the fault
1&: SELECT pg_basebackup();

-- Wait until fault has been triggered
SELECT gp_wait_until_triggered_fault('base_backup_post_create_checkpoint', 1, 1);

-- See that pg_basebackup is still running
SELECT application_name, state FROM pg_stat_replication;

-- Perform operations that causes xlog seg generation
SELECT 1 FROM pg_switch_xlog();
SELECT 1 FROM pg_switch_xlog();
CHECKPOINT;
SELECT 1 FROM pg_switch_xlog();
SELECT 1 FROM pg_switch_xlog();
CHECKPOINT;
SELECT 1 FROM pg_switch_xlog();
SELECT 1 FROM pg_switch_xlog();
CHECKPOINT;
SELECT 1 FROM pg_switch_xlog();
SELECT 1 FROM pg_switch_xlog();
CHECKPOINT;
SELECT 1 FROM pg_switch_xlog();
SELECT 1 FROM pg_switch_xlog();
CHECKPOINT;
SELECT 1 FROM pg_switch_xlog();
SELECT 1 FROM pg_switch_xlog();
CHECKPOINT;
SELECT 1 FROM pg_switch_xlog();
SELECT 1 FROM pg_switch_xlog();
CHECKPOINT;
SELECT 1 FROM pg_switch_xlog();
SELECT 1 FROM pg_switch_xlog();
CHECKPOINT;
SELECT 1 FROM pg_switch_xlog();
SELECT 1 FROM pg_switch_xlog();
CHECKPOINT;
SELECT 1 FROM pg_switch_xlog();
SELECT 1 FROM pg_switch_xlog();
CHECKPOINT;

-- Resume basebackup
SELECT gp_inject_fault('base_backup_post_create_checkpoint', 'reset', 1);

-- Wait until basebackup finishes
1<:

-- Verify if basebackup completed successfully
-- See if recovery.conf exists (Yes - Pass)
SELECT application_name, state FROM pg_stat_replication;
!\retcode ls /tmp/master_xlog_cleanup_test/recovery.conf;
