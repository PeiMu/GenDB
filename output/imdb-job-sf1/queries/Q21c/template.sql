/* Q21c */
SELECT
  MIN(cn.name) AS company_name,
  MIN(lt.link) AS link_type,
  MIN(t.title) AS western_follow_up
FROM company_name AS cn, company_type AS ct, keyword AS k, link_type AS lt, movie_companies AS mc, movie_info AS mi, movie_keyword AS mk, movie_link AS ml, title AS t
WHERE
  cn.country_code <> %(country_code_neq)s
  AND (
    cn.name LIKE %(name_pattern)s OR cn.name LIKE %(name_pattern_2)s
  )
  AND ct.kind = %(kind_eq)s
  AND k.keyword = %(keyword_eq)s
  AND lt.link LIKE %(link_pattern)s
  AND mc.note IS NULL
  AND mi.info IN (
    'Sweden',
    'Norway',
    'Germany',
    'Denmark',
    'Swedish',
    'Denish',
    'Norwegian',
    'German',
    'English'
  )
  AND t.production_year BETWEEN %(production_year_lower)s AND %(production_year_upper)s
  AND lt.id = ml.link_type_id
  AND ml.movie_id = t.id
  AND t.id = mk.movie_id
  AND mk.keyword_id = k.id
  AND t.id = mc.movie_id
  AND mc.company_type_id = ct.id
  AND mc.company_id = cn.id
  AND mi.movie_id = t.id
  AND ml.movie_id = mk.movie_id
  AND ml.movie_id = mc.movie_id
  AND mk.movie_id = mc.movie_id
  AND ml.movie_id = mi.movie_id
  AND mk.movie_id = mi.movie_id
  AND mc.movie_id = mi.movie_id
