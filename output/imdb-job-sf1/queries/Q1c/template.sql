/* Q1c */
SELECT
  MIN(mc.note) AS production_note,
  MIN(t.title) AS movie_title,
  MIN(t.production_year) AS movie_year
FROM company_type AS ct, info_type AS it, movie_companies AS mc, movie_info_idx AS mi_idx, title AS t
WHERE
  ct.kind = %(kind_eq)s
  AND it.info = %(info_eq)s
  AND mc.note NOT LIKE %(note_pattern)s
  AND (
    mc.note LIKE %(note_pattern_2)s
  )
  AND t.production_year > %(production_year_lower)s
  AND ct.id = mc.company_type_id
  AND t.id = mc.movie_id
  AND t.id = mi_idx.movie_id
  AND mc.movie_id = mi_idx.movie_id
  AND it.id = mi_idx.info_type_id
