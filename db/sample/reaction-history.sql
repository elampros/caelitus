-- Sample like/dislike history for the last 60 days, relative to the day the
-- script is loaded (so "today" and "last 7 days" are never empty).
-- Deterministic: CRC32 instead of RAND(), so every load produces the same
-- numbers for the same day. Only books with reactions enabled get reactions.
--
-- Per book: a popularity of 1..30 decides how many likes a day it gets; about
-- one book in eight is "disliked" (mostly dislikes), so both rankings
-- (most liked, most disliked) have something to show. About 60% of the days
-- have activity. Uses MariaDB's built-in sequence table seq_0_to_59.

INSERT INTO book_reactions_daily (book_id, day, likes, dislikes)
SELECT id, day,
       IF(disliked, r % 3, r % (popularity + 1)),
       IF(disliked, (r DIV 7) % (popularity + 1), (r DIV 7) % (1 + popularity DIV 6))
FROM (SELECT b.id,
             CURDATE() - INTERVAL s.seq DAY AS day,
             CRC32(CONCAT(b.id, ':', s.seq)) AS r,
             1 + CRC32(b.title) % 30 AS popularity,
             CRC32(CONCAT('disliked:', b.title)) % 8 = 0 AS disliked
      FROM books b
      JOIN seq_0_to_59 s
      WHERE b.reactions_enabled) AS days
WHERE r % 10 < 6;

DELETE FROM book_reactions_daily WHERE likes = 0 AND dislikes = 0;

-- All-time totals on the book row, as the server keeps them.
UPDATE books b
JOIN (SELECT book_id, SUM(likes) AS likes, SUM(dislikes) AS dislikes
      FROM book_reactions_daily
      GROUP BY book_id) t ON t.book_id = b.id
SET b.likes = t.likes, b.dislikes = t.dislikes;
