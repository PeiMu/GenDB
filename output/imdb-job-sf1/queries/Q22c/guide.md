# Q22c Guide

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
Differs from Q22a in: expanded `mi.info` IN-list (10 literals), `mi_idx.info < '8.5'`, `production_year > 2005`.

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
- File: `company_type/id.bin` (rows = 4); identity. No `ct.kind` filter — trivial join.

### info_type.id, info_type.info
- Files: `info_type/id.bin`, `info_type/info.off|.dat` (rows = 113)
- Use: TWO linear scans → `it1_id` ('countries'), `it2_id` ('rating').

### keyword.id, keyword.keyword
- Files: `keyword/id.bin`, `keyword/keyword.off|.dat` (rows = 134170)
- Use: scan; collect `kw_ids` (4 entries).

### kind_type.id, kind_type.kind
- Files: `kind_type/id.bin`, `kind_type/kind.off|.dat` (rows = 7)
- Use: scan; collect `kt_ids` ({movie, episode}).

### movie_companies.movie_id, .company_id, .company_type_id, .note
- Files: `movie_companies/{movie_id,company_id,company_type_id}.bin`, `note.off|.dat`
- Use: NOT LIKE `%(USA)%` AND LIKE `%(200%)%` via two `memmem` calls; then probe cn.country_code via mc.company_id.

### movie_info.movie_id, .info_type_id, .info
- Files: `movie_info/{movie_id,info_type_id}.bin`, `info.off|.dat`
- Use: filter `info_type_id == it1_id` then `flat_hash_set<string_view>` of 10 literals over info. Length-prefilter (lengths 3..10 chars) before set probe.

### movie_info_idx.movie_id, .info_type_id, .info
- Files: `movie_info_idx/{movie_id,info_type_id}.bin`, `info.off|.dat`
- Use: filter `info_type_id == it2_id` then lex compare `< "8.5"`. Project via `MIN`.

### movie_keyword.movie_id, .keyword_id
- Files: `movie_keyword/{movie_id,keyword_id}.bin`
- Use: `kw_ids.contains(keyword_id)`.

### title.id, .title, .kind_id, .production_year
- Files: `title/id.bin`, `title/title.off|.dat`, `title/kind_id.bin`, `title/production_year.bin`
- Use: driver; filter `production_year > 2005` (skip INT32_MIN), `kt_ids.contains(kind_id)`. Project `MIN(t.title)`.

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
- Same shape as Q22a/b with looser predicates: `> 2005`, `< '8.5'`, 10-literal IN-set on mi.info. Expect more surviving titles per driver step.
- Resolve at startup: `us_code`, `it1_id`, `it2_id`, `kw_ids` (4), `kt_ids` (2), `mi_info_set` (10 string_views).
- Driver: t row v=1..2528312. Cheap int filters first, then per-fact via offsets_only indexes.
- For each surviving v, the four facts (mk, mi, mi_idx, mc) must each contain ≥1 row that passes its column filters.
- MIN aggregation over (cn.name, mi_idx.info, t.title).
- LIKE: mc.note ⇒ two `memmem` calls per row.
- mi_idx.info compare is lexicographic string compare (`memcmp`).

## Indexes
- `movie_companies__movie_id` (offsets_only): `_idx/movie_companies__movie_id__offsets.bin` (int32, 2528314).
- `movie_info__movie_id` (offsets_only): `_idx/movie_info__movie_id__offsets.bin`.
- `movie_info_idx__movie_id` (offsets_only): `_idx/movie_info_idx__movie_id__offsets.bin`.
- `movie_keyword__movie_id` (offsets_only): `_idx/movie_keyword__movie_id__offsets.bin`.

Snippet:
```cpp
int32_t lo = off[v], hi = off[v+1];
for (int32_t r = lo; r < hi; ++r) { /* mc/mi/mi_idx/mk row r */ }
```
- No invented indexes on text columns; all varlen filters via `.off`+`.dat`.
