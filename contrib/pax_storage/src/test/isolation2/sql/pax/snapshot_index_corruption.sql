-- @Description Test index corruption when invalid snapshot used.
--
-- Create AO table, insert few rows on it.
drop table if exists test_pax;
create table test_pax(i bigint) distributed by (i);
insert into test_pax select generate_series(1,100);
-- Test 1
-- Begin single-insert transaction.
1: begin;
1: insert into test_pax values(101);
-- Try to create index, it should hold on lock before commit below.
2&: create index test_pax_idx on test_pax(i);
-- Commit single-insert transaction, so index continues creation.
1: commit;
-- Force index usage and check row is here (false before fix).
2<:
2: set optimizer=off;
2: set enable_seqscan=off;
2: explain (costs off) select i from test_pax where i = 101;
2: select i from test_pax where i = 101;

-- Test 2
-- Drop incomplete index
1: drop index test_pax_idx;
-- Check row is here and start repeatable read transaction.
2: select i from test_pax where i = 100;
2: begin;
2: set transaction isolation level repeatable read;
2: select 1;
-- Update row selected above and create new index
1: update test_pax set i = 200 where i = 100;
1: create index test_pax_idx on test_pax(i);
-- For the repeatable read isolation level row still there.
2: explain (costs off) select i from test_pax where i = 100;
-- FIXME: The result of PAX is different with AO
2: select i from test_pax where i = 100;
