## SQL

```sql
SELECT MIN(n.name) AS voicing_actress,
       MIN(t.title) AS kung_fu_panda
FROM aka_name AS an, char_name AS chn, cast_info AS ci,
     company_name AS cn, info_type AS it, movie_companies AS mc,
     movie_info AS mi, name AS n, role_type AS rt, title AS t
WHERE ci.note = '(voice)'
  AND cn.country_code='[us]'
  AND it.info='release dates'
  AND mc.note LIKE '%(200%)%'
  AND (mc.note LIKE '%(USA)%' OR mc.note LIKE '%(worldwide)%')
  AND mi.info IS NOT NULL
  AND (mi.info LIKE 'Japan:%2007%' OR mi.info LIKE 'USA:%2008%')
  AND n.gender='f' AND n.name LIKE '%Angel%'
  AND rt.role='actress'
  AND t.production_year BETWEEN 2007 AND 2008
  AND t.title LIKE '%Kung%Fu%Panda%'
  AND t.id=mi.movie_id AND t.id=mc.movie_id AND t.id=ci.movie_id
  AND cn.id=mc.company_id AND it.id=mi.info_type_id
  AND n.id=ci.person_id AND rt.id=ci.role_id
  AND n.id=an.person_id AND chn.id=ci.person_role_id;
```

## Column Reference

### role_type.role / info_type.info / company_name.country_code / name.gender
Same as Q19a — resolve `rt_actress`, `it_rd`, `cc_us`, `g_f` once.

### name.name (varlen)
Files: `name/name.off`, `name/name.dat`. LIKE `%Angel%`.

### cast_info.note, person_id, movie_id, role_id, person_role_id
Files under `cast_info/`. Single literal `'(voice)'` → exact length+memcmp.

### movie_companies.note, movie_id, company_id
Files under `movie_companies/`. Multiple LIKEs.

### movie_info.info, info_type_id, movie_id
Files under `movie_info/`.

### title.id, title, production_year
Files under `title/`. `title.title` LIKE `%Kung%Fu%Panda%` → two-pointer scan: find "Kung", then "Fu", then "Panda" in order via successive `memmem`.

### aka_name.person_id (existence)
File: `aka_name/person_id.bin`. Confirm via `aka_name__person_id` index range.

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| role_type | 12 | id | dense-PK |
| info_type | 113 | id | dense-PK |
| company_name | 234,997 | id | dense-PK |
| name | 4,167,491 | id | dense-PK |
| aka_name | 901,343 | person_id | offsets_only |
| char_name | 3,140,339 | id | dense-PK |
| cast_info | 36,244,344 | movie_id | offsets_only movie_id; CSR person_id/role_id/person_role_id |
| movie_companies | 2,609,129 | movie_id | offsets_only movie_id; CSR company_id |
| movie_info | 14,835,720 | movie_id | offsets_only movie_id |
| title | 2,528,312 | id | dense-PK |

## Query Analysis

Join graph identical to Q19a.

Strongest filter: `t.title LIKE '%Kung%Fu%Panda%'` AND `production_year BETWEEN 2007 AND 2008`. This yields a handful of movies. Use that as the driver.

Plan:
1. Resolve `rt_actress`, `it_rd`, `cc_us`, `g_f`.
2. Scan `title` once: collect `mv` where `production_year ∈ [2007,2008]` and title LIKE `%Kung%Fu%Panda%`. Likely <10 rows.
3. For each `mv` in T:
   - mc range: keep rows whose note matches all of `%(200%)%`, (`%(USA)%` OR `%(worldwide)%`), AND `cn.country_code==cc_us`.
   - mi range: keep rows where info_type_id==it_rd, info LIKE `Japan:%2007%` OR `USA:%2008%`.
   - ci range: for each ci row require `note=='(voice)'` (exact), `role_id==rt_actress`, person_role_id non-NULL. Read `pid=person_id[r]`; check `n_gender[pid-1]==g_f` AND `n.name[pid-1]` LIKE `%Angel%`. Check aka_name exists for pid.
   - Update MIN(n.name), MIN(t.title).

Selectivities:
- title LIKE `%Kung%Fu%Panda%` + year 2007..2008 → handful.
- Combined → almost certainly 1-2 movies.

LIKE notes:
- `%Kung%Fu%Panda%`: three substrings in order; advance pointer after each match.
- `mc.note LIKE '%(200%)%'`: `memmem` "(20" then either '0'..'9' digit then ')'; simplest: `memmem` "(200", then scan to next ')'. (Approximate with `strstr` for "(20" followed by digit then ')'.)
- `Japan:%2007%`: prefix check "Japan:" then `memmem` "2007".
- `USA:%2008%`: prefix "USA:" then `memmem` "2008".
- `%Angel%`: `memmem`.

## Indexes

### cast_info__movie_id (offsets_only)
File: `_idx/cast_info__movie_id__offsets.bin`.

### movie_companies__movie_id (offsets_only)
File: `_idx/movie_companies__movie_id__offsets.bin`.

### movie_info__movie_id (offsets_only)
File: `_idx/movie_info__movie_id__offsets.bin`.

### aka_name__person_id (offsets_only)
File: `_idx/aka_name__person_id__offsets.bin`. `aka_name` existence per person.

Note: with the title-LIKE filter so tight, no CSR drive is needed.
