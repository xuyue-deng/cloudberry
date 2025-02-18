-- Test this scenario:
-- mirror has latency replaying the WAL from the primary, the master is reset
-- from PANIC, master will start the DTX recovery process to recover the
-- in-progress two-phase transactions. 
-- The FTS process should be able to continue probe and 'sync off' the mirror
-- while the 'dtx recovery' process is hanging recovering distributed transactions.

1: create table t_wait_lsn(a int);
5: create table t_wait_lsn2(a int);

-- suspend segment 0 before performing 'COMMIT PREPARED'
2: select gp_inject_fault_infinite('finish_prepared_start_of_function', 'suspend', dbid) from gp_segment_configuration where content=0 and role='p';
1&: insert into t_wait_lsn values(2),(1);
5&: insert into t_wait_lsn2 values(2),(1);
2: select gp_wait_until_triggered_fault('finish_prepared_start_of_function', 2, dbid) from gp_segment_configuration where content=0 and role='p';

-- let walreceiver on mirror 0 skip WAL flush
2: select gp_inject_fault_infinite('walrecv_skip_flush', 'skip', dbid) from gp_segment_configuration where content=0 and role='m';
-- resume 'COMMIT PREPARED', session 1 will hang on 'SyncRepWaitForLSN'
2: select gp_inject_fault_infinite('finish_prepared_start_of_function', 'reset', dbid) from gp_segment_configuration where content=0 and role='p';

0U: select count(*) from pg_prepared_xacts;

-- stop mirror
3: SELECT pg_ctl(datadir, 'stop', 'immediate') FROM gp_segment_configuration WHERE content=0 AND role = 'm';
!\retcode gpfts -R 1 -A -D;
-- trigger master reset
3: select gp_inject_fault('exec_simple_query_start', 'panic', current_setting('gp_dbid')::smallint);
-- verify master panic happens. The PANIC message does not emit sometimes so
-- mask it.
-- start_matchsubs
-- m/PANIC:  fault triggered, fault name:'exec_simple_query_start' fault type:'panic'\n/
-- s/PANIC:  fault triggered, fault name:'exec_simple_query_start' fault type:'panic'\n//
-- end_matchsubs
3: select 1;

-- potential flakiness: there is a chance where the coordinator
-- recovers fast enough (from the panic above) that we end up fault injecting too late.
-1U: select gp_inject_fault_infinite('post_progress_recovery_comitted', 'suspend', dbid) FROM gp_segment_configuration WHERE content=-1 AND role='p';
-1U: select gp_wait_until_triggered_fault('post_progress_recovery_comitted', 1, dbid) from gp_segment_configuration where content=-1 and role='p';
-1U: select * from gp_stat_progress_dtx_recovery;
-1U: select gp_inject_fault_infinite('post_progress_recovery_comitted', 'reset', dbid) from gp_segment_configuration where content=-1 and role='p';

-- wait for coordinator finish crash recovery
-1U: select wait_until_standby_in_state('streaming');

-- wait for FTS to 'sync off' the mirror, meanwhile, dtx recovery process will
-- restart repeatedly.
-- the query should succeed finally since dtx recovery process is able to quit.
-- this's what we want to test.
4: select count(*) from t_wait_lsn;
1<:
5<:

!\retcode gpfts -R 1 -A -D;
!\retcode gprecoverseg -a;
!\retcode gpfts -R 1 -A -D;
-- loop while segments come in sync
4: select wait_until_all_segments_synchronized();
4: select pg_sleep(10);
4: select count(*) from t_wait_lsn;
4: drop table t_wait_lsn;
4: drop table t_wait_lsn2;

4: select gp_inject_fault('walrecv_skip_flush', 'reset', dbid) from gp_segment_configuration where content=0;
