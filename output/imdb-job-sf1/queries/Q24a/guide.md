# Q24a Guide

## SQL
```sql
SELECT MIN(chn.name) AS voiced_char_name,
       MIN(n.name) AS voicing_actress_name,
       MIN(t.title) AS voiced_action_movie_jap_eng
FROM aka_name AS an,
     char_name AS chn,
     cast_info AS ci,
     company_name AS cn,
     info_type AS it,
     keyword AS k,
     movie_companies AS mc,
     movie_info AS mi,
     movie_keyword AS mk,
     name AS n,
     role_type AS rt,
     title AS t
WHERE ci.note IN ('(voice)', '(voice: Japanese version)',
                  '(voice) (uncredited)', '(voice: English version)')
  AND cn.country_code = '[us]'
  AND it.info = 'release dates'
  AND k.keyword IN ('hero','martial-arts','hand-to-hand-combat')
  AND mi.info IS NOT NULL
  AND (mi.info LIKE 'Japan:%200%' OR mi.info LIKE 'USA:%200%')
  AND n.gender = 'f'
  AND n.name LIKE '%An%'
  AND rt.role = 'actress'
  AND t.production_year > 2000
  AND t.id = mi.movie_id
  AND t.id = mc.movie_id
  AND t.id = ci.movie_id
  AND t.id = mk.movie_id
  AND mc.movie_id = ci.movie_id
  AND mc.movie_id = mi.movie_id
  AND mc.movie_id = mk.movie_id
  AND mi.movie_id = ci.movie_id
  AND mi.movie_id = mk.movie_id
  AND ci.movie_id = mk.movie_id
  AND cn.id = mc.company_id
  AND it.id = mi.info_type_id
  AND n.id = ci.person_id
  AND rt.id = ci.role_id
  AND n.id = an.person_id
  AND ci.person_id = an.person_id
  AND chn.id = ci.person_role_id
  AND k.id = mk.keyword_id;
```

## Column Reference

### aka_name.person_id (FK to name, int32_t)
- File: `aka_name/person_id.bin` (rows = 901343); FK-sorted by person_id.
- Use: offsets_only `aka_name__person_id`. Existence check that `an.person_id == n.id`.

### char_name.id (PK, int32_t)
- File: `char_name/id.bin` (rows = 3140339); dense identity.

### char_name.name (varlen)
- Files: `char_name/name.off`, `char_name/name.dat`
- Use: projected via `MIN(chn.name)`.

### cast_info.movie_id, .person_id, .person_role_id, .role_id, .note
- Files: `cast_info/{movie_id,person_id,person_role_id,role_id}.bin`, `note.off|.dat` (rows = 36244344); FK-sorted by movie_id.
- Use: offsets_only `cast_info__movie_id`; filter `role_id == rt_id` (actress), `ci.note` ∈ 4-set, `person_id` survives name filters, `person_role_id` non-NULL → chn lookup.

### company_name.id, .country_code
- Files: `company_name/id.bin`, `country_code.bin|.dict.off|.dict.dat` (rows = 234997)
- Use: resolve `us_code`. Filter `country_code[id-1] == us_code`.

### info_type.id, .info
- Files: `info_type/id.bin`, `info.off|.dat` (rows = 113)
- Use: scan → `it_id` for `'release dates'`.

### keyword.id, .keyword
- Files: `keyword/id.bin`, `keyword.off|.dat` (rows = 134170)
- Use: scan for each of `{hero, martial-arts, hand-to-hand-combat}`; build `kw_ids` (3 ids).

### movie_companies.movie_id, .company_id
- Files: `movie_companies/{movie_id,company_id}.bin` (rows = 2609129)
- Use: offsets_only by movie_id; probe `country_code[company_id-1] == us_code`.

### movie_info.movie_id, .info_type_id, .info
- Files: `movie_info/{movie_id,info_type_id}.bin`, `info.off|.dat` (rows = 14835720)
- Use: filter `info_type_id == it_id`. Then info non-empty AND (LIKE `'Japan:%200%'` OR LIKE `'USA:%200%'`): match prefix `'Japan:'` (6 bytes) or `'USA:'` (4 bytes) via memcmp, then `memmem("200")` in remainder.

### movie_keyword.movie_id, .keyword_id
- Files: `movie_keyword/{movie_id,keyword_id}.bin`
- Use: offsets_only by movie_id; filter `kw_ids.contains(keyword_id)`.

### name.id, .name, .gender
- Files: `name/id.bin` (identity, rows = 4167491), `name.off|.dat`, `gender.bin|.dict.off|.dict.dat` (int8 dict)
- Use: resolve `f_code` for `'f'`. Pre-build a `valid_persons` bitset by scanning name.gender + name.name: `gender_code == f_code` AND `memmem(name, "An")`. Project `MIN(n.name)`.

### role_type.id, .role
- Files: `role_type/id.bin`, `role.off|.dat` (rows = 12)
- Use: scan to resolve `rt_id` for `'actress'`.

### title.id, .title, .production_year
- Files: `title/id.bin`, `title.off|.dat`, `production_year.bin` (rows = 2528312)
- Use: driver; filter `production_year > 2000` (skip INT32_MIN). Project `MIN(t.title)`.

## Table Stats
| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| role_type | 12 | dim | id | — |
| info_type | 113 | dim | id | — |
| keyword | 134,170 | dim | id | 100000 |
| company_name | 234,997 | dim | id | 100000 |
| title | 2,528,312 | driver | id | 100000 |
| movie_companies | 2,609,129 | fact | movie_id | 100000 |
| movie_keyword | 4,523,930 | fact | movie_id | 100000 |
| movie_info | 14,835,720 | fact | movie_id | 200000 |
| aka_name | 901,343 | fact | person_id | 100000 |
| name | 4,167,491 | dim/fact | id | 100000 |
| char_name | 3,140,339 | dim | id | 100000 |
| cast_info | 36,244,344 | fact | movie_id | 200000 |

## Query Analysis
- Join graph: title hub on movie_id (mc, mi, mk, ci); ci hub on person_id (n) and aka_name; ci→chn via person_role_id; ci→rt via role_id.
- Resolve before driver: `us_code`, `it_id`, `kw_ids` (3), `rt_id` ('actress'), `f_code` (gender dict).
- Pre-pass on name: build a bitset `valid_persons[p-1]` where `gender == f_code` AND `name LIKE '%An%'`. This avoids re-checking per ci row.
- Driver: title v=1..2528312. Filter `production_year > 2000`.
- Per-survivor v: visit mk range (cheap; filter kw_ids), then mc range (us_code), then mi range (it_id + info LIKE prefix), then ci range. Within ci: filter `role_id == rt_id`, `valid_persons[person_id-1]`, `ci.note` IN-set (4 literals; build `flat_hash_set<string_view>`; length prefilter), and `person_role_id != INT32_MIN`. For aka_name existence: `aka_name__person_id` offsets_only gives `lo<hi` check.
- Selectivities: `role_id == rt_id` (actress) ~moderate; `valid_persons` very selective (gender='f' AND name LIKE '%An%'); ci.note IN-set is selective; kw_ids 3/134170; us_code majority of mc; mi info LIKE is selective on year suffix.
- MIN aggregation: running min for chn.name, n.name, t.title.
- LIKE: `n.name` LIKE `'%An%'` → memmem in pre-pass; `mi.info` → prefix memcmp + memmem ` 200`. Empty slices fail.

## Indexes
- `movie_companies__movie_id` (offsets_only): `_idx/movie_companies__movie_id__offsets.bin` (int32, 2528314).
- `movie_info__movie_id` (offsets_only): `_idx/movie_info__movie_id__offsets.bin`.
- `movie_keyword__movie_id` (offsets_only): `_idx/movie_keyword__movie_id__offsets.bin`.
- `cast_info__movie_id` (offsets_only): `_idx/cast_info__movie_id__offsets.bin`.
- `aka_name__person_id` (offsets_only): `_idx/aka_name__person_id__offsets.bin` (int32, 4167493).

Snippet:
```cpp
int32_t lo = off[v], hi = off[v+1];
for (int32_t r = lo; r < hi; ++r) { /* fact row r */ }
```
- char_name fetched by direct index `name.off[chn_id-1] .. name.off[chn_id]` after reading `ci.person_role_id`.
- No invented indexes on note/info/keyword text.
