# Q22b Guide

## SQL
```sql
SELECT MIN(cn.name) AS movie_company,
       MIN(mi_idx.info) AS rating,
       MIN(t.title) AS western_violent_movie
FROM company_name AS cn,
     company_type AS ct,
     info_type AS it1,
     info_type AS it2,
     keyword AS k,
     kind_type AS kt,
     movie_companies AS mc,
     movie_info AS mi,
     movie_info_idx AS mi_idx,
     movie_keyword AS mk,
     title AS t
WHERE cn.country_code != '[us]'
  AND it1.info = 'countries'
  AND it2.info = 'rating'
  AND k.keyword IN ('murder','murder-in-title','blood','violence')
  AND kt.kind IN ('movie','episode')
  AND mc.note NOT LIKE '%(USA)%'
  AND mc.note LIKE '%(200%)%'
  AND mi.info IN ('Germany','German','USA','American')
  AND mi_idx.info < '7.0'
  AND t.production_year > 2009
  AND kt.id = t.kind_id
  AND t.id = mi.movie_id
  AND t.id = mk.movie_id
  AND t.id = mi_idx.movie_id
  AND t.id = mc.movie_id
  AND mk.movie_id = mi.movie_id
  AND mk.movie_id = mi_idx.movie_id
  AND mk.movie_id = mc.movie_id
  AND mi.movie_id = mi_idx.movie_id
  AND mi.movie_id = mc.movie_id
  AND mc.movie_id = mi_idx.movie_id
  AND k.id = mk.keyword_id
  AND it1.id = mi.info_type_id
  AND it2.id = mi_idx.info_type_id
  AND ct.id = mc.company_type_id
  AND cn.id = mc.company_id;
```
Identical to Q22a except `production_year > 2009`.

## Column Reference

### company_name.id (PK, int32_t)
- File: `company_name/id.bin` (rows = 234997); dense identity.

### company_name.name (varlen)
- Files: `company_name/name.off`, `company_name/name.dat`
- Use: projected via `MIN(cn.name)`.

### company_name.country_code (dict, int16_t)
- Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`
- Use: resolve `us_code` for `'[us]'`. Predicate `!= '[us]'` skips code 0 (NULL) AND `== us_code`.

### company_type.id (PK, int32_t)
- File: `company_type/id.bin` (rows = 4); identity. No `ct.kind` filter ⇒ trivial join (any mc.company_type_id in [1..4]).

### info_type.id (PK, int32_t)
- File: `info_type/id.bin` (rows = 113); identity.

### info_type.info (varlen)
- Files: `info_type/info.off`, `info_type/info.dat`
- Use: scan TWICE: `it1_id` for `'countries'`, `it2_id` for `'rating'`.

### keyword.id, keyword.keyword
- Files: `keyword/id.bin`, `keyword/keyword.off|.dat` (rows = 134170)
- Use: scan for each of the 4 literals, build `flat_hash_set<int32_t> kw_ids`.

### kind_type.id, kind_type.kind
- Files: `kind_type/id.bin`, `kind_type/kind.off|.dat` (rows = 7)
- Use: scan; build `flat_hash_set<int32_t> kt_ids` ({movie, episode}).

### movie_companies.movie_id, .company_id, .company_type_id, .note
- Files: `movie_companies/{movie_id,company_id,company_type_id}.bin`, `note.off|.dat` (rows = 2609129)
- Use: iterate via offsets_only index; check note (NOT LIKE + LIKE: two `memmem`), then cn.country_code probe.

### movie_info.movie_id, .info_type_id, .info
- Files: `movie_info/{movie_id,info_type_id}.bin`, `info.off|.dat` (rows = 14835720)
- Use: filter `info_type_id == it1_id` then `flat_hash_set<string_view>{Germany,German,USA,American}.contains(info)`.

### movie_info_idx.movie_id, .info_type_id, .info
- Files: `movie_info_idx/{movie_id,info_type_id}.bin`, `info.off|.dat` (rows = 1380035)
- Use: filter `info_type_id == it2_id` then string lex compare `< "7.0"`. Also projected via `MIN`.

### movie_keyword.movie_id, .keyword_id
- Files: `movie_keyword/{movie_id,keyword_id}.bin` (rows = 4523930)
- Use: filter `kw_ids.contains(keyword_id)`.

### title.id, .title, .kind_id, .production_year
- Files: `title/id.bin`, `title/title.off|.dat`, `title/kind_id.bin`, `title/production_year.bin`
- Use: driver; filter `production_year > 2009` (skip INT32_MIN) and `kt_ids.contains(kind_id)`. Project `MIN(t.title)`.

## Table Stats
| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| company_type | 4 | dim | id | — |
| kind_type | 7 | dim | id | — |
| info_type | 113 | dim (used twice) | id | — |
| keyword | 134,170 | dim | id | 100000 |
| company_name | 234,997 | dim | id | 100000 |
| title | 2,528,312 | driver | id | 100000 |
| movie_companies | 2,609,129 | fact | movie_id | 100000 |
| movie_info_idx | 1,380,035 | fact | movie_id | 100000 |
| movie_keyword | 4,523,930 | fact | movie_id | 100000 |
| movie_info | 14,835,720 | fact | movie_id | 200000 |

## Query Analysis
- Same shape as Q22a; only difference is tighter year filter (`> 2009` vs `> 2008`).
- Resolve before driver: `us_code`, `it1_id`, `it2_id`, `kw_ids` (size 4), `kt_ids` (size 2).
- Driver: title row v=1..2528312, apply cheap int filters first (production_year, kind_id).
- For survivors: fetch mk range (cheapest), mi_idx range (small table, ~1.38M rows), mi range, mc range. Each must yield ≥1 satisfying row.
- MIN aggregation over (cn.name, mi_idx.info, t.title) — running mins.
- LIKE: mc.note has both NOT LIKE and LIKE ⇒ two `memmem` calls. Empty slice → drop.
- mi_idx.info `< '7.0'` is string compare, not numeric — `memcmp` on bytes.

## Indexes
- `movie_companies__movie_id` (offsets_only): `_idx/movie_companies__movie_id__offsets.bin` (int32, 2528314).
- `movie_info__movie_id` (offsets_only): `_idx/movie_info__movie_id__offsets.bin`.
- `movie_info_idx__movie_id` (offsets_only): `_idx/movie_info_idx__movie_id__offsets.bin`.
- `movie_keyword__movie_id` (offsets_only): `_idx/movie_keyword__movie_id__offsets.bin`.

Snippet:
```cpp
int32_t lo = off[v], hi = off[v+1];
for (int32_t r = lo; r < hi; ++r) { /* fact row r */ }
```
- No invented indexes; cn.country_code resolved via dict scan, all varlen filters by direct `.off`+`.dat` access.
