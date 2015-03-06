\setrandom aid 1 `100000 * $tps`
\setrandom bid 1 `1 * $tps`
\setrandom tid 1 `10 * $tps`
\setrandom delta 1 1000
BEGIN
UPDATE accounts SET abalance = abalance + :delta WHERE aid = :aid
SELECT abalance FROM accounts WHERE aid = :aid
UPDATE tellers SET tbalance = tbalance + :delta WHERE tid = :tid
UPDATE branches SET bbalance = bbalance + :delta WHERE bid = :bid
INSERT INTO history (tid, bid, aid, delta, mtime) VALUES (:tid, :bid, :aid, :delta, current_timestamp)
END
