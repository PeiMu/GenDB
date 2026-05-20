# Q22d Guide

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
  AND mi.info IN ('Sweden','Norway','Germany','Denmark','Swedish','Denish',
                  'Norwegian','German','USA','American')
  AND mi_idx.info < '8.5'
  AND t.production_year > 2005
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
Same as Q22c but without the `mc.note` LIKE/NOT LIKE predicates.

## Column Reference

### company_name.id (PK, int32_t)
- File: `company_name/id.bin` (rows = 234997); identity.

### company_name.name (varlen)
- Files: `company_name/name.off`, `company_name/name.dat`
- Use: projected via `MIN(cn.name)`.

### company_name.country_code (dict, int16_t)
- Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`
- Use: resolve `us_code`. Filter `code != 0 && code != us_code`.

### company_type.id (PK, int32_t)
- File: `company_type/id.bin` (rows = 4); identity. Trivial join (no kind filter).

### info_type.id, info_type.info
- Files: `info_type/id.bin`, `info_type/info.off|.dat`
- Use: scan TWICE → `it1_id` ('countries'), `it2_id` ('rating').

### keyword.id, keyword.keyword
- Files: `keyword/id.bin`, `keyword/keyword.off|.dat`
- Use: scan; build `kw_ids` (4 entries).

### kind_type.id, kind_type.kind
- Files: `kind_type/id.bin`, `kind_type/kind.off|.dat`
- Use: scan; build `kt_ids` ({movie, episode}).

### movie_companies.movie_id, .company_id, .company_type_id
- Files: `movie_companies/{movie_id,company_id,company_type_id}.bin` (rows = 2609129)
- Use: iterate via offsets_only index; probe cn.country_code via mc.company_id. NOTE: mc.note column is NOT read by Q22d.

### movie_info.movie_id, .info_type_id, .info
- Files: `movie_info/{movie_id,info_type_id}.bin`, `info.off|.dat`
- Use: filter `info_type_id == it1_id` then 10-literal IN-set match over varlen info.

### movie_info_idx.movie_id, .info_type_id, .info
- Files: `movie_info_idx/{movie_id,info_type_id}.bin`, `info.off|.dat`
- Use: filter `info_type_id == it2_id` then lex compare `< "8.5"`. Projected via `MIN`.

### movie_keyword.movie_id, .keyword_id
- Files: `movie_keyword/{movie_id,keyword_id}.bin`
- Use: `kw_ids.contains(keyword_id)`.

### title.id, .title, .kind_id, .production_year
- Files: `title/id.bin`, `title/title.off|.dat`, `title/kind_id.bin`, `title/production_year.bin`
- Use: driver; filter `production_year > 2005`, `kt_ids.contains(kind_id)`. Project `MIN(t.title)`.

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
- Same shape as Q22c, but mc only needs `cn.country_code != '[us]'` — no note text predicates ⇒ much higher mc pass rate (skip `note.off/.dat` entirely).
- Resolve at startup: `us_code`, `it1_id`, `it2_id`, `kw_ids` (4), `kt_ids` (2), `mi_info_set` (10).
- Driver: t row v=1..2528312. Cheap int filters first (year, kind). Then for each fact via offsets_only indexes.
- Per-survivor: probe mk → mi_idx (1.38M, smaller fact) → mi (14.8M, biggest) → mc. Each must yield ≥1 satisfying row.
- MIN aggregation over (cn.name, mi_idx.info, t.title).
- LIKE notes: none in Q22d.
- mi_idx.info `< '8.5'` is lex string compare via `memcmp`.

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
- No index on any text column. country_code resolved via dict scan once.
