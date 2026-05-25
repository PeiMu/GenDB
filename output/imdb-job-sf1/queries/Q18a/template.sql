/* Q18a */
SELECT
  MIN(mi.info) AS movie_budget,
  MIN(mi_idx.info) AS movie_votes,
  MIN(t.title) AS movie_title
FROM cast_info AS ci, info_type AS it1, info_type AS it2, movie_info AS mi, movie_info_idx AS mi_idx, name AS n, title AS t
WHERE
  ci.note IN ('(producer)', '(executive producer)')
  AND it1.info = %(info_eq_2)s
  AND it2.info = %(info_eq)s
  AND n.gender = %(gender_eq)s
  AND n.name LIKE %(name_pattern)s
  AND t.id = mi.movie_id
  AND t.id = mi_idx.movie_id
  AND t.id = ci.movie_id
  AND ci.movie_id = mi.movie_id
  AND ci.movie_id = mi_idx.movie_id
  AND mi.movie_id = mi_idx.movie_id
  AND n.id = ci.person_id
  AND it1.id = mi.info_type_id
  AND it2.id = mi_idx.info_type_id
