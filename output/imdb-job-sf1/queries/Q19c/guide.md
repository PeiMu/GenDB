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
  AND mi.info IS NOT NULL
  AND (mi.info LIKE 'Japan:%200%' OR mi.info LIKE 'USA:%200%')
  AND n.gender='f' AND n.name LIKE '%An%'
  AND rt.role='actress'
  AND t.production_year > 2000
  AND t.id=mi.movie_id AND t.id=mc.movie_id AND t.id=ci.movie_id
  AND cn.id=mc.company_id AND it.id=mi.info_type_id
  AND n.id=ci.person_id AND rt.id=ci.role_id
  AND n.id=an.person_id AND chn.id=ci.person_role_id;
```

## Column Reference

Identical to Q19a. Key differences:
- No `mc.note` filter (mc only constrained by country_code via cn).
- `n.name LIKE '%An%'` is weaker (covers many names).
- `t.production_year > 2000` is broad.
- mi.info LIKE patterns still apply.

### role_type.role / info_type.info / company_name.country_code / name.gender
Standard dim resolution as in Q19a (`rt_actress`, `it_rd`, `cc_us`, `g_f`).

### name.name (varlen)
LIKE `%An%` — short pattern; many hits. Combine with `n_gender==g_f` to prune.

### cast_info.note (varlen)
IN-list 4 voice strings — `flat_hash_set<string_view>`.

### title.id, title, production_year
`production_year > 2000` AND `!= INT32_MIN`.

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

Join graph (same star as Q19a/b; no mc.note filter):
```
rt --role_id-- ci --movie_id-- t --movie_id-- mc --company_id-- cn
                |              \--movie_id-- mi --info_type_id-- it
                +--person_id-- n --id-- an
                +--person_role_id-- chn
```

Filters are weaker than Q19a/b, so the join is heavier. Best plan: drive from `mi.info LIKE` because mi.info has just two anchored patterns (`Japan:%200%`, `USA:%200%`) and `info_type_id==it_rd` is needed simultaneously.

Plan:
1. Resolve `rt_actress`, `it_rd`, `cc_us`, `g_f`.
2. Walk `movie_info__info_type_id` CSR for `it_rd` (release dates) — iterate only that subset of mi rows.
3. For each mi row r with `info_type_id==it_rd`: check info length >= 5 and either starts with "Japan:" + contains "200" later, or starts with "USA:" + contains "200" later. Collect `mv=movie_id[r]` into T.
4. Filter T by `production_year[mv-1] > 2000` (and != INT32_MIN).
5. For each `mv` in T:
   - mc range: keep mc rows where `cn_country_code[company_id-1]==cc_us`. (no note filter)
   - ci range: for each ci row require `note IN voice-set`, `role_id==rt_actress`, `person_role_id != INT32_MIN`; `pid=person_id[r]`; check `n_gender[pid-1]==g_f` and `n.name[pid-1]` LIKE `%An%`; require aka_name range non-empty for pid.
   - Update MIN(n.name), MIN(t.title).

Selectivities:
- year > 2000 → ~half of titles.
- mi info LIKE → small (release-date subset further filtered).
- ci.note voice IN-list → small.
- n.gender='f' + `%An%` → still large; combined with ci filter shrinks fast.

LIKE notes:
- `%An%`: `memmem` length-pruned.
- `Japan:%200%`: prefix check + later `memmem` "200".
- `USA:%200%`: prefix + `memmem` "200".

## Indexes

### movie_info__info_type_id (CSR)
Files: `_idx/movie_info__info_type_id__offsets.bin` (int32, 115), `_idx/movie_info__info_type_id__rowids.bin` (int32, 14835720).
```cpp
auto mit_off = read_vec<int32_t>(store + "/_idx/movie_info__info_type_id__offsets.bin");
auto mit_row = read_vec<int32_t>(store + "/_idx/movie_info__info_type_id__rowids.bin");
int32_t lo=mit_off[it_rd], hi=mit_off[it_rd+1];
for (int32_t k=lo; k<hi; ++k) { int32_t r=mit_row[k]; /* mi row r */ }
```

### cast_info__movie_id (offsets_only)
File: `_idx/cast_info__movie_id__offsets.bin`.

### movie_companies__movie_id (offsets_only)
File: `_idx/movie_companies__movie_id__offsets.bin`.

### aka_name__person_id (offsets_only)
File: `_idx/aka_name__person_id__offsets.bin`.
