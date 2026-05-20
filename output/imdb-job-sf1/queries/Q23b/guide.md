# Q23b Guide

## SQL
```sql
SELECT MIN(kt.kind) AS movie_kind,
       MIN(t.title) AS complete_nerdy_internet_movie
FROM complete_cast AS cc,
     comp_cast_type AS cct1,
     company_name AS cn,
     company_type AS ct,
     info_type AS it1,
     keyword AS k,
     kind_type AS kt,
     movie_companies AS mc,
     movie_info AS mi,
     movie_keyword AS mk,
     title AS t
WHERE cct1.kind = 'complete+verified'
  AND cn.country_code = '[us]'
  AND it1.info = 'release dates'
  AND k.keyword IN ('nerd','loner','alienation','dignity')
  AND kt.kind IN ('movie')
  AND mi.note LIKE '%internet%'
  AND mi.info LIKE 'USA:% 200%'
  AND t.production_year > 2000
  AND kt.id = t.kind_id
  AND t.id = mi.movie_id
  AND t.id = mk.movie_id
  AND t.id = mc.movie_id
  AND t.id = cc.movie_id
  AND mk.movie_id = mi.movie_id
  AND mk.movie_id = mc.movie_id
  AND mk.movie_id = cc.movie_id
  AND mi.movie_id = mc.movie_id
  AND mi.movie_id = cc.movie_id
  AND mc.movie_id = cc.movie_id
  AND k.id = mk.keyword_id
  AND it1.id = mi.info_type_id
  AND cn.id = mc.company_id
  AND ct.id = mc.company_type_id
  AND cct1.id = cc.status_id;
```
Differs from Q23a: adds `k.keyword IN (nerd, loner, alienation, dignity)` and tightens `mi.info` to LIKE `'USA:% 200%'` only.

## Column Reference

### complete_cast.movie_id, .status_id
- Files: `complete_cast/movie_id.bin`, `complete_cast/status_id.bin` (rows = 135086); FK-sorted by movie_id.
- Use: offsets_only index; filter `status_id == cct1_id`.

### comp_cast_type.id, .kind
- Files: `comp_cast_type/id.bin`, `comp_cast_type/kind.off|.dat` (rows = 4)
- Use: scan kind to resolve `cct1_id` for `'complete+verified'`.

### company_name.id, .country_code
- Files: `company_name/id.bin`, `country_code.bin|.dict.off|.dict.dat` (rows = 234997)
- Use: resolve `us_code`. Filter `code == us_code`.

### company_type.id (PK, int32_t)
- File: `company_type/id.bin` (rows = 4); identity. Trivial join.

### info_type.id, .info
- Files: `info_type/id.bin`, `info.off|.dat` (rows = 113)
- Use: scan to resolve `it1_id` ('release dates'). Only one info_type used.

### keyword.id, .keyword
- Files: `keyword/id.bin`, `keyword.off|.dat` (rows = 134170)
- Use: scan for each of `{nerd, loner, alienation, dignity}`; build `flat_hash_set<int32_t> kw_ids`.

### kind_type.id, .kind
- Files: `kind_type/id.bin`, `kind.off|.dat` (rows = 7)
- Use: scan to resolve `kt_id` for `'movie'`. Project `MIN(kt.kind)`.

### movie_companies.movie_id, .company_id, .company_type_id
- Files: `movie_companies/{movie_id,company_id,company_type_id}.bin` (rows = 2609129)
- Use: offsets_only by movie_id; check `country_code[company_id-1] == us_code`.

### movie_info.movie_id, .info_type_id, .info, .note
- Files: `movie_info/{movie_id,info_type_id}.bin`, `info.off|.dat`, `note.off|.dat` (rows = 14835720)
- Use: filter `info_type_id == it1_id`. Then `note` LIKE `%internet%` (one `memmem`). Then `info` LIKE `'USA:% 200%'`: 4-byte prefix `memcmp("USA:",...)` plus `memmem` for `" 200"`.

### movie_keyword.movie_id, .keyword_id
- Files: `movie_keyword/{movie_id,keyword_id}.bin` (rows = 4523930)
- Use: filter `kw_ids.contains(keyword_id)`.

### title.id, .title, .kind_id, .production_year
- Files: `title/id.bin`, `title.off|.dat`, `kind_id.bin`, `production_year.bin` (rows = 2528312)
- Use: driver. Filter `kind_id == kt_id` AND `production_year > 2000`. Project `MIN(t.title)`.

## Table Stats
| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| comp_cast_type | 4 | dim | id | — |
| company_type | 4 | dim | id | — |
| kind_type | 7 | dim | id | — |
| info_type | 113 | dim | id | — |
| keyword | 134,170 | dim | id | 100000 |
| company_name | 234,997 | dim | id | 100000 |
| complete_cast | 135,086 | fact | movie_id | 100000 |
| title | 2,528,312 | driver | id | 100000 |
| movie_companies | 2,609,129 | fact | movie_id | 100000 |
| movie_keyword | 4,523,930 | fact | movie_id | 100000 |
| movie_info | 14,835,720 | fact | movie_id | 200000 |

## Query Analysis
- Same shape as Q23a with two added selectivity boosts: keyword IN-set (4 ids) and info LIKE limited to `'USA:% 200%'` (so 2000s only).
- Resolve at startup: `cct1_id`, `us_code`, `it1_id`, `kt_id`, `kw_ids` (4).
- Driver: title v with `production_year > 2000` AND `kind_id == kt_id`.
- Per-survivor fact order: cc (very selective via cct1_id), mk (selective via kw_ids), mc (us_code check), mi (most expensive — it1_id filter then note+info LIKE).
- MIN over (kt.kind, t.title); kt.kind is constant string for 'movie'.
- LIKE notes: mi.note → one memmem; mi.info → prefix memcmp(`USA:`) + memmem(` 200`).

## Indexes
- `complete_cast__movie_id` (offsets_only): `_idx/complete_cast__movie_id__offsets.bin` (int32, 2528314).
- `movie_companies__movie_id` (offsets_only): `_idx/movie_companies__movie_id__offsets.bin`.
- `movie_info__movie_id` (offsets_only): `_idx/movie_info__movie_id__offsets.bin`.
- `movie_keyword__movie_id` (offsets_only): `_idx/movie_keyword__movie_id__offsets.bin`.

Snippet:
```cpp
int32_t lo = off[v], hi = off[v+1];
for (int32_t r = lo; r < hi; ++r) { /* fact row r */ }
```
- No movie_info_idx involvement. No invented indexes on text predicates.
