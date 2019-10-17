-- show tables
\d

-- show queries
SELECT query FROM pg_stat_activity WHERE query NOT LIKE '%pg_stat_activity%' AND query NOT LIKE '%gp_acquire_stone%';
