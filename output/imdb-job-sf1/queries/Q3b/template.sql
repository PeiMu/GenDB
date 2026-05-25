/* Q3b */
SELECT
  MIN(t.title) AS movie_title
FROM keyword AS k, movie_info AS mi, movie_keyword AS mk, title AS t
WHERE
  k.keyword LIKE %(keyword_pattern)s
  AND mi.info IN ('Bulgaria')
  AND t.production_year > %(production_year_lower)s
  AND t.id = mi.movie_id
  AND t.id = mk.movie_id
  AND mk.movie_id = mi.movie_id
  AND k.id = mk.keyword_id
