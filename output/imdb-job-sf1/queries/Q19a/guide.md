## SQL

```sql
SELECT MIN(n.name) AS voicing_actress,
       MIN(t.title) AS voiced_movie
FROM aka_name AS an, char_name AS chn, cast_info AS ci,
     company_name AS cn, info_type AS it, movie_companies AS mc,
     movie_info AS mi, name AS n, role_type AS rt, title AS t
WHERE ci.note IN ('(voice)','(voice: Japanese version)','(voice) (uncredited)','(voice: English version)')
  AND cn.country_code='[us]'
  AND it.info='release dates'
  AND mc.note IS NOT NULL
  AND (mc.note LIKE '%(USA)%' OR mc.note LIKE '%(worldwide)%')
  AND mi.info IS NOT NULL
  AND (mi.info LIKE 'Japan:%200%' OR mi.info LIKE 'USA:%200%')
  AND n.gender='f' AND n.name LIKE '%Ang%'
  AND rt.role='actress'
  AND t.production_year BETWEEN 2005 AND 2009
  AND t.id=mi.movie_id AND t.id=mc.movie_id AND t.id=ci.movie_id
  AND cn.id=mc.company_id AND it.id=mi.info_type_id
  AND n.id=ci.person_id AND rt.id=ci.role_id
  AND n.id=an.person_id AND chn.id=ci.person_role_id;
```

## Column Reference

### role_type.role (varlen)
Files: `role_type/role.off` (13), `role_type/role.dat`. Scan 12 rows, resolve `'actress'` → `rt_actress`.

### info_type.info (varlen)
Files: `info_type/info.off`, `info_type/info.dat`. Resolve `'release dates'` → `it_rd`.

### company_name.country_code (int16 dict)
Files: `company_name/country_code.bin` (234997), `.dict.off`, `.dict.dat`. Resolve `'[us]'` → `cc_us` (int16).

### name.gender (int8 dict)
Files: `name/gender.bin`, `.dict.off`, `.dict.dat`. Resolve `'f'` → `g_f`.

### name.name, name.id (varlen / int32)
Files: `name/name.off+.dat`, `name/id.bin` identity. 4167491 rows. LIKE `%Ang%` → `memmem`.

### aka_name.person_id (int32)
File: `aka_name/person_id.bin` (901343). aka_name sorted by person_id; use `aka_name__person_id` to confirm existence per pid.

### char_name (dense-PK)
chn.id dense identity; lookup `chn` rows trivially. ci.person_role_id is the FK; use either dense check or `cast_info__person_role_id` CSR. Here char_name has no filter, so simply check `ci.person_role_id != INT32_MIN` (nullable).

### cast_info.note, person_id, movie_id, role_id, person_role_id (varlen + int32 ×4)
Files: `cast_info/note.off+.dat`, `.../person_id.bin`, `.../movie_id.bin`, `.../role_id.bin`, `.../person_role_id.bin`. 36244344 rows; sorted by movie_id.

### movie_companies.note, movie_id, company_id (varlen / int32 / int32)
Files: `movie_companies/note.off+.dat`, `.../movie_id.bin`, `.../company_id.bin`. 2609129 rows.

### movie_info.info, info_type_id, movie_id (varlen / int32 / int32)
Files: `movie_info/info.off+.dat`, `.../info_type_id.bin`, `.../movie_id.bin`. 14835720 rows.

### title.id, title, production_year (int32/varlen/int32)
Files: `title/id.bin` identity, `title/title.off+.dat`, `title/production_year.bin`. NULL=INT32_MIN.

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| role_type | 12 | id | dense-PK |
| info_type | 113 | id | dense-PK |
| company_name | 234,997 | id | dense-PK; country_code int16 dict |
| name | 4,167,491 | id | dense-PK; gender int8 dict |
| aka_name | 901,343 | person_id | offsets_only person_id |
| char_name | 3,140,339 | id | dense-PK |
| cast_info | 36,244,344 | movie_id | offsets_only movie_id; CSR person_id/role_id/person_role_id |
| movie_companies | 2,609,129 | movie_id | offsets_only movie_id; CSR company_id |
| movie_info | 14,835,720 | movie_id | offsets_only movie_id |
| title | 2,528,312 | id | dense-PK |

## Query Analysis

Join graph:
```
rt --role_id-- ci --movie_id-- t --movie_id-- mc --company_id-- cn
                |              \--movie_id-- mi --info_type_id-- it
                +--person_id-- n --id-- an
                +--person_role_id-- chn
```

Driver: scan `title/production_year.bin` for `2005..2009` → small movie set T (~120K).

Plan:
1. Resolve `rt_actress`, `it_rd`, `cc_us`, `g_f`.
2. Build name candidate set `P`: scan name.name for `%Ang%` AND `n_gender[i]==g_f` → small.
3. Build T = title ids with year in [2005,2009].
4. For each `mv` in T:
   - `mc` range via `movie_companies__movie_id`: keep rows where note non-empty AND (LIKE `%(USA)%` OR `%(worldwide)%`) AND `cn_country_code[company_id-1]==cc_us`. If none, skip.
   - `mi` range via `movie_info__movie_id`: keep rows where `info_type_id==it_rd` AND info non-empty AND (LIKE `Japan:%200%` OR `USA:%200%`). If none, skip.
   - `ci` range via `cast_info__movie_id`: for each ci row require `note IN voice-set`, `role_id==rt_actress`, `person_id ∈ P`, `person_role_id != INT32_MIN`. Verify `an` exists via `aka_name__person_id` (range non-empty).
   - Update MIN(n.name[person_id-1]), MIN(title[mv-1]).

Selectivities:
- year 2005..2009 → ~5% of titles.
- ci.note IN 4 voice strings → tiny.
- n.name `%Ang%` AND gender='f' → small.
- mc note LIKE (USA|worldwide) AND country_code='[us]' → small.
- mi.info LIKE patterns → small.

LIKE notes:
- `LIKE '%(USA)%'`, `LIKE '%(worldwide)%'`: `memmem`.
- `LIKE 'Japan:%200%'`: anchored prefix; check first 6 bytes `== "Japan:"`, then `memmem` "200" in tail.
- Same for `USA:%200%`.

## Indexes

### cast_info__movie_id (offsets_only)
File: `_idx/cast_info__movie_id__offsets.bin` (int32, 2528314).

### movie_companies__movie_id (offsets_only)
File: `_idx/movie_companies__movie_id__offsets.bin` (int32, 2528314).

### movie_info__movie_id (offsets_only)
File: `_idx/movie_info__movie_id__offsets.bin` (int32, 2528314).

### aka_name__person_id (offsets_only)
File: `_idx/aka_name__person_id__offsets.bin` (int32, 4167493). Existence check `off[pid+1]>off[pid]`.

### cast_info__person_id (CSR) — alternative driver
Files: `_idx/cast_info__person_id__offsets.bin`, `_idx/cast_info__person_id__rowids.bin`. Useful if `P` is small enough to drive instead of year set.
