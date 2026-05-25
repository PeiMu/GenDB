/* Q19b */
SELECT
  MIN(n.name) AS voicing_actress,
  MIN(t.title) AS kung_fu_panda
FROM aka_name AS an, char_name AS chn, cast_info AS ci, company_name AS cn, info_type AS it, movie_companies AS mc, movie_info AS mi, name AS n, role_type AS rt, title AS t
WHERE
  ci.note = %(note_eq)s
  AND cn.country_code = %(country_code_eq)s
  AND it.info = %(info_eq)s
  AND mc.note LIKE %(note_pattern)s
  AND (
    mc.note LIKE %(note_pattern_2)s OR mc.note LIKE %(note_pattern_3)s
  )
  AND NOT mi.info IS NULL
  AND (
    mi.info LIKE %(info_pattern)s OR mi.info LIKE %(info_pattern_2)s
  )
  AND n.gender = %(gender_eq)s
  AND n.name LIKE %(name_pattern)s
  AND rt.role = %(role_eq)s
  AND t.production_year BETWEEN %(production_year_lower)s AND %(production_year_upper)s
  AND t.title LIKE %(title_pattern)s
  AND t.id = mi.movie_id
  AND t.id = mc.movie_id
  AND t.id = ci.movie_id
  AND mc.movie_id = ci.movie_id
  AND mc.movie_id = mi.movie_id
  AND mi.movie_id = ci.movie_id
  AND cn.id = mc.company_id
  AND it.id = mi.info_type_id
  AND n.id = ci.person_id
  AND rt.id = ci.role_id
  AND n.id = an.person_id
  AND ci.person_id = an.person_id
  AND chn.id = ci.person_role_id
