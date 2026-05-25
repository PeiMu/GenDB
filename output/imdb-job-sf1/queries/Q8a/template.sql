/* Q8a */
SELECT
  MIN(an1.name) AS actress_pseudonym,
  MIN(t.title) AS japanese_movie_dubbed
FROM aka_name AS an1, cast_info AS ci, company_name AS cn, movie_companies AS mc, name AS n1, role_type AS rt, title AS t
WHERE
  ci.note = %(note_eq)s
  AND cn.country_code = %(country_code_eq)s
  AND mc.note LIKE %(note_pattern_2)s
  AND mc.note NOT LIKE %(note_pattern)s
  AND n1.name LIKE %(name_pattern_2)s
  AND n1.name NOT LIKE %(name_pattern)s
  AND rt.role = %(role_eq)s
  AND an1.person_id = n1.id
  AND n1.id = ci.person_id
  AND ci.movie_id = t.id
  AND t.id = mc.movie_id
  AND mc.company_id = cn.id
  AND ci.role_id = rt.id
  AND an1.person_id = ci.person_id
  AND ci.movie_id = mc.movie_id
