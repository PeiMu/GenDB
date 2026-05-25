/* Q20b */
SELECT
  MIN(t.title) AS complete_downey_ironman_movie
FROM complete_cast AS cc, comp_cast_type AS cct1, comp_cast_type AS cct2, char_name AS chn, cast_info AS ci, keyword AS k, kind_type AS kt, movie_keyword AS mk, name AS n, title AS t
WHERE
  cct1.kind = %(kind_eq_2)s
  AND cct2.kind LIKE %(kind_pattern)s
  AND chn.name NOT LIKE %(name_pattern_2)s
  AND (
    chn.name LIKE %(name_pattern_3)s OR chn.name LIKE %(name_pattern_4)s
  )
  AND k.keyword IN (
    'superhero',
    'sequel',
    'second-part',
    'marvel-comics',
    'based-on-comic',
    'tv-special',
    'fight',
    'violence'
  )
  AND kt.kind = %(kind_eq)s
  AND n.name LIKE %(name_pattern)s
  AND t.production_year > %(production_year_lower)s
  AND kt.id = t.kind_id
  AND t.id = mk.movie_id
  AND t.id = ci.movie_id
  AND t.id = cc.movie_id
  AND mk.movie_id = ci.movie_id
  AND mk.movie_id = cc.movie_id
  AND ci.movie_id = cc.movie_id
  AND chn.id = ci.person_role_id
  AND n.id = ci.person_id
  AND k.id = mk.keyword_id
  AND cct1.id = cc.subject_id
  AND cct2.id = cc.status_id
