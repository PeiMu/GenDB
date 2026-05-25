/* Q8b */
SELECT
  MIN(an.name) AS acress_pseudonym,
  MIN(t.title) AS japanese_anime_movie
FROM aka_name AS an, cast_info AS ci, company_name AS cn, movie_companies AS mc, name AS n, role_type AS rt, title AS t
WHERE
  ci.note = %(note_eq)s
  AND cn.country_code = %(country_code_eq)s
  AND mc.note LIKE %(note_pattern_2)s
  AND mc.note NOT LIKE %(note_pattern)s
  AND (
    mc.note LIKE %(note_pattern_3)s OR mc.note LIKE %(note_pattern_4)s
  )
  AND n.name LIKE %(name_pattern_2)s
  AND n.name NOT LIKE %(name_pattern)s
  AND rt.role = %(role_eq)s
  AND t.production_year BETWEEN %(production_year_lower)s AND %(production_year_upper)s
  AND (
    t.title LIKE %(title_pattern)s OR t.title LIKE %(title_pattern_2)s
  )
  AND an.person_id = n.id
  AND n.id = ci.person_id
  AND ci.movie_id = t.id
  AND t.id = mc.movie_id
  AND mc.company_id = cn.id
  AND ci.role_id = rt.id
  AND an.person_id = ci.person_id
  AND ci.movie_id = mc.movie_id
