/* Q26a */
SELECT
  MIN(chn.name) AS character_name,
  MIN(mi_idx.info) AS rating,
  MIN(n.name) AS playing_actor,
  MIN(t.title) AS complete_hero_movie
FROM complete_cast AS cc, comp_cast_type AS cct1, comp_cast_type AS cct2, char_name AS chn, cast_info AS ci, info_type AS it2, keyword AS k, kind_type AS kt, movie_info_idx AS mi_idx, movie_keyword AS mk, name AS n, title AS t
WHERE
  cct1.kind = %(kind_eq_2)s
  AND cct2.kind LIKE %(kind_pattern)s
  AND NOT chn.name IS NULL
  AND (
    chn.name LIKE %(name_pattern)s OR chn.name LIKE %(name_pattern_2)s
  )
  AND it2.info = %(info_eq)s
  AND k.keyword IN (
    'superhero',
    'marvel-comics',
    'based-on-comic',
    'tv-special',
    'fight',
    'violence',
    'magnet',
    'web',
    'claw',
    'laser'
  )
  AND kt.kind = %(kind_eq)s
  AND mi_idx.info > %(info_lower)s
  AND t.production_year > %(production_year_lower)s
  AND kt.id = t.kind_id
  AND t.id = mk.movie_id
  AND t.id = ci.movie_id
  AND t.id = cc.movie_id
  AND t.id = mi_idx.movie_id
  AND mk.movie_id = ci.movie_id
  AND mk.movie_id = cc.movie_id
  AND mk.movie_id = mi_idx.movie_id
  AND ci.movie_id = cc.movie_id
  AND ci.movie_id = mi_idx.movie_id
  AND cc.movie_id = mi_idx.movie_id
  AND chn.id = ci.person_role_id
  AND n.id = ci.person_id
  AND k.id = mk.keyword_id
  AND cct1.id = cc.subject_id
  AND cct2.id = cc.status_id
  AND it2.id = mi_idx.info_type_id
