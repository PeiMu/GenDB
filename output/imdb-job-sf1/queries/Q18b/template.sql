/* Q18b */
SELECT
  MIN(mi.info) AS movie_budget,
  MIN(mi_idx.info) AS movie_votes,
  MIN(t.title) AS movie_title
FROM cast_info AS ci, info_type AS it1, info_type AS it2, movie_info AS mi, movie_info_idx AS mi_idx, name AS n, title AS t
WHERE
  ci.note IN ('(writer)', '(head writer)', '(written by)', '(story)', '(story editor)')
  AND it1.info = %(info_eq_2)s
  AND it2.info = %(info_eq)s
  AND mi.info IN ('Horror', 'Thriller')
  AND mi.note IS NULL
  AND mi_idx.info > %(info_lower)s
  AND NOT n.gender IS NULL
  AND n.gender = %(gender_eq)s
  AND t.production_year BETWEEN %(production_year_lower)s AND %(production_year_upper)s
  AND t.id = mi.movie_id
  AND t.id = mi_idx.movie_id
  AND t.id = ci.movie_id
  AND ci.movie_id = mi.movie_id
  AND ci.movie_id = mi_idx.movie_id
  AND mi.movie_id = mi_idx.movie_id
  AND n.id = ci.person_id
  AND it1.id = mi.info_type_id
  AND it2.id = mi_idx.info_type_id
