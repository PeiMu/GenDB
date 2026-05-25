/* Q5b */
SELECT
  MIN(t.title) AS american_vhs_movie
FROM company_type AS ct, info_type AS it, movie_companies AS mc, movie_info AS mi, title AS t
WHERE
  ct.kind = %(kind_eq)s
  AND mc.note LIKE %(note_pattern_3)s
  AND mc.note LIKE %(note_pattern_2)s
  AND mc.note LIKE %(note_pattern)s
  AND mi.info IN ('USA', 'America')
  AND t.production_year > %(production_year_lower)s
  AND t.id = mi.movie_id
  AND t.id = mc.movie_id
  AND mc.movie_id = mi.movie_id
  AND ct.id = mc.company_type_id
  AND it.id = mi.info_type_id
