/* Q15c */
SELECT
  MIN(mi.info) AS release_date,
  MIN(t.title) AS modern_american_internet_movie
FROM aka_title AS at1, company_name AS cn, company_type AS ct, info_type AS it1, keyword AS k, movie_companies AS mc, movie_info AS mi, movie_keyword AS mk, title AS t
WHERE
  cn.country_code = %(country_code_eq)s
  AND it1.info = %(info_eq)s
  AND mi.note LIKE %(note_pattern)s
  AND NOT mi.info IS NULL
  AND (
    mi.info LIKE %(info_pattern)s OR mi.info LIKE %(info_pattern_2)s
  )
  AND t.production_year > %(production_year_lower)s
  AND t.id = at1.movie_id
  AND t.id = mi.movie_id
  AND t.id = mk.movie_id
  AND t.id = mc.movie_id
  AND mk.movie_id = mi.movie_id
  AND mk.movie_id = mc.movie_id
  AND mk.movie_id = at1.movie_id
  AND mi.movie_id = mc.movie_id
  AND mi.movie_id = at1.movie_id
  AND mc.movie_id = at1.movie_id
  AND k.id = mk.keyword_id
  AND it1.id = mi.info_type_id
  AND cn.id = mc.company_id
  AND ct.id = mc.company_type_id
