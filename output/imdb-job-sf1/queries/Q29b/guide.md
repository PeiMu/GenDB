# Q29b Guide

## SQL
```sql
SELECT MIN(chn.name) AS voiced_char,
       MIN(n.name) AS voicing_actress,
       MIN(t.title) AS voiced_animation
FROM aka_name AS an, complete_cast AS cc, comp_cast_type AS cct1, comp_cast_type AS cct2,
     char_name AS chn, cast_info AS ci, company_name AS cn, info_type AS it,
     info_type AS it3, keyword AS k, movie_companies AS mc, movie_info AS mi,
     movie_keyword AS mk, name AS n, person_info AS pi, role_type AS rt, title AS t
WHERE cct1.kind = 'cast' AND cct2.kind = 'complete+verified'
  AND chn.name = 'Queen'
  AND ci.note IN ('(voice)','(voice) (uncredited)','(voice: English version)')
  AND cn.country_code = '[us]'
  AND it.info = 'release dates' AND it3.info = 'height'
  AND k.keyword = 'computer-animation'
  AND mi.info LIKE 'USA:%200%'
  AND n.gender = 'f' AND n.name LIKE '%An%'
  AND rt.role = 'actress'
  AND t.title = 'Shrek 2'
  AND t.production_year BETWEEN 2000 AND 2005
  AND t.id = mi.movie_id AND t.id = mc.movie_id AND t.id = ci.movie_id
  AND t.id = mk.movie_id AND t.id = cc.movie_id
  AND cn.id = mc.company_id AND it.id = mi.info_type_id
  AND n.id = ci.person_id AND rt.id = ci.role_id
  AND n.id = an.person_id AND chn.id = ci.person_role_id
  AND n.id = pi.person_id AND it3.id = pi.info_type_id
  AND k.id = mk.keyword_id
  AND cct1.id = cc.subject_id AND cct2.id = cc.status_id;
```

## Column Reference

### comp_cast_type.kind (filter, varlen)
- Files: `comp_cast_type/kind.off`, `comp_cast_type/kind.dat`; rows: 4.
- Resolve `cast_id` and `verified_id` (==`'complete+verified'`).

### char_name.name (filter, varlen) — EXACT `'Queen'`
- Files: `char_name/name.off`, `char_name/name.dat`; rows: 3140339; dense PK.
- Build `chn_set` of all row indices i where bytes equal `"Queen"` exactly; id = i+1.

### cast_info.movie_id / .person_id / .person_role_id / .role_id / .note
- Files: `cast_info/movie_id.bin`, `cast_info/person_id.bin`, `cast_info/person_role_id.bin`, `cast_info/role_id.bin`, `cast_info/note.off`+`.dat`. Rows: 36244344.
- Filter: `person_role_id ∈ chn_set`, `role_id == actress_id`, `note ∈ 3-literal set`.

### company_name.name / .country_code
- Files: `company_name/name.off`+`.dat`; `company_name/country_code.bin`+`.dict.off`+`.dict.dat`; rows: 234997.
- Resolve `us_code` from dict; `cn_us_set = {ids with code == us_code}`.

### info_type.info (filter, varlen)
- Files: `info_type/info.off`, `info_type/info.dat`; rows: 113.
- Resolve `it_id` (==`'release dates'`) — used for `mi.info_type_id`.
- Resolve `it3_id` (==`'height'`) — used for `pi.info_type_id`. (Q29b differs from Q29a/c: height instead of trivia.)

### keyword.keyword (filter, varlen)
- Files: `keyword/keyword.off`, `keyword/keyword.dat`. Resolve `cam_kw_id` for `'computer-animation'`.

### movie_companies.movie_id / .company_id
- Files: `movie_companies/movie_id.bin`, `movie_companies/company_id.bin`.

### movie_info.movie_id / .info_type_id / .info
- Files: `movie_info/movie_id.bin`, `movie_info/info_type_id.bin`, `movie_info/info.off`+`.dat`.
- Filter: `info_type_id == it_id && info LIKE 'USA:%200%'` (starts with `USA:` then somewhere `200`).

### movie_keyword.movie_id / .keyword_id
- Files: `movie_keyword/movie_id.bin`, `movie_keyword/keyword_id.bin`. Filter `keyword_id == cam_kw_id`.

### complete_cast.movie_id / .subject_id / .status_id
- Files: `complete_cast/movie_id.bin`, `complete_cast/subject_id.bin`, `complete_cast/status_id.bin`.
- Filter: `subject_id == cast_id && status_id == verified_id`.

### name.name / .gender (filter+project)
- Files: `name/name.off`+`.dat`; `name/gender.bin`+`.dict.off`+`.dict.dat`; rows: 4167491.
- Resolve `f_code` from gender dict. Lazy per-candidate predicate: `gender_code==f_code` AND `name` contains `An`.

### aka_name.person_id (existence)
- File: `aka_name/person_id.bin`; rows: 901343; sorted.
- Probe via `aka_name__person_id` offsets_only.

### person_info.person_id / .info_type_id (existence + filter)
- Files: `person_info/person_id.bin`, `person_info/info_type_id.bin`; rows: 2963664; sorted by person_id.
- Two indexes available; **use `person_info__person_id` (offsets_only)** here because we have a small set of candidate persons (from Shrek 2 voice cast).

### role_type.role (filter, varlen)
- Files: `role_type/role.off`, `role_type/role.dat`; rows: 12. Resolve `actress_id`.

### title.id / .title / .production_year — `t.title='Shrek 2'`, year BETWEEN 2000 AND 2005
- Files: `title/id.bin`, `title/title.off`+`.dat`, `title/production_year.bin`; rows: 2528312.

## Table Stats
| Table | Rows | Role | Sort Order | Block Size |
|---|---|---|---|---|
| comp_cast_type | 4 | dim | id | n/a |
| info_type | 113 | dim | id | n/a |
| role_type | 12 | dim | id | n/a |
| keyword | 134170 | dim | id | 100000 |
| char_name | 3140339 | dim (filter) | id | 100000 |
| company_name | 234997 | dim (filter) | id | 100000 |
| name | 4167491 | dim (filter+project) | id | 100000 |
| aka_name | 901343 | existence | person_id | 100000 |
| title | 2528312 | very narrow driver | id | 100000 |
| complete_cast | 135086 | fact | movie_id | 100000 |
| movie_companies | 2609129 | fact | movie_id | 100000 |
| movie_keyword | 4523930 | fact | movie_id | 100000 |
| movie_info | 14835720 | fact | movie_id | 200000 |
| cast_info | 36244344 | fact | movie_id | 200000 |
| person_info | 2963664 | per-person probe | person_id | 100000 |

## Query Analysis
- Same structure as Q29a; key differences: `it3 = 'height'` (not trivia); `mi.info LIKE 'USA:%200%'` only (no Japan branch); year BETWEEN 2000 AND 2005 (tighter).
- Driver: scan title for `'Shrek 2'`. For each match: check year ∈ [2000,2005]; then probe per-movie facts.
- Build once: `cast_id`, `verified_id`, `f_code`, `us_code`, `actress_id`, `it_id`, `it3_id` (height), `cam_kw_id`, `chn_set`, `cn_us_set`, ci.note 3-literal set.
- Probe order: mk → cc → mc → mi → ci → per-person (n, an, pi).
- MIN: 3 outputs.
- LIKE notes: `USA:%200%` = prefix `USA:` then memmem `200` afterward; `n.name LIKE '%An%'` = memmem `An`.

## Indexes
- `movie_keyword__movie_id` (offsets_only): `_idx/movie_keyword__movie_id__offsets.bin`.
- `complete_cast__movie_id` (offsets_only): `_idx/complete_cast__movie_id__offsets.bin`.
- `movie_companies__movie_id` (offsets_only): `_idx/movie_companies__movie_id__offsets.bin`.
- `movie_info__movie_id` (offsets_only): `_idx/movie_info__movie_id__offsets.bin`.
- `cast_info__movie_id` (offsets_only): `_idx/cast_info__movie_id__offsets.bin`.
- `aka_name__person_id` (offsets_only): `_idx/aka_name__person_id__offsets.bin`.
- `person_info__person_id` (offsets_only): `_idx/person_info__person_id__offsets.bin`. Use this — NOT `person_info__info_type_id` — since candidate persons are few.

Usage:
```cpp
int32_t lo = off[key_id], hi = off[key_id + 1];
for (int32_t r = lo; r < hi; ++r) { /* row r */ }
```

Rules:
- Resolve `[us]`, `'f'`, `'height'`, `'release dates'` at runtime from dict / varlen.
- Varlen NULL = empty; dict NULL = code 0.
- `'Queen'` may match multiple chn rows — collect them all.
- Avoid scanning person_info globally; use per-person slice.
