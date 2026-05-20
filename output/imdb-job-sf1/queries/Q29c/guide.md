# Q29c Guide

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
  AND ci.note IN ('(voice)','(voice: Japanese version)',
                  '(voice) (uncredited)','(voice: English version)')
  AND cn.country_code = '[us]'
  AND it.info = 'release dates' AND it3.info = 'trivia'
  AND k.keyword = 'computer-animation'
  AND mi.info IS NOT NULL
  AND (mi.info LIKE 'Japan:%200%' OR mi.info LIKE 'USA:%200%')
  AND n.gender = 'f' AND n.name LIKE '%An%'
  AND rt.role = 'actress'
  AND t.production_year BETWEEN 2000 AND 2010
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
- Resolve `cast_id` and `verified_id`.

### char_name.name (join only — NO 'Queen' filter in Q29c)
- Files: `char_name/name.off`, `char_name/name.dat`; rows: 3140339; dense PK.
- Q29c has NO `chn.name='Queen'` predicate. char_name is joined only via `chn.id = ci.person_role_id` for projection MIN(chn.name). Read chn.name lazily for surviving ci rows.

### cast_info.movie_id / .person_id / .person_role_id / .role_id / .note
- Files: `cast_info/movie_id.bin`, `cast_info/person_id.bin`, `cast_info/person_role_id.bin` (nullable), `cast_info/role_id.bin`, `cast_info/note.off`+`.dat`. Rows: 36244344.
- Filter: `role_id == actress_id`, `note ∈ 4-literal set` (Q29c has 4 voice notes, including `'(voice: Japanese version)'`).
- `person_role_id` is non-NULL required (otherwise no chn projection); skip NULL_INT.

### company_name.name / .country_code
- Files: `company_name/name.off`+`.dat`; `company_name/country_code.bin`+`.dict.off`+`.dict.dat`; rows: 234997.
- Resolve `us_code`; build `cn_us_set`.

### info_type.info (filter, varlen)
- Files: `info_type/info.off`, `info_type/info.dat`; rows: 113.
- Resolve `it_id` (`'release dates'`) for `mi.info_type_id`; `it3_id` (`'trivia'`) for `pi.info_type_id`.

### keyword.keyword (filter, varlen)
- Files: `keyword/keyword.off`, `keyword/keyword.dat`. Resolve `cam_kw_id`.

### movie_companies.movie_id / .company_id
- Files: `movie_companies/movie_id.bin`, `movie_companies/company_id.bin`.

### movie_info.movie_id / .info_type_id / .info
- Files: `movie_info/movie_id.bin`, `movie_info/info_type_id.bin`, `movie_info/info.off`+`.dat`.
- Filter: `info_type_id == it_id && (info LIKE 'Japan:%200%' OR info LIKE 'USA:%200%')`. Also IS NOT NULL (empty entry).

### movie_keyword.movie_id / .keyword_id
- Files: `movie_keyword/movie_id.bin`, `movie_keyword/keyword_id.bin`.

### complete_cast.movie_id / .subject_id / .status_id
- Files: `complete_cast/movie_id.bin`, `complete_cast/subject_id.bin`, `complete_cast/status_id.bin`.

### name.name / .gender
- Files: `name/name.off`+`.dat`; `name/gender.bin`+`.dict.off`+`.dict.dat`; rows: 4167491.
- Resolve `f_code`. Lazy per-candidate predicate.

### aka_name.person_id (existence)
- File: `aka_name/person_id.bin`; rows: 901343; sorted.

### person_info.person_id / .info_type_id (existence + filter)
- Files: `person_info/person_id.bin`, `person_info/info_type_id.bin`; rows: 2963664; sorted by person_id.
- Two indexes available. Q29c has NO title or chn filter; t.production_year window only. Candidate movies could be MANY. Approach:
  1. Drive from keyword (cam) via `movie_keyword__keyword_id` CSR (small movie set ~few thousand).
  2. Per movie t_id, filter year ∈ [2000,2010]; then probe cc, mc, mi, ci.
  3. For each surviving ci.person_id, use `person_info__person_id` (offsets_only) for per-person pi slice and test `info_type_id == it3_id`.

### role_type.role (filter, varlen)
- Files: `role_type/role.off`, `role_type/role.dat`; rows: 12. Resolve `actress_id`.

### title.id / .title / .production_year (driver via mk)
- Files: `title/id.bin`, `title/title.off`+`.dat` (project), `title/production_year.bin`. Rows: 2528312.
- No title equality filter in Q29c; just year BETWEEN 2000 AND 2010.

## Table Stats
| Table | Rows | Role | Sort Order | Block Size |
|---|---|---|---|---|
| comp_cast_type | 4 | dim | id | n/a |
| info_type | 113 | dim | id | n/a |
| role_type | 12 | dim | id | n/a |
| keyword | 134170 | dim (driver entry) | id | 100000 |
| char_name | 3140339 | project | id | 100000 |
| company_name | 234997 | dim (filter) | id | 100000 |
| name | 4167491 | dim (filter+project) | id | 100000 |
| aka_name | 901343 | existence | person_id | 100000 |
| title | 2528312 | year filter | id | 100000 |
| complete_cast | 135086 | fact | movie_id | 100000 |
| movie_companies | 2609129 | fact | movie_id | 100000 |
| movie_keyword | 4523930 | driver via cam | movie_id | 100000 |
| movie_info | 14835720 | fact | movie_id | 200000 |
| cast_info | 36244344 | fact | movie_id | 200000 |
| person_info | 2963664 | per-person probe | person_id | 100000 |

## Query Analysis
- No title literal filter in Q29c — wider candidate movie set than Q29a/b. Best driver is keyword `computer-animation` via CSR.
- Build once: `cast_id`, `verified_id`, `f_code`, `us_code`, `actress_id`, `it_id`, `it3_id`, `cam_kw_id`, `cn_us_set`, ci.note 4-literal set.
- Plan:
  1. Use `movie_keyword__keyword_id` CSR with `cam_kw_id` → enumerate movies M.
  2. For each m: title year ∈ [2000,2010].
  3. complete_cast range with subject==cast_id, status==verified_id.
  4. movie_companies range with company_id ∈ cn_us_set.
  5. movie_info range with it_id and `LIKE 'Japan:%200%' OR LIKE 'USA:%200%'`.
  6. cast_info range with role==actress and note ∈ 4-set. Read person_role_id (skip NULL); lazily fetch chn.name for MIN.
  7. For each ci.person_id: lazy `gender_code==f_code && name contains 'An'`; existence in aka_name; existence in person_info with `info_type_id==it3_id` via per-person offsets_only.
- MIN: 3 outputs (chn.name, n.name, t.title).
- LIKE notes: prefix `Japan:`/`USA:` then memmem `200`; `%An%` substring.

## Indexes
- `movie_keyword__keyword_id` (CSR)
  - Files: `_idx/movie_keyword__keyword_id__offsets.bin` (int32, length 134172), `_idx/movie_keyword__keyword_id__rowids.bin` (int32, length 4523930).
  - Use: `lo=off[cam_kw_id]; hi=off[cam_kw_id+1]; for k in [lo,hi): mk_row=rowids[k]; t_id=mk_movie_id[mk_row]`.
- `movie_companies__movie_id` (offsets_only): `_idx/movie_companies__movie_id__offsets.bin`.
- `movie_info__movie_id` (offsets_only): `_idx/movie_info__movie_id__offsets.bin`.
- `complete_cast__movie_id` (offsets_only): `_idx/complete_cast__movie_id__offsets.bin`.
- `cast_info__movie_id` (offsets_only): `_idx/cast_info__movie_id__offsets.bin`.
- `aka_name__person_id` (offsets_only): `_idx/aka_name__person_id__offsets.bin`. Existence: `off[p+1]>off[p]`.
- `person_info__person_id` (offsets_only): `_idx/person_info__person_id__offsets.bin`. Per-person slice — preferred over the `__info_type_id` CSR because candidate persons are few.

Usage:
```cpp
int32_t lo = off[parent_id], hi = off[parent_id + 1];
for (int32_t r = lo; r < hi; ++r) { /* child row r */ }
```

Rules:
- Never invent indexes; use offsets_only and CSR per shared context.
- Resolve dict codes (`[us]`, `'f'`) at runtime; varlen NULL via empty entry; dict NULL via code 0.
- person_info has TWO indexes — use `person_info__person_id` here since we have a small candidate person set.
- ci.person_role_id may be NULL_INT — skip those rows for chn projection.
