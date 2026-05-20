## SQL

```sql
SELECT MIN(n.name) AS voicing_actress,
       MIN(t.title) AS jap_engl_voiced_movie
FROM aka_name AS an, char_name AS chn, cast_info AS ci,
     company_name AS cn, info_type AS it, movie_companies AS mc,
     movie_info AS mi, name AS n, role_type AS rt, title AS t
WHERE ci.note IN ('(voice)','(voice: Japanese version)','(voice) (uncredited)','(voice: English version)')
  AND cn.country_code='[us]'
  AND it.info='release dates'
  AND n.gender='f'
  AND rt.role='actress'
  AND t.production_year > 2000
  AND t.id=mi.movie_id AND t.id=mc.movie_id AND t.id=ci.movie_id
  AND cn.id=mc.company_id AND it.id=mi.info_type_id
  AND n.id=ci.person_id AND rt.id=ci.role_id
  AND n.id=an.person_id AND chn.id=ci.person_role_id;
```

## Column Reference

Same columns as Q19a/c. Differences:
- No name LIKE.
- No mi.info LIKE; mi only constrained by `info_type_id==it_rd`.
- No mc.note filter.

### role_type.role, info_type.info, company_name.country_code, name.gender
Resolve `rt_actress`, `it_rd`, `cc_us`, `g_f`.

### name.gender (int8 dict)
Files: `name/gender.bin` (4167491). Filter `n_gender[i]==g_f` → ~half of names.

### cast_info.note, person_id, movie_id, role_id, person_role_id
4-string IN-list voice notes.

### movie_companies.movie_id, company_id (no note)
Files: `movie_companies/movie_id.bin`, `.../company_id.bin`.

### movie_info.info_type_id, movie_id (no info LIKE)
Files: `movie_info/info_type_id.bin`, `.../movie_id.bin`. Existence check only — need at least one row with `it_rd`.

### title.id, title, production_year
`production_year > 2000` AND `!= INT32_MIN`.

### aka_name.person_id, char_name
aka_name existence via index; char_name only via FK non-NULL.

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| role_type | 12 | id | dense-PK |
| info_type | 113 | id | dense-PK |
| company_name | 234,997 | id | dense-PK |
| name | 4,167,491 | id | dense-PK |
| aka_name | 901,343 | person_id | offsets_only |
| char_name | 3,140,339 | id | dense-PK |
| cast_info | 36,244,344 | movie_id | offsets_only movie_id; CSR person_id/role_id |
| movie_companies | 2,609,129 | movie_id | offsets_only movie_id; CSR company_id |
| movie_info | 14,835,720 | movie_id | offsets_only movie_id |
| title | 2,528,312 | id | dense-PK |

## Query Analysis

Join graph same as Q19a/b/c. Weakest filters of the four; expect large intermediate.

Plan:
1. Resolve dim ids.
2. Scan title once: T = {mv : production_year[mv-1] > 2000 AND != INT32_MIN}. ~half the titles.
3. For each `mv` in T:
   - mi range via `movie_info__movie_id`: need any row with `info_type_id==it_rd`. (Skip movies without release-dates info.)
   - mc range via `movie_companies__movie_id`: need any row with `cn_country_code[company_id-1]==cc_us`.
   - ci range via `cast_info__movie_id`: for each ci row test `note ∈ voice-set` AND `role_id==rt_actress` AND `person_role_id!=INT32_MIN`; pid=ci.person_id; check `n_gender[pid-1]==g_f`; check aka_name range non-empty.
   - Update MIN(name[pid-1]), MIN(title[mv-1]).

Selectivities:
- year > 2000 → ~half of titles.
- mi it_rd existence per movie → fairly common.
- mc country='[us]' → many movies.
- ci.note IN 4 voice strings + role_id==rt_actress → small.
- n.gender='f' → ~half of names.

Aggressive shortcut: scan `cast_info` once linearly. For each ci row r where `note ∈ voice-set` AND `role_id==rt_actress`: pid=person_id[r], mv=movie_id[r]. Apply title.production_year>2000 (lookup), n_gender (lookup), then probe mi and mc movie ranges for existence. This skips the entire title scan and is typically faster when ci.note IN voice-set is small.

LIKE notes: none — all equality / IN / range.

## Indexes

### cast_info__movie_id (offsets_only)
File: `_idx/cast_info__movie_id__offsets.bin` (int32, 2528314).

### movie_companies__movie_id (offsets_only)
File: `_idx/movie_companies__movie_id__offsets.bin`.

### movie_info__movie_id (offsets_only)
File: `_idx/movie_info__movie_id__offsets.bin`.

### movie_info__info_type_id (CSR) — alt driver
Files: `_idx/movie_info__info_type_id__offsets.bin`, `_idx/movie_info__info_type_id__rowids.bin`. Enumerate movies that have a release-dates info row.

### aka_name__person_id (offsets_only)
File: `_idx/aka_name__person_id__offsets.bin`. Existence check `off[pid+1] > off[pid]`.
