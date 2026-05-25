/* Q14b */
SELECT
  MIN(mi_idx.info) AS rating,
  MIN(t.title) AS western_dark_production
FROM info_type AS it1, info_type AS it2, keyword AS k, kind_type AS kt, movie_info AS mi, movie_info_idx AS mi_idx, movie_keyword AS mk, title AS t
WHERE
  it1.info = %(info_eq)s
  AND it2.info = %(info_eq_2)s
  AND k.keyword IN ('murder', 'murder-in-title')
  AND kt.kind = %(kind_eq)s
  AND mi.info IN (
    'Sweden',
    'Norway',
    'Germany',
    'Denmark',
    'Swedish',
    'Denish',
    'Norwegian',
    'German',
    'USA',
    'American'
  )
  AND mi_idx.info > %(info_lower)s
  AND t.production_year > %(production_year_lower)s
  AND (
    t.title LIKE %(title_pattern_2)s
    OR t.title LIKE %(title_pattern_3)s
    OR t.title LIKE %(title_pattern)s
  )
  AND kt.id = t.kind_id
  AND t.id = mi.movie_id
  AND t.id = mk.movie_id
  AND t.id = mi_idx.movie_id
  AND mk.movie_id = mi.movie_id
  AND mk.movie_id = mi_idx.movie_id
  AND mi.movie_id = mi_idx.movie_id
  AND k.id = mk.keyword_id
  AND it1.id = mi.info_type_id
  AND it2.id = mi_idx.info_type_id
