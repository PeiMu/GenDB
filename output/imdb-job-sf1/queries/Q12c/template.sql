/* Q12c */
SELECT
  MIN(cn.name) AS movie_company,
  MIN(mi_idx.info) AS rating,
  MIN(t.title) AS mainstream_movie
FROM company_name AS cn, company_type AS ct, info_type AS it1, info_type AS it2, movie_companies AS mc, movie_info AS mi, movie_info_idx AS mi_idx, title AS t
WHERE
  cn.country_code = %(country_code_eq)s
  AND ct.kind = %(kind_eq)s
  AND it1.info = %(info_eq_2)s
  AND it2.info = %(info_eq)s
  AND mi.info IN ('Drama', 'Horror', 'Western', 'Family')
  AND mi_idx.info > %(info_lower)s
  AND t.production_year BETWEEN %(production_year_lower)s AND %(production_year_upper)s
  AND t.id = mi.movie_id
  AND t.id = mi_idx.movie_id
  AND mi.info_type_id = it1.id
  AND mi_idx.info_type_id = it2.id
  AND t.id = mc.movie_id
  AND ct.id = mc.company_type_id
  AND cn.id = mc.company_id
  AND mc.movie_id = mi.movie_id
  AND mc.movie_id = mi_idx.movie_id
  AND mi.movie_id = mi_idx.movie_id
