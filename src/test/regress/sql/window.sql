-- TODO: incremental sort is turned off by default, because it may have
-- wrong result for some core case. Turn it on to run the existing tests
-- and minimize the difference from upstream.
-- start_matchignore
-- m/INFO:  GPORCA failed to produce a plan, falling back to Postgres-based planner/
-- m/DETAIL:  Falling back to Postgres-based planner because GPORCA does not support the following feature: Non-default collation/
-- end_matchignore
set enable_incremental_sort=on;

--
-- WINDOW FUNCTIONS
--
SET optimizer_trace_fallback=on;

CREATE TEMPORARY TABLE empsalary (
    depname varchar,
    empno bigint,
    salary int,
    enroll_date date
);

INSERT INTO empsalary VALUES
('develop', 10, 5200, '2007-08-01'),
('sales', 1, 5000, '2006-10-01'),
('personnel', 5, 3500, '2007-12-10'),
('sales', 4, 4800, '2007-08-08'),
('personnel', 2, 3900, '2006-12-23'),
('develop', 7, 4200, '2008-01-01'),
('develop', 9, 4500, '2008-01-01'),
('sales', 3, 4800, '2007-08-01'),
('develop', 8, 6000, '2006-10-01'),
('develop', 11, 5200, '2007-08-15');

SELECT depname, empno, salary, sum(salary) OVER (PARTITION BY depname) FROM empsalary ORDER BY depname, salary;

SELECT depname, empno, salary, rank() OVER (PARTITION BY depname ORDER BY salary) FROM empsalary;

-- with GROUP BY
SELECT four, ten, SUM(SUM(four)) OVER (PARTITION BY four), AVG(ten) FROM tenk1
GROUP BY four, ten ORDER BY four, ten;

SELECT depname, empno, salary, sum(salary) OVER w FROM empsalary WINDOW w AS (PARTITION BY depname);

SELECT depname, empno, salary, rank() OVER w FROM empsalary WINDOW w AS (PARTITION BY depname ORDER BY salary) ORDER BY rank() OVER w;

-- empty window specification
SELECT COUNT(*) OVER () FROM tenk1 WHERE unique2 < 10;

SELECT COUNT(*) OVER w FROM tenk1 WHERE unique2 < 10 WINDOW w AS ();

-- no window operation
SELECT four FROM tenk1 WHERE FALSE WINDOW w AS (PARTITION BY ten);

-- cumulative aggregate
SELECT sum(four) OVER (PARTITION BY ten ORDER BY unique2) AS sum_1, ten, four FROM tenk1 WHERE unique2 < 10;

SELECT row_number() OVER (ORDER BY unique2) FROM tenk1 WHERE unique2 < 10;

SELECT rank() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10;

SELECT dense_rank() OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10;

SELECT percent_rank() OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10;

SELECT cume_dist() OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10;

SELECT ntile(3) OVER (ORDER BY ten, four), ten, four FROM tenk1 WHERE unique2 < 10;

SELECT ntile(NULL) OVER (ORDER BY ten, four), ten, four FROM tenk1 LIMIT 2;

SELECT lag(ten) OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10;

SELECT lag(ten, four) OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10;

SELECT lag(ten, four, 0) OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10;
SELECT lag(ten, four, 0.7) OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10 ORDER BY four, ten;

SELECT lead(ten) OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10;

SELECT lead(ten * 2, 1) OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10;

SELECT lead(ten * 2, 1, -1) OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10;
SELECT lead(ten * 2, 1, -1.4) OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10 ORDER BY four, ten;

SELECT first_value(ten) OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10;

-- test split window func

explain SELECT * FROM (SELECT rank() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 < 3;
explain SELECT * FROM (SELECT row_number() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 < 3;
explain SELECT * FROM (SELECT dense_rank() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 < 3;
explain SELECT * FROM (SELECT percent_rank() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 < 0.5;
set optimizer_force_split_window_function to on;
-- worked
explain SELECT * FROM (SELECT rank() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 < 3;
explain SELECT * FROM (SELECT row_number() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 < 3;
explain SELECT * FROM (SELECT dense_rank() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 < 3;

explain SELECT * FROM (SELECT rank() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 <= 3;
explain SELECT * FROM (SELECT row_number() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 <= 3;
explain SELECT * FROM (SELECT dense_rank() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 <= 3;

-- no worked
explain SELECT * FROM (SELECT cume_dist() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 <= 1;
explain SELECT * FROM (SELECT percent_rank() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 < 0.5;
explain SELECT * FROM (SELECT rank() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 > 1;
explain SELECT * FROM (SELECT row_number() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 > 1;
explain SELECT * FROM (SELECT dense_rank() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 > 1;

-- verify the split window function result
set optimizer_force_split_window_function to off;
SELECT * FROM (SELECT rank() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 < 3;
SELECT * FROM (SELECT row_number() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 < 3;
SELECT * FROM (SELECT dense_rank() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 < 3;

SELECT * FROM (SELECT rank() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 <= 3;
SELECT * FROM (SELECT row_number() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 <= 3;
SELECT * FROM (SELECT dense_rank() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 <= 3;
set optimizer_force_split_window_function to on;
SELECT * FROM (SELECT rank() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 < 3;
SELECT * FROM (SELECT row_number() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 < 3;
SELECT * FROM (SELECT dense_rank() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 < 3;

SELECT * FROM (SELECT rank() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 <= 3;
SELECT * FROM (SELECT row_number() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 <= 3;
SELECT * FROM (SELECT dense_rank() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10) t WHERE rank_1 <= 3;
reset optimizer_force_split_window_function;

-- last_value returns the last row of the frame, which is CURRENT ROW in ORDER BY window.
-- the column `ten` is ordered, so we should call last_value on this
-- column. Using other cols the result is flaky because there are
-- tuples with the same `ten` while different other col values.
SELECT last_value(ten) OVER (ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10;

set search_path=singleseg, public;
SELECT last_value(ten) OVER (PARTITION BY four ORDER BY ten), ten, four FROM
	(SELECT * FROM tenk1 WHERE unique2 < 10 ORDER BY four, ten)s
	ORDER BY four, ten;

SELECT nth_value(ten, four + 1) OVER (PARTITION BY four ORDER BY ten), ten, four
	FROM (SELECT * FROM tenk1 WHERE unique2 < 10 ORDER BY four, ten)s order by four,ten;

reset search_path;

SELECT ten, two, sum(hundred) AS gsum, sum(sum(hundred)) OVER (PARTITION BY two ORDER BY ten) AS wsum
FROM tenk1 GROUP BY ten, two;

SELECT count(*) OVER (PARTITION BY four), four FROM (SELECT * FROM tenk1 WHERE two = 1)s WHERE unique2 < 10;

SELECT (count(*) OVER (PARTITION BY four ORDER BY ten) +
  sum(hundred) OVER (PARTITION BY four ORDER BY ten))::varchar AS cntsum
  FROM tenk1 WHERE unique2 < 10;

-- opexpr with different windows evaluation.
SELECT * FROM(
  SELECT count(*) OVER (PARTITION BY four ORDER BY ten) +
    sum(hundred) OVER (PARTITION BY two ORDER BY ten) AS total,
    count(*) OVER (PARTITION BY four ORDER BY ten) AS fourcount,
    sum(hundred) OVER (PARTITION BY two ORDER BY ten) AS twosum
    FROM tenk1
)sub
WHERE total <> fourcount + twosum;

SELECT avg(four) OVER (PARTITION BY four ORDER BY thousand / 100) FROM tenk1 WHERE unique2 < 10;

SELECT ten, two, sum(hundred) AS gsum, sum(sum(hundred)) OVER win AS wsum
FROM tenk1 GROUP BY ten, two WINDOW win AS (PARTITION BY two ORDER BY ten);

-- more than one window with GROUP BY
SELECT sum(salary),
	row_number() OVER (ORDER BY depname),
	sum(sum(salary)) OVER (ORDER BY depname DESC)
FROM empsalary GROUP BY depname;

-- identical windows with different names
SELECT sum(salary) OVER w1, count(*) OVER w2
FROM empsalary WINDOW w1 AS (ORDER BY salary), w2 AS (ORDER BY salary);

-- subplan
SELECT lead(ten, (SELECT two FROM tenk1 WHERE s.unique2 = unique2)) OVER (PARTITION BY four ORDER BY ten)
FROM tenk1 s WHERE unique2 < 10;

-- empty table
SELECT count(*) OVER (PARTITION BY four) FROM (SELECT * FROM tenk1 WHERE FALSE)s;

-- mixture of agg/wfunc in the same window
SELECT sum(salary) OVER w, rank() OVER w FROM empsalary WINDOW w AS (PARTITION BY depname ORDER BY salary DESC);

-- strict aggs
SELECT empno, depname, salary, bonus, depadj, MIN(bonus) OVER (ORDER BY empno), MAX(depadj) OVER () FROM(
	SELECT *,
		CASE WHEN enroll_date < '2008-01-01' THEN 2008 - extract(YEAR FROM enroll_date) END * 500 AS bonus,
		CASE WHEN
			AVG(salary) OVER (PARTITION BY depname) < salary
		THEN 200 END AS depadj FROM empsalary
)s;

-- window function over ungrouped agg over empty row set (bug before 9.1)
SELECT SUM(COUNT(f1)) OVER () FROM int4_tbl WHERE f1=42;

-- window function with ORDER BY an expression involving aggregates (9.1 bug)
select ten,
  sum(unique1) + sum(unique2) as res,
  rank() over (order by sum(unique1) + sum(unique2)) as rank
from tenk1
group by ten order by ten;

-- window and aggregate with GROUP BY expression (9.2 bug)
explain (costs off)
select first_value(max(x)) over (), y
  from (select unique1 as x, ten+four as y from tenk1) ss
  group by y;

-- test non-default frame specifications
SELECT four, ten,
	sum(ten) over (partition by four order by ten),
	last_value(ten) over (partition by four order by ten)
FROM (select distinct ten, four from tenk1) ss;

SELECT four, ten,
	sum(ten) over (partition by four order by ten range between unbounded preceding and current row),
	last_value(ten) over (partition by four order by ten range between unbounded preceding and current row)
FROM (select distinct ten, four from tenk1) ss;

SELECT four, ten,
	sum(ten) over (partition by four order by ten range between unbounded preceding and unbounded following),
	last_value(ten) over (partition by four order by ten range between unbounded preceding and unbounded following)
FROM (select distinct ten, four from tenk1) ss;

SELECT four, ten/4 as two,
	sum(ten/4) over (partition by four order by ten/4 range between unbounded preceding and current row),
	last_value(ten/4) over (partition by four order by ten/4 range between unbounded preceding and current row)
FROM (select distinct ten, four from tenk1) ss;

SELECT four, ten/4 as two,
	sum(ten/4) over (partition by four order by ten/4 rows between unbounded preceding and current row),
	last_value(ten/4) over (partition by four order by ten/4 rows between unbounded preceding and current row)
FROM (select distinct ten, four from tenk1) ss;

SELECT sum(unique1) over (order by four range between current row and unbounded following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

set search_path=singleseg, public;
SELECT sum(unique1) over (rows between current row and unbounded following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (rows between 2 preceding and 2 following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (rows between 2 preceding and 2 following exclude no others),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (rows between 2 preceding and 2 following exclude current row),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (rows between 2 preceding and 2 following exclude group),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (rows between 2 preceding and 2 following exclude ties),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT first_value(unique1) over (ORDER BY four rows between current row and 2 following exclude current row),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT first_value(unique1) over (ORDER BY four rows between current row and 2 following exclude group),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT first_value(unique1) over (ORDER BY four rows between current row and 2 following exclude ties),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT last_value(unique1) over (ORDER BY four rows between current row and 2 following exclude current row),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT last_value(unique1) over (ORDER BY four rows between current row and 2 following exclude group),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT last_value(unique1) over (ORDER BY four rows between current row and 2 following exclude ties),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (rows between 2 preceding and 1 preceding),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (rows between 1 following and 3 following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (rows between unbounded preceding and 1 following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (w range between current row and unbounded following),
	unique1, four
FROM tenk1 WHERE unique1 < 10 WINDOW w AS (order by four);

SELECT sum(unique1) over (w range between unbounded preceding and current row exclude current row),
	unique1, four
FROM tenk1 WHERE unique1 < 10 WINDOW w AS (order by four);

SELECT sum(unique1) over (w range between unbounded preceding and current row exclude group),
	unique1, four
FROM tenk1 WHERE unique1 < 10 WINDOW w AS (order by four);

SELECT sum(unique1) over (w range between unbounded preceding and current row exclude ties),
	unique1, four
FROM tenk1 WHERE unique1 < 10 WINDOW w AS (order by four);

SELECT first_value(unique1) over w,
	nth_value(unique1, 2) over w AS nth_2,
	last_value(unique1) over w, unique1, four
FROM tenk1 WHERE unique1 < 10
WINDOW w AS (order by four range between current row and unbounded following);

SELECT sum(unique1) over
	(order by unique1
	 rows (SELECT unique1 FROM tenk1 ORDER BY unique1 LIMIT 1) + 1 PRECEDING),
	unique1
FROM tenk1 WHERE unique1 < 10;

CREATE TEMP VIEW v_window AS
	SELECT i, sum(i) over (order by i rows between 1 preceding and 1 following) as sum_rows
	FROM generate_series(1, 10) i;

SELECT * FROM v_window;

SELECT pg_get_viewdef('v_window');

reset search_path;

CREATE OR REPLACE TEMP VIEW v_window AS
	SELECT i, sum(i) over (order by i rows between 1 preceding and 1 following
	exclude current row) as sum_rows FROM generate_series(1, 10) i;

SELECT * FROM v_window;

SELECT pg_get_viewdef('v_window');

CREATE OR REPLACE TEMP VIEW v_window AS
	SELECT i, sum(i) over (order by i rows between 1 preceding and 1 following
	exclude group) as sum_rows FROM generate_series(1, 10) i;

SELECT * FROM v_window;

SELECT pg_get_viewdef('v_window');

CREATE OR REPLACE TEMP VIEW v_window AS
	SELECT i, sum(i) over (order by i rows between 1 preceding and 1 following
	exclude ties) as sum_rows FROM generate_series(1, 10) i;

SELECT * FROM v_window;

SELECT pg_get_viewdef('v_window');

CREATE OR REPLACE TEMP VIEW v_window AS
	SELECT i, sum(i) over (order by i rows between 1 preceding and 1 following
	exclude no others) as sum_rows FROM generate_series(1, 10) i;

SELECT * FROM v_window;

SELECT pg_get_viewdef('v_window');

CREATE OR REPLACE TEMP VIEW v_window AS
	SELECT i, sum(i) over (order by i groups between 1 preceding and 1 following) as sum_rows FROM generate_series(1, 10) i;

SELECT * FROM v_window;

SELECT pg_get_viewdef('v_window');

DROP VIEW v_window;

CREATE TEMP VIEW v_window AS
	SELECT i, min(i) over (order by i range between '1 day' preceding and '10 days' following) as min_i
  FROM generate_series(now(), now()+'100 days'::interval, '1 hour') i;

SELECT pg_get_viewdef('v_window');

-- RANGE offset PRECEDING/FOLLOWING tests

SELECT sum(unique1) over (order by four range between 2::int8 preceding and 1::int2 preceding),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four desc range between 2::int8 preceding and 1::int2 preceding),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four range between 2::int8 preceding and 1::int2 preceding exclude no others),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four range between 2::int8 preceding and 1::int2 preceding exclude current row),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four range between 2::int8 preceding and 1::int2 preceding exclude group),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four range between 2::int8 preceding and 1::int2 preceding exclude ties),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four range between 2::int8 preceding and 6::int2 following exclude ties),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four range between 2::int8 preceding and 6::int2 following exclude group),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (partition by four order by unique1 range between 5::int8 preceding and 6::int2 following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (partition by four order by unique1 range between 5::int8 preceding and 6::int2 following
	exclude current row),unique1, four
FROM tenk1 WHERE unique1 < 10;

select sum(salary) over (order by enroll_date range between '1 year'::interval preceding and '1 year'::interval following),
	salary, enroll_date from empsalary;

select sum(salary) over (order by enroll_date desc range between '1 year'::interval preceding and '1 year'::interval following),
	salary, enroll_date from empsalary;

select sum(salary) over (order by enroll_date desc range between '1 year'::interval following and '1 year'::interval following),
	salary, enroll_date from empsalary;

select sum(salary) over (order by enroll_date range between '1 year'::interval preceding and '1 year'::interval following
	exclude current row), salary, enroll_date from empsalary;

select sum(salary) over (order by enroll_date range between '1 year'::interval preceding and '1 year'::interval following
	exclude group), salary, enroll_date from empsalary;

select sum(salary) over (order by enroll_date range between '1 year'::interval preceding and '1 year'::interval following
	exclude ties), salary, enroll_date from empsalary;

select first_value(salary) over(order by salary range between 1000 preceding and 1000 following),
	lead(salary) over(order by salary range between 1000 preceding and 1000 following),
	nth_value(salary, 1) over(order by salary range between 1000 preceding and 1000 following),
	salary from empsalary;

select last_value(salary) over(order by salary range between 1000 preceding and 1000 following),
	lag(salary) over(order by salary range between 1000 preceding and 1000 following),
	salary from empsalary;

select first_value(salary) over(order by salary range between 1000 following and 3000 following
	exclude current row),
	lead(salary) over(order by salary range between 1000 following and 3000 following exclude ties),
	nth_value(salary, 1) over(order by salary range between 1000 following and 3000 following
	exclude ties),
	salary from empsalary;

select last_value(salary) over(order by salary range between 1000 following and 3000 following
	exclude group),
	lag(salary) over(order by salary range between 1000 following and 3000 following exclude group),
	salary from empsalary;

select first_value(salary) over(order by enroll_date range between unbounded preceding and '1 year'::interval following
	exclude ties),
	last_value(salary) over(order by enroll_date range between unbounded preceding and '1 year'::interval following),
	salary, enroll_date from empsalary;

select first_value(salary) over(order by enroll_date range between unbounded preceding and '1 year'::interval following
	exclude ties),
	last_value(salary) over(order by enroll_date range between unbounded preceding and '1 year'::interval following
	exclude ties),
	salary, enroll_date from empsalary;

select first_value(salary) over(order by enroll_date range between unbounded preceding and '1 year'::interval following
	exclude group),
	last_value(salary) over(order by enroll_date range between unbounded preceding and '1 year'::interval following
	exclude group),
	salary, enroll_date from empsalary;

select first_value(salary) over(order by enroll_date range between unbounded preceding and '1 year'::interval following
	exclude current row),
	last_value(salary) over(order by enroll_date range between unbounded preceding and '1 year'::interval following
	exclude current row),
	salary, enroll_date from empsalary;

-- RANGE offset PRECEDING/FOLLOWING with null values
select x, y,
       first_value(y) over w,
       last_value(y) over w
from
  (select x, x as y from generate_series(1,5) as x
   union all select null, 42
   union all select null, 43) ss
window w as
  (order by x asc nulls first range between 2 preceding and 2 following);

select x, y,
       first_value(y) over w,
       last_value(y) over w
from
  (select x, x as y from generate_series(1,5) as x
   union all select null, 42
   union all select null, 43) ss
window w as
  (order by x asc nulls last range between 2 preceding and 2 following);

select x, y,
       first_value(y) over w,
       last_value(y) over w
from
  (select x, x as y from generate_series(1,5) as x
   union all select null, 42
   union all select null, 43) ss
window w as
  (order by x desc nulls first range between 2 preceding and 2 following);

select x, y,
       first_value(y) over w,
       last_value(y) over w
from
  (select x, x as y from generate_series(1,5) as x
   union all select null, 42
   union all select null, 43) ss
window w as
  (order by x desc nulls last range between 2 preceding and 2 following);

-- Check overflow behavior for various integer sizes

select x, last_value(x) over (order by x::smallint range between current row and 2147450884 following)
from generate_series(32764, 32766) x;

select x, last_value(x) over (order by x::smallint desc range between current row and 2147450885 following)
from generate_series(-32766, -32764) x;

select x, last_value(x) over (order by x range between current row and 4 following)
from generate_series(2147483644, 2147483646) x;

select x, last_value(x) over (order by x desc range between current row and 5 following)
from generate_series(-2147483646, -2147483644) x;

select x, last_value(x) over (order by x range between current row and 4 following)
from generate_series(9223372036854775804, 9223372036854775806) x;

select x, last_value(x) over (order by x desc range between current row and 5 following)
from generate_series(-9223372036854775806, -9223372036854775804) x;

-- Test in_range for other numeric datatypes

create temp table numerics(
    id int,
    f_float4 float4,
    f_float8 float8,
    f_numeric numeric
);

insert into numerics values
(0, '-infinity', '-infinity', '-infinity'),
(1, -3, -3, -3),
(2, -1, -1, -1),
(3, 0, 0, 0),
(4, 1.1, 1.1, 1.1),
(5, 1.12, 1.12, 1.12),
(6, 2, 2, 2),
(7, 100, 100, 100),
(8, 'infinity', 'infinity', 'infinity'),
(9, 'NaN', 'NaN', 'NaN');

select id, f_float4, first_value(id) over w, last_value(id) over w
from numerics
window w as (order by f_float4 range between
             1 preceding and 1 following);
select id, f_float4, first_value(id) over w, last_value(id) over w
from numerics
window w as (order by f_float4 range between
             1 preceding and 1.1::float4 following);
select id, f_float4, first_value(id) over w, last_value(id) over w
from numerics
window w as (order by f_float4 range between
             'inf' preceding and 'inf' following);
select id, f_float4, first_value(id) over w, last_value(id) over w
from numerics
window w as (order by f_float4 range between
             'inf' preceding and 'inf' preceding);
select id, f_float4, first_value(id) over w, last_value(id) over w
from numerics
window w as (order by f_float4 range between
             'inf' following and 'inf' following);
select id, f_float4, first_value(id) over w, last_value(id) over w
from numerics
window w as (order by f_float4 range between
             1.1 preceding and 'NaN' following);  -- error, NaN disallowed

select id, f_float8, first_value(id) over w, last_value(id) over w
from numerics
window w as (order by f_float8 range between
             1 preceding and 1 following);
select id, f_float8, first_value(id) over w, last_value(id) over w
from numerics
window w as (order by f_float8 range between
             1 preceding and 1.1::float8 following);
select id, f_float8, first_value(id) over w, last_value(id) over w
from numerics
window w as (order by f_float8 range between
             'inf' preceding and 'inf' following);
select id, f_float8, first_value(id) over w, last_value(id) over w
from numerics
window w as (order by f_float8 range between
             'inf' preceding and 'inf' preceding);
select id, f_float8, first_value(id) over w, last_value(id) over w
from numerics
window w as (order by f_float8 range between
             'inf' following and 'inf' following);
select id, f_float8, first_value(id) over w, last_value(id) over w
from numerics
window w as (order by f_float8 range between
             1.1 preceding and 'NaN' following);  -- error, NaN disallowed

select id, f_numeric, first_value(id) over w, last_value(id) over w
from numerics
window w as (order by f_numeric range between
             1 preceding and 1 following);
select id, f_numeric, first_value(id) over w, last_value(id) over w
from numerics
window w as (order by f_numeric range between
             1 preceding and 1.1::numeric following);
select id, f_numeric, first_value(id) over w, last_value(id) over w
from numerics
window w as (order by f_numeric range between
             1 preceding and 1.1::float8 following);  -- currently unsupported
select id, f_numeric, first_value(id) over w, last_value(id) over w
from numerics
window w as (order by f_numeric range between
             'inf' preceding and 'inf' following);
select id, f_numeric, first_value(id) over w, last_value(id) over w
from numerics
window w as (order by f_numeric range between
             'inf' preceding and 'inf' preceding);
select id, f_numeric, first_value(id) over w, last_value(id) over w
from numerics
window w as (order by f_numeric range between
             'inf' following and 'inf' following);
select id, f_numeric, first_value(id) over w, last_value(id) over w
from numerics
window w as (order by f_numeric range between
             1.1 preceding and 'NaN' following);  -- error, NaN disallowed

-- Test in_range for other datetime datatypes

create temp table datetimes(
    id int,
    f_time time,
    f_timetz timetz,
    f_interval interval,
    f_timestamptz timestamptz,
    f_timestamp timestamp
);

insert into datetimes values
(1, '11:00', '11:00 BST', '1 year', '2000-10-19 10:23:54+01', '2000-10-19 10:23:54'),
(2, '12:00', '12:00 BST', '2 years', '2001-10-19 10:23:54+01', '2001-10-19 10:23:54'),
(3, '13:00', '13:00 BST', '3 years', '2001-10-19 10:23:54+01', '2001-10-19 10:23:54'),
(4, '14:00', '14:00 BST', '4 years', '2002-10-19 10:23:54+01', '2002-10-19 10:23:54'),
(5, '15:00', '15:00 BST', '5 years', '2003-10-19 10:23:54+01', '2003-10-19 10:23:54'),
(6, '15:00', '15:00 BST', '5 years', '2004-10-19 10:23:54+01', '2004-10-19 10:23:54'),
(7, '17:00', '17:00 BST', '7 years', '2005-10-19 10:23:54+01', '2005-10-19 10:23:54'),
(8, '18:00', '18:00 BST', '8 years', '2006-10-19 10:23:54+01', '2006-10-19 10:23:54'),
(9, '19:00', '19:00 BST', '9 years', '2007-10-19 10:23:54+01', '2007-10-19 10:23:54'),
(10, '20:00', '20:00 BST', '10 years', '2008-10-19 10:23:54+01', '2008-10-19 10:23:54');

select id, f_time, first_value(id) over w, last_value(id) over w
from datetimes
window w as (order by f_time range between
             '70 min'::interval preceding and '2 hours'::interval following);

select id, f_time, first_value(id) over w, last_value(id) over w
from datetimes
window w as (order by f_time desc range between
             '70 min' preceding and '2 hours' following);

select id, f_timetz, first_value(id) over w, last_value(id) over w
from datetimes
window w as (order by f_timetz range between
             '70 min'::interval preceding and '2 hours'::interval following);

select id, f_timetz, first_value(id) over w, last_value(id) over w
from datetimes
window w as (order by f_timetz desc range between
             '70 min' preceding and '2 hours' following);

select id, f_interval, first_value(id) over w, last_value(id) over w
from datetimes
window w as (order by f_interval range between
             '1 year'::interval preceding and '1 year'::interval following);

select id, f_interval, first_value(id) over w, last_value(id) over w
from datetimes
window w as (order by f_interval desc range between
             '1 year' preceding and '1 year' following);

select id, f_timestamptz, first_value(id) over w, last_value(id) over w
from datetimes
window w as (order by f_timestamptz range between
             '1 year'::interval preceding and '1 year'::interval following);

select id, f_timestamptz, first_value(id) over w, last_value(id) over w
from datetimes
window w as (order by f_timestamptz desc range between
             '1 year' preceding and '1 year' following);

select id, f_timestamp, first_value(id) over w, last_value(id) over w
from datetimes
window w as (order by f_timestamp range between
             '1 year'::interval preceding and '1 year'::interval following);

select id, f_timestamp, first_value(id) over w, last_value(id) over w
from datetimes
window w as (order by f_timestamp desc range between
             '1 year' preceding and '1 year' following);

-- RANGE offset PRECEDING/FOLLOWING error cases
select sum(salary) over (order by enroll_date, salary range between '1 year'::interval preceding and '2 years'::interval following
	exclude ties), salary, enroll_date from empsalary;

select sum(salary) over (range between '1 year'::interval preceding and '2 years'::interval following
	exclude ties), salary, enroll_date from empsalary;

select sum(salary) over (order by depname range between '1 year'::interval preceding and '2 years'::interval following
	exclude ties), salary, enroll_date from empsalary;

select max(enroll_date) over (order by enroll_date range between 1 preceding and 2 following
	exclude ties), salary, enroll_date from empsalary;

select max(enroll_date) over (order by salary range between -1 preceding and 2 following
	exclude ties), salary, enroll_date from empsalary;

select max(enroll_date) over (order by salary range between 1 preceding and -2 following
	exclude ties), salary, enroll_date from empsalary;

select max(enroll_date) over (order by salary range between '1 year'::interval preceding and '2 years'::interval following
	exclude ties), salary, enroll_date from empsalary;

select max(enroll_date) over (order by enroll_date range between '1 year'::interval preceding and '-2 years'::interval following
	exclude ties), salary, enroll_date from empsalary;

-- GROUPS tests

SELECT sum(unique1) over (order by four groups between unbounded preceding and current row),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four groups between unbounded preceding and unbounded following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four groups between current row and unbounded following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four groups between 1 preceding and unbounded following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four groups between 1 following and unbounded following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four groups between unbounded preceding and 2 following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four groups between 2 preceding and 1 preceding),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four groups between 2 preceding and 1 following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four groups between 0 preceding and 0 following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four groups between 2 preceding and 1 following
	exclude current row), unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four groups between 2 preceding and 1 following
	exclude group), unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four groups between 2 preceding and 1 following
	exclude ties), unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (partition by ten
	order by four groups between 0 preceding and 0 following),unique1, four, ten
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (partition by ten
	order by four groups between 0 preceding and 0 following exclude current row), unique1, four, ten
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (partition by ten
	order by four groups between 0 preceding and 0 following exclude group), unique1, four, ten
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (partition by ten
	order by four groups between 0 preceding and 0 following exclude ties), unique1, four, ten
FROM tenk1 WHERE unique1 < 10;

select first_value(salary) over(order by enroll_date groups between 1 preceding and 1 following),
	lead(salary) over(order by enroll_date groups between 1 preceding and 1 following),
	nth_value(salary, 1) over(order by enroll_date groups between 1 preceding and 1 following),
	salary, enroll_date from empsalary;

select last_value(salary) over(order by enroll_date groups between 1 preceding and 1 following),
	lag(salary) over(order by enroll_date groups between 1 preceding and 1 following),
	salary, enroll_date from empsalary;

select first_value(salary) over(order by enroll_date groups between 1 following and 3 following
	exclude current row),
	lead(salary) over(order by enroll_date groups between 1 following and 3 following exclude ties),
	nth_value(salary, 1) over(order by enroll_date groups between 1 following and 3 following
	exclude ties),
	salary, enroll_date from empsalary;

select last_value(salary) over(order by enroll_date groups between 1 following and 3 following
	exclude group),
	lag(salary) over(order by enroll_date groups between 1 following and 3 following exclude group),
	salary, enroll_date from empsalary;

-- Show differences in offset interpretation between ROWS, RANGE, and GROUPS
WITH cte (x) AS (
        SELECT * FROM generate_series(1, 35, 2)
)
SELECT x, (sum(x) over w)
FROM cte
WINDOW w AS (ORDER BY x rows between 1 preceding and 1 following);

WITH cte (x) AS (
        SELECT * FROM generate_series(1, 35, 2)
)
SELECT x, (sum(x) over w)
FROM cte
WINDOW w AS (ORDER BY x range between 1 preceding and 1 following);

WITH cte (x) AS (
        SELECT * FROM generate_series(1, 35, 2)
)
SELECT x, (sum(x) over w)
FROM cte
WINDOW w AS (ORDER BY x groups between 1 preceding and 1 following);

WITH cte (x) AS (
        select 1 union all select 1 union all select 1 union all
        SELECT * FROM generate_series(5, 49, 2)
)
SELECT x, (sum(x) over w)
FROM cte
WINDOW w AS (ORDER BY x rows between 1 preceding and 1 following);

WITH cte (x) AS (
        select 1 union all select 1 union all select 1 union all
        SELECT * FROM generate_series(5, 49, 2)
)
SELECT x, (sum(x) over w)
FROM cte
WINDOW w AS (ORDER BY x range between 1 preceding and 1 following);

WITH cte (x) AS (
        select 1 union all select 1 union all select 1 union all
        SELECT * FROM generate_series(5, 49, 2)
)
SELECT x, (sum(x) over w)
FROM cte
WINDOW w AS (ORDER BY x groups between 1 preceding and 1 following);

-- with UNION
SELECT count(*) OVER (PARTITION BY four) FROM (SELECT * FROM tenk1 UNION ALL SELECT * FROM tenk2)s LIMIT 0;

-- check some degenerate cases
create temp table t1 (f1 int, f2 int8);
insert into t1 values (1,1),(1,2),(2,2);

select f1, sum(f1) over (partition by f1
                         range between 1 preceding and 1 following)
from t1 where f1 = f2;  -- error, must have order by
explain (costs off)
select f1, sum(f1) over (partition by f1 order by f2
                         range between 1 preceding and 1 following)
from t1 where f1 = f2;
select f1, sum(f1) over (partition by f1 order by f2
                         range between 1 preceding and 1 following)
from t1 where f1 = f2;
select f1, sum(f1) over (partition by f1, f1 order by f2
                         range between 2 preceding and 1 preceding)
from t1 where f1 = f2;
select f1, sum(f1) over (partition by f1, f2 order by f2
                         range between 1 following and 2 following)
from t1 where f1 = f2;

select f1, sum(f1) over (partition by f1
                         groups between 1 preceding and 1 following)
from t1 where f1 = f2;  -- error, must have order by
explain (costs off)
select f1, sum(f1) over (partition by f1 order by f2
                         groups between 1 preceding and 1 following)
from t1 where f1 = f2;
select f1, sum(f1) over (partition by f1 order by f2
                         groups between 1 preceding and 1 following)
from t1 where f1 = f2;
select f1, sum(f1) over (partition by f1, f1 order by f2
                         groups between 2 preceding and 1 preceding)
from t1 where f1 = f2;
select f1, sum(f1) over (partition by f1, f2 order by f2
                         groups between 1 following and 2 following)
from t1 where f1 = f2;

-- ordering by a non-integer constant is allowed
SELECT rank() OVER (ORDER BY length('abc'));

-- can't order by another window function
SELECT rank() OVER (ORDER BY rank() OVER (ORDER BY random()));

-- some other errors
SELECT * FROM empsalary WHERE row_number() OVER (ORDER BY salary) < 10;

SELECT * FROM empsalary INNER JOIN tenk1 ON row_number() OVER (ORDER BY salary) < 10;

SELECT rank() OVER (ORDER BY 1), count(*) FROM empsalary GROUP BY 1;

SELECT * FROM rank() OVER (ORDER BY random());

DELETE FROM empsalary WHERE (rank() OVER (ORDER BY random())) > 10;

DELETE FROM empsalary RETURNING rank() OVER (ORDER BY random());

SELECT count(*) OVER w FROM tenk1 WINDOW w AS (ORDER BY unique1), w AS (ORDER BY unique1);

SELECT rank() OVER (PARTITION BY four, ORDER BY ten) FROM tenk1;

-- Not allowed in PostgreSQL, but is allowed in GPDB for backwards-compatibility.
-- Added LIMIT to reduce the size of the output.
SELECT count() OVER () FROM tenk1 limit 5;

SELECT generate_series(1, 100) OVER () FROM empsalary;

SELECT ntile(0) OVER (ORDER BY ten), ten, four FROM tenk1;

SELECT nth_value(four, 0) OVER (ORDER BY ten), ten, four FROM tenk1;

-- filter

SELECT sum(salary), row_number() OVER (ORDER BY depname), sum(
    sum(salary) FILTER (WHERE enroll_date > '2007-01-01')
) FILTER (WHERE depname <> 'sales') OVER (ORDER BY depname DESC) AS "filtered_sum",
    depname
FROM empsalary GROUP BY depname;

-- Test pushdown of quals into a subquery containing window functions

-- pushdown is safe because all PARTITION BY clauses include depname:
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT depname,
          sum(salary) OVER (PARTITION BY depname) depsalary,
          min(salary) OVER (PARTITION BY depname || 'A', depname) depminsalary
   FROM empsalary) emp
WHERE depname = 'sales';

-- pushdown is unsafe because there's a PARTITION BY clause without depname:
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT depname,
          sum(salary) OVER (PARTITION BY enroll_date) enroll_salary,
          min(salary) OVER (PARTITION BY depname) depminsalary
   FROM empsalary) emp
WHERE depname = 'sales';

-- pushdown is unsafe because the subquery contains window functions and the qual is volatile:
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT depname,
          sum(salary) OVER (PARTITION BY depname) depsalary,
          min(salary) OVER (PARTITION BY depname || 'A', depname) depminsalary
   FROM empsalary) emp
WHERE depname = 'sales' OR RANDOM() > 0.5;

-- Test Sort node collapsing
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT depname,
          sum(salary) OVER (PARTITION BY depname order by empno) depsalary,
          min(salary) OVER (PARTITION BY depname, empno order by enroll_date) depminsalary
   FROM empsalary) emp
WHERE depname = 'sales';

-- Test Sort node reordering
EXPLAIN (COSTS OFF)
SELECT
  lead(1) OVER (PARTITION BY depname ORDER BY salary, enroll_date),
  lag(1) OVER (PARTITION BY depname ORDER BY salary,enroll_date,empno)
FROM empsalary;

-- Test incremental sorting
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT depname,
          empno,
          salary,
          enroll_date,
          row_number() OVER (PARTITION BY depname ORDER BY enroll_date) AS first_emp,
          row_number() OVER (PARTITION BY depname ORDER BY enroll_date DESC) AS last_emp
   FROM empsalary) emp
WHERE first_emp = 1 OR last_emp = 1;

SELECT * FROM
  (SELECT depname,
          empno,
          salary,
          enroll_date,
          row_number() OVER (PARTITION BY depname ORDER BY enroll_date) AS first_emp,
          row_number() OVER (PARTITION BY depname ORDER BY enroll_date DESC) AS last_emp
   FROM empsalary) emp
WHERE first_emp = 1 OR last_emp = 1;

-- cleanup
DROP TABLE empsalary;

-- test user-defined window function with named args and default args
CREATE FUNCTION nth_value_def(val anyelement, n integer = 1) RETURNS anyelement
  LANGUAGE internal WINDOW IMMUTABLE STRICT AS 'window_nth_value';

-- GPDB: LIMIT 100 added, to force the result of the subquery to be ordered
-- across all segments.
SELECT nth_value_def(n := 2, val := ten) OVER (PARTITION BY four), ten, four
  FROM (SELECT * FROM tenk1 WHERE unique2 < 10 ORDER BY four, ten LIMIT 100) s;

SELECT nth_value_def(ten) OVER (PARTITION BY four), ten, four
  FROM (SELECT * FROM tenk1 WHERE unique2 < 10 ORDER BY four, ten LIMIT 100) s;

--
-- Test the basic moving-aggregate machinery
--

-- create aggregates that record the series of transform calls (these are
-- intentionally not true inverses)

CREATE FUNCTION logging_sfunc_nonstrict(text, anyelement) RETURNS text AS
$$ SELECT COALESCE($1, '') || '*' || quote_nullable($2) $$
LANGUAGE SQL IMMUTABLE;

CREATE FUNCTION logging_msfunc_nonstrict(text, anyelement) RETURNS text AS
$$ SELECT COALESCE($1, '') || '+' || quote_nullable($2) $$
LANGUAGE SQL IMMUTABLE;

CREATE FUNCTION logging_minvfunc_nonstrict(text, anyelement) RETURNS text AS
$$ SELECT $1 || '-' || quote_nullable($2) $$
LANGUAGE SQL IMMUTABLE;

CREATE AGGREGATE logging_agg_nonstrict (anyelement)
(
	stype = text,
	sfunc = logging_sfunc_nonstrict,
	mstype = text,
	msfunc = logging_msfunc_nonstrict,
	minvfunc = logging_minvfunc_nonstrict
);

CREATE AGGREGATE logging_agg_nonstrict_initcond (anyelement)
(
	stype = text,
	sfunc = logging_sfunc_nonstrict,
	mstype = text,
	msfunc = logging_msfunc_nonstrict,
	minvfunc = logging_minvfunc_nonstrict,
	initcond = 'I',
	minitcond = 'MI'
);

CREATE FUNCTION logging_sfunc_strict(text, anyelement) RETURNS text AS
$$ SELECT $1 || '*' || quote_nullable($2) $$
LANGUAGE SQL STRICT IMMUTABLE;

CREATE FUNCTION logging_msfunc_strict(text, anyelement) RETURNS text AS
$$ SELECT $1 || '+' || quote_nullable($2) $$
LANGUAGE SQL STRICT IMMUTABLE;

CREATE FUNCTION logging_minvfunc_strict(text, anyelement) RETURNS text AS
$$ SELECT $1 || '-' || quote_nullable($2) $$
LANGUAGE SQL STRICT IMMUTABLE;

CREATE AGGREGATE logging_agg_strict (text)
(
	stype = text,
	sfunc = logging_sfunc_strict,
	mstype = text,
	msfunc = logging_msfunc_strict,
	minvfunc = logging_minvfunc_strict
);

CREATE AGGREGATE logging_agg_strict_initcond (anyelement)
(
	stype = text,
	sfunc = logging_sfunc_strict,
	mstype = text,
	msfunc = logging_msfunc_strict,
	minvfunc = logging_minvfunc_strict,
	initcond = 'I',
	minitcond = 'MI'
);

-- test strict and non-strict cases
SELECT
	p::text || ',' || i::text || ':' || COALESCE(v::text, 'NULL') AS row,
	logging_agg_nonstrict(v) over wnd as nstrict,
	logging_agg_nonstrict_initcond(v) over wnd as nstrict_init,
	logging_agg_strict(v::text) over wnd as strict,
	logging_agg_strict_initcond(v) over wnd as strict_init
FROM (VALUES
	(1, 1, NULL),
	(1, 2, 'a'),
	(1, 3, 'b'),
	(1, 4, NULL),
	(1, 5, NULL),
	(1, 6, 'c'),
	(2, 1, NULL),
	(2, 2, 'x'),
	(3, 1, 'z')
) AS t(p, i, v)
WINDOW wnd AS (PARTITION BY P ORDER BY i ROWS BETWEEN 1 PRECEDING AND CURRENT ROW)
ORDER BY p, i;

-- and again, but with filter
SELECT
	p::text || ',' || i::text || ':' ||
		CASE WHEN f THEN COALESCE(v::text, 'NULL') ELSE '-' END as row,
	logging_agg_nonstrict(v) filter(where f) over wnd as nstrict_filt,
	logging_agg_nonstrict_initcond(v) filter(where f) over wnd as nstrict_init_filt,
	logging_agg_strict(v::text) filter(where f) over wnd as strict_filt,
	logging_agg_strict_initcond(v) filter(where f) over wnd as strict_init_filt
FROM (VALUES
	(1, 1, true,  NULL),
	(1, 2, false, 'a'),
	(1, 3, true,  'b'),
	(1, 4, false, NULL),
	(1, 5, false, NULL),
	(1, 6, false, 'c'),
	(2, 1, false, NULL),
	(2, 2, true,  'x'),
	(3, 1, true,  'z')
) AS t(p, i, f, v)
WINDOW wnd AS (PARTITION BY p ORDER BY i ROWS BETWEEN 1 PRECEDING AND CURRENT ROW)
ORDER BY p, i;

-- test that volatile arguments disable moving-aggregate mode
SELECT
	i::text || ':' || COALESCE(v::text, 'NULL') as row,
	logging_agg_strict(v::text)
		over wnd as inverse,
	logging_agg_strict(v::text || CASE WHEN random() < 0 then '?' ELSE '' END)
		over wnd as noinverse
FROM (VALUES
	(1, 'a'),
	(2, 'b'),
	(3, 'c')
) AS t(i, v)
WINDOW wnd AS (ORDER BY i ROWS BETWEEN 1 PRECEDING AND CURRENT ROW)
ORDER BY i;

SELECT
	i::text || ':' || COALESCE(v::text, 'NULL') as row,
	logging_agg_strict(v::text) filter(where true)
		over wnd as inverse,
	logging_agg_strict(v::text) filter(where random() >= 0)
		over wnd as noinverse
FROM (VALUES
	(1, 'a'),
	(2, 'b'),
	(3, 'c')
) AS t(i, v)
WINDOW wnd AS (ORDER BY i ROWS BETWEEN 1 PRECEDING AND CURRENT ROW)
ORDER BY i;

-- test that non-overlapping windows don't use inverse transitions
SELECT
	logging_agg_strict(v::text) OVER wnd
FROM (VALUES
	(1, 'a'),
	(2, 'b'),
	(3, 'c')
) AS t(i, v)
WINDOW wnd AS (ORDER BY i ROWS BETWEEN CURRENT ROW AND CURRENT ROW)
ORDER BY i;

-- test that returning NULL from the inverse transition functions
-- restarts the aggregation from scratch. The second aggregate is supposed
-- to test cases where only some aggregates restart, the third one checks
-- that one aggregate restarting doesn't cause others to restart.

CREATE FUNCTION sum_int_randrestart_minvfunc(int4, int4) RETURNS int4 AS
$$ SELECT CASE WHEN random() < 0.2 THEN NULL ELSE $1 - $2 END $$
LANGUAGE SQL STRICT;

CREATE AGGREGATE sum_int_randomrestart (int4)
(
	stype = int4,
	sfunc = int4pl,
	mstype = int4,
	msfunc = int4pl,
	minvfunc = sum_int_randrestart_minvfunc
);

-- In PostgreSQL, the 'vs' CTE is constructed using random() and
-- generate_series(), but GPDB inlines CTEs even when they contain volatile
-- expressions, causing incorrect results. That's a bug in GPDB, of course,
-- but for the purposes of this test, we work around that by using a
-- non-volatile WITH clause. The list of values below was created by running
-- the original subquery using random() once, and copying the result here.
--
-- See https://github.com/greenplum-db/gpdb/issues/1349
WITH
vs (i, v) AS (
VALUES
 ( 1, 18),
 ( 2, 91),
 ( 3, 62),
 ( 4, 34),
 ( 5, 12),
 ( 6, 99),
 ( 7,  4),
 ( 8, 32),
 ( 9, 75),
 (10, 38),
 (11,  0),
 (12, 43),
 (13, 95),
 (14, 83),
 (15, 99),
 (16, 44),
 (17, 27),
 (18, 11),
 (19, 27),
 (20, 19),
 (21, 71),
 (22, 52),
 (23, 49),
 (24, 58),
 (25, 35),
 (26, 66),
 (27, 12),
 (28, 49),
 (29,  9),
 (30, 89),
 (31,  7),
 (32, 27),
 (33, 80),
 (34, 69),
 (35, 61),
 (36, 92),
 (37, 68),
 (38, 65),
 (39, 23),
 (40, 43),
 (41,  3),
 (42, 24),
 (43, 86),
 (44, 98),
 (45,  6),
 (46, 85),
 (47, 42),
 (48, 33),
 (49, 96),
 (50, 68),
 (51, 52),
 (52, 67),
 (53, 20),
 (54,  1),
 (55, 25),
 (56, 55),
 (57, 67),
 (58, 37),
 (59,  4),
 (60, 76),
 (61, 26),
 (62, 11),
 (63,  3),
 (64,  6),
 (65, 80),
 (66, 64),
 (67, 98),
 (68, 48),
 (69, 29),
 (70, 21),
 (71, 91),
 (72, 31),
 (73, 45),
 (74, 77),
 (75, 29),
 (76, 51),
 (77, 63),
 (78, 71),
 (79, 84),
 (80, 59),
 (81, 39),
 (82, 36),
 (83, 26),
 (84, 60),
 (85, 37),
 (86, 51),
 (87, 15),
 (88,  4),
 (89, 88),
 (90, 19),
 (91, 80),
 (92, 14),
 (93, 30),
 (94, 83),
 (95, 20),
 (96, 10),
 (97, 47),
 (98, 18),
 (99, 58),
(100, 75)
),
sum_following AS (
	SELECT i, SUM(v) OVER
		(ORDER BY i DESC ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS s
	FROM vs
)
SELECT DISTINCT
	sum_following.s = sum_int_randomrestart(v) OVER fwd AS eq1,
	-sum_following.s = sum_int_randomrestart(-v) OVER fwd AS eq2,
	100*3+(vs.i-1)*3 = length(logging_agg_nonstrict(''::text) OVER fwd) AS eq3
FROM vs
JOIN sum_following ON sum_following.i = vs.i
WINDOW fwd AS (
	ORDER BY vs.i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
);

--
-- Test various built-in aggregates that have moving-aggregate support
--

-- test inverse transition functions handle NULLs properly
SELECT i,AVG(v::bigint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,1),(2,2),(3,NULL),(4,NULL)) t(i,v);

SELECT i,AVG(v::int) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,1),(2,2),(3,NULL),(4,NULL)) t(i,v);

SELECT i,AVG(v::smallint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,1),(2,2),(3,NULL),(4,NULL)) t(i,v);

SELECT i,AVG(v::numeric) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,1.5),(2,2.5),(3,NULL),(4,NULL)) t(i,v);

SELECT i,AVG(v::interval) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,'1 sec'),(2,'2 sec'),(3,NULL),(4,NULL)) t(i,v);

SELECT i,SUM(v::smallint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,1),(2,2),(3,NULL),(4,NULL)) t(i,v);

SELECT i,SUM(v::int) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,1),(2,2),(3,NULL),(4,NULL)) t(i,v);

SELECT i,SUM(v::bigint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,1),(2,2),(3,NULL),(4,NULL)) t(i,v);

SELECT i,SUM(v::money) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,'1.10'),(2,'2.20'),(3,NULL),(4,NULL)) t(i,v);

SELECT i,SUM(v::interval) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,'1 sec'),(2,'2 sec'),(3,NULL),(4,NULL)) t(i,v);

SELECT i,SUM(v::numeric) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,1.1),(2,2.2),(3,NULL),(4,NULL)) t(i,v);

SELECT SUM(n::numeric) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,1.01),(2,2),(3,3)) v(i,n);

SELECT i,COUNT(v) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,1),(2,2),(3,NULL),(4,NULL)) t(i,v);

SELECT i,COUNT(*) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,1),(2,2),(3,NULL),(4,NULL)) t(i,v);

SELECT VAR_POP(n::bigint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT VAR_POP(n::int) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT VAR_POP(n::smallint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT VAR_POP(n::numeric) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT VAR_SAMP(n::bigint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT VAR_SAMP(n::int) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT VAR_SAMP(n::smallint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT VAR_SAMP(n::numeric) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT VARIANCE(n::bigint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT VARIANCE(n::int) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT VARIANCE(n::smallint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT VARIANCE(n::numeric) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT STDDEV_POP(n::bigint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,NULL),(2,600),(3,470),(4,170),(5,430),(6,300)) r(i,n);

SELECT STDDEV_POP(n::int) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,NULL),(2,600),(3,470),(4,170),(5,430),(6,300)) r(i,n);

SELECT STDDEV_POP(n::smallint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,NULL),(2,600),(3,470),(4,170),(5,430),(6,300)) r(i,n);

SELECT STDDEV_POP(n::numeric) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,NULL),(2,600),(3,470),(4,170),(5,430),(6,300)) r(i,n);

SELECT STDDEV_SAMP(n::bigint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,NULL),(2,600),(3,470),(4,170),(5,430),(6,300)) r(i,n);

SELECT STDDEV_SAMP(n::int) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,NULL),(2,600),(3,470),(4,170),(5,430),(6,300)) r(i,n);

SELECT STDDEV_SAMP(n::smallint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,NULL),(2,600),(3,470),(4,170),(5,430),(6,300)) r(i,n);

SELECT STDDEV_SAMP(n::numeric) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,NULL),(2,600),(3,470),(4,170),(5,430),(6,300)) r(i,n);

SELECT STDDEV(n::bigint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(0,NULL),(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT STDDEV(n::int) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(0,NULL),(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT STDDEV(n::smallint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(0,NULL),(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT STDDEV(n::numeric) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(0,NULL),(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

-- test that inverse transition functions work with various frame options
SELECT i,SUM(v::int) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND CURRENT ROW)
  FROM (VALUES(1,1),(2,2),(3,NULL),(4,NULL)) t(i,v);

SELECT i,SUM(v::int) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND 1 FOLLOWING)
  FROM (VALUES(1,1),(2,2),(3,NULL),(4,NULL)) t(i,v);

SELECT i,SUM(v::int) OVER (ORDER BY i ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING)
  FROM (VALUES(1,1),(2,2),(3,3),(4,4)) t(i,v);

-- ensure aggregate over numeric properly recovers from NaN values
SELECT a, b,
       SUM(b) OVER(ORDER BY A ROWS BETWEEN 1 PRECEDING AND CURRENT ROW)
FROM (VALUES(1,1::numeric),(2,2),(3,'NaN'),(4,3),(5,4)) t(a,b);

-- It might be tempting for someone to add an inverse trans function for
-- float and double precision. This should not be done as it can give incorrect
-- results. This test should fail if anyone ever does this without thinking too
-- hard about it.
SELECT to_char(SUM(n::float8) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND 1 FOLLOWING),'999999999999999999999D9')
  FROM (VALUES(1,1e20),(2,1)) n(i,n);

SELECT i, b, bool_and(b) OVER w, bool_or(b) OVER w
  FROM (VALUES (1,true), (2,true), (3,false), (4,false), (5,true)) v(i,b)
  WINDOW w AS (ORDER BY i ROWS BETWEEN CURRENT ROW AND 1 FOLLOWING);

-- Tests for problems with failure to walk or mutate expressions
-- within window frame clauses.

-- test walker (fails with collation error if expressions are not walked)
SELECT array_agg(i) OVER w
  FROM generate_series(1,5) i
WINDOW w AS (ORDER BY i ROWS BETWEEN (('foo' < 'foobar')::integer) PRECEDING AND CURRENT ROW);

-- test mutator (fails when inlined if expressions are not mutated)
CREATE FUNCTION pg_temp.f(group_size BIGINT) RETURNS SETOF integer[]
AS $$
    SELECT array_agg(s) OVER w
      FROM generate_series(1,5) s
    WINDOW w AS (ORDER BY s ROWS BETWEEN CURRENT ROW AND GROUP_SIZE FOLLOWING)
$$ LANGUAGE SQL STABLE;

EXPLAIN (costs off) SELECT * FROM pg_temp.f(2);
SELECT * FROM pg_temp.f(2);
RESET optimizer_trace_fallback;
