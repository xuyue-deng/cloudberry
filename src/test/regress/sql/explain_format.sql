-- start_matchsubs
-- m/\(actual time=\d+\.\d+..\d+\.\d+ rows=\d+ loops=\d+\)/
-- s/\(actual time=\d+\.\d+..\d+\.\d+ rows=\d+ loops=\d+\)/(actual time=##.###..##.### rows=# loops=#)/
-- m/\(slice\d+\)    Executor memory: (\d+)\w bytes\./
-- s/Executor memory: (\d+)\w bytes\./Executor memory: (#####)K bytes./
-- m/\(slice\d+\)    Executor memory: (\d+)\w bytes avg x \d+(x\(\d+\))* workers, \d+\w bytes max \(seg\d+\)\./
-- s/Executor memory: (\d+)\w bytes avg x \d+(x\(\d+\))* workers, \d+\w bytes max \(seg\d+\)\./Executor memory: ####K bytes avg x #### workers, ####K bytes max (seg#)./
-- m/Work_mem: \d+\w bytes max\./
-- s/Work_mem: \d+\w bytes max\. */Work_mem: ###K bytes max./
-- m/Execution Time: \d+\.\d+ ms/
-- s/Execution Time: \d+\.\d+ ms/Execution Time: ##.### ms/
-- m/Planning Time: \d+\.\d+ ms/
-- s/Planning Time: \d+\.\d+ ms/Planning Time: ##.### ms/
-- m/cost=\d+\.\d+\.\.\d+\.\d+ rows=\d+ width=\d+/
-- s/\(cost=\d+\.\d+\.\.\d+\.\d+ rows=\d+ width=\d+\)/(cost=##.###..##.### rows=### width=###)/
-- m/Memory used:  \d+\w?B/
-- s/Memory used:  \d+\w?B/Memory used: ###B/
-- m/Memory Usage: \d+\w?B/
-- s/Memory Usage: \d+\w?B/Memory Usage: ###B/
-- m/Memory wanted:  \d+\w?kB/
-- s/Memory wanted:  \d+\w?kB/Memory wanted: ###kB/
-- m/Peak Memory Usage: \d+/
-- s/Peak Memory Usage: \d+/Peak Memory Usage: ###/
-- m/Buckets: \d+/
-- s/Buckets: \d+/Buckets: ###/
-- m/Batches: \d+/
-- s/Batches: \d+/Batches: ###/
-- end_matchsubs
--
-- DEFAULT syntax
CREATE TABLE apples(id int PRIMARY KEY, type text);
INSERT INTO apples(id) SELECT generate_series(1, 100000);
CREATE TABLE box_locations(id int PRIMARY KEY, address text);
CREATE TABLE boxes(id int PRIMARY KEY, apple_id int REFERENCES apples(id), location_id int REFERENCES box_locations(id));

--- Check Explain Text format output
-- explain_processing_off
EXPLAIN SELECT * from boxes LEFT JOIN apples ON apples.id = boxes.apple_id LEFT JOIN box_locations ON box_locations.id = boxes.location_id;
-- explain_processing_on

--- Check Explain Analyze Text output that include the slices information
-- explain_processing_off
EXPLAIN (ANALYZE) SELECT * from boxes LEFT JOIN apples ON apples.id = boxes.apple_id LEFT JOIN box_locations ON box_locations.id = boxes.location_id;
-- explain_processing_on

-- Unaligned output format is better for the YAML / XML / JSON outputs.
-- In aligned format, you have end-of-line markers at the end of each line,
-- and its position depends on the longest line. If the width changes, all
-- lines need to be adjusted for the moved end-of-line-marker.
\a

-- YAML Required replaces for costs and time changes
-- start_matchsubs
-- m/ Loops: \d+/
-- s/ Loops: \d+/ Loops: #/
-- m/ Cost: \d+\.\d+/
-- s/ Cost: \d+\.\d+/ Cost: ###.##/
-- m/ Rows: \d+/
-- s/ Rows: \d+/ Rows: #####/
-- m/ Plan Width: \d+/
-- s/ Plan Width: \d+/ Plan Width: ##/
-- m/ Time: \d+\.\d+/
-- s/ Time: \d+\.\d+/ Time: ##.###/
-- m/Execution Time: \d+\.\d+/
-- s/Execution Time: \d+\.\d+/Execution Time: ##.###/
-- m/Segments: \d+/
-- s/Segments: \d+/Segments: #/
-- m/Pivotal Optimizer \(GPORCA\) version \d+\.\d+\.\d+",?/
-- s/Pivotal Optimizer \(GPORCA\) version \d+\.\d+\.\d+",?/Pivotal Optimizer \(GPORCA\)"/
-- m/ Memory: \d+/
-- s/ Memory: \d+/ Memory: ###/
-- m/Maximum Memory Used: \d+/
-- s/Maximum Memory Used: \d+/Maximum Memory Used: ###/
-- m/Workers: \d+/
-- s/Workers: \d+/Workers: ##/
-- m/Subworkers: \d+/
-- s/Subworkers: \d+/Subworkers: ##/
-- m/Average: \d+/
-- s/Average: \d+/Average: ##/
-- m/Total memory used across slices: \d+/
-- s/Total memory used across slices: \d+\s*/Total memory used across slices: ###/
-- m/Memory used: \d+/
-- s/Memory used: \d+/Memory used: ###/
-- end_matchsubs
-- Check Explain YAML output
EXPLAIN (FORMAT YAML) SELECT * from boxes LEFT JOIN apples ON apples.id = boxes.apple_id LEFT JOIN box_locations ON box_locations.id = boxes.location_id;

--- Check Explain Analyze YAML output that include the slices information
-- explain_processing_off
EXPLAIN (ANALYZE, FORMAT YAML) SELECT * from boxes LEFT JOIN apples ON apples.id = boxes.apple_id LEFT JOIN box_locations ON box_locations.id = boxes.location_id;
-- explain_processing_on

--
-- Test a simple case with JSON and XML output, too.
--
-- This should be enough for those format. The only difference between JSON,
-- XML, and YAML is in the formatting, after all.

-- Check JSON format
--
-- start_matchsubs
-- m/Pivotal Optimizer \(GPORCA\) version \d+\.\d+\.\d+/
-- s/Pivotal Optimizer \(GPORCA\) version \d+\.\d+\.\d+/Pivotal Optimizer \(GPORCA\)/
-- end_matchsubs
-- explain_processing_off
EXPLAIN (FORMAT JSON, COSTS OFF) SELECT * FROM generate_series(1, 10);

EXPLAIN (FORMAT XML, COSTS OFF) SELECT * FROM generate_series(1, 10);

-- Test for an old bug in printing Sequence nodes in JSON/XML format
-- (https://github.com/greenplum-db/gpdb/issues/9410)
CREATE TABLE jsonexplaintest (i int4) PARTITION BY RANGE (i) (START(1) END(3) EVERY(1));
EXPLAIN (FORMAT JSON, COSTS OFF) SELECT * FROM jsonexplaintest WHERE i = 2;

-- start_matchsubs
-- m/Extra Text: \(seg\d+\)   hash table\(s\): \d+; \d+ groups total in \d+ batches, \d+ spill partitions; disk usage: \d+KB; chain length \d+.\d+ avg, \d+ max; using \d+ of \d+ buckets; total \d+ expansions./
-- s/Extra Text: \(seg\d+\)   hash table\(s\): \d+; \d+ groups total in \d+ batches, \d+ spill partitions; disk usage: \d+KB; chain length \d+.\d+ avg, \d+ max; using \d+ of \d+ buckets; total \d+ expansions./Extra Text: (seg0)   hash table(s): ###; ### groups total in ### batches, ### spill partitions; disk usage: ###KB; chain length ###.## avg, ### max; using ## of ### buckets; total ### expansions./
-- m/Work_mem: \d+K bytes max, \d+K bytes wanted/
-- s/Work_mem: \d+K bytes max, \d+K bytes wanted/Work_mem: ###K bytes max, ###K bytes wanted/
-- end_matchsubs
-- Greenplum hash table extra message
CREATE TABLE test_src_tbl AS
SELECT i % 10000 AS a, i % 10000 + 1 AS b FROM generate_series(1, 50000) i DISTRIBUTED BY (a);
ANALYZE test_src_tbl;

-- Enable optimizer_enable_hashagg, and set statement_mem to a small value to force spilling
set optimizer_enable_hashagg = on;
SET statement_mem = '1000kB';

-- Hashagg with spilling
set hash_mem_multiplier = 1;
CREATE TABLE test_hashagg_spill AS
SELECT a, COUNT(DISTINCT b) AS b FROM test_src_tbl GROUP BY a;
EXPLAIN (analyze, costs off) SELECT a, COUNT(DISTINCT b) AS b FROM test_src_tbl GROUP BY a;

-- Hashagg with grouping sets
CREATE TABLE test_hashagg_groupingsets AS
SELECT a, avg(b) AS b FROM test_src_tbl GROUP BY grouping sets ((a), (b));
-- The planner generates multiple hash tables but ORCA uses Shared Scan.
-- flaky test
-- EXPLAIN (analyze, costs off) SELECT a, avg(b) AS b FROM test_src_tbl GROUP BY grouping sets ((a), (b));

reset hash_mem_multiplier;
RESET optimizer_enable_hashagg;
RESET statement_mem;

-- Cleanup
DROP TABLE boxes;
DROP TABLE apples;
DROP TABLE box_locations;
DROP TABLE jsonexplaintest;
DROP TABLE test_src_tbl;
DROP TABLE test_hashagg_spill;
DROP TABLE test_hashagg_groupingsets;
