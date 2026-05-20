# Q23c Guide

## SQL
```sql
SELECT MIN(kt.kind) AS movie_kind,
       MIN(t.title) AS complete_us_internet_movie
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
  AND kt.kind IN ('movie','tv movie','video movie','direct-to-video')
  AND mi.note LIKE '%internet%'
  AND mi.info IS NOT NULL
  AND (mi.info LIKE 'USA:% 199%' OR mi.info LIKE 'USA:% 200%')
  AND t.production_year > 1990
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
Differs from Q23a: `kt.kind` widened to 4 literals; `production_year > 1990`.

## Column Reference

### complete_cast.movie_id, .status_id
- Files: `complete_cast/movie_id.bin`, `complete_cast/status_id.bin` (rows = 135086); FK-sorted.
- Use: offsets_only by movie_id; filter `status_id == cct1_id`.

### comp_cast_type.id, .kind
- Files: `comp_cast_type/id.bin`, `comp_cast_type/kind.off|.dat` (rows = 4)
- Use: scan kind → `cct1_id` ('complete+verified').

### company_name.id, .country_code
- Files: `company_name/id.bin`, `country_code.bin|.dict.off|.dict.dat` (rows = 234997)
- Use: resolve `us_code`. Filter `code == us_code`.

### company_type.id (PK)
- File: `company_type/id.bin` (rows = 4); identity. Trivial join.

### info_type.id, .info
- Files: `info_type/id.bin`, `info.off|.dat` (rows = 113)
- Use: scan → `it1_id` for `'release dates'`.

### keyword.id, .keyword
- Files: `keyword/id.bin`, `keyword.off|.dat` (rows = 134170)
- Use: trivial join — no `k.keyword` predicate (any mk.keyword_id passes via FK).

### kind_type.id, .kind
- Files: `kind_type/id.bin`, `kind.off|.dat` (rows = 7)
- Use: scan → build `flat_hash_set<int32_t> kt_ids` of size 4. Project `MIN(kt.kind)`.

### movie_companies.movie_id, .company_id, .company_type_id
- Files: `movie_companies/{movie_id,company_id,company_type_id}.bin` (rows = 2609129)
- Use: offsets_only; probe `country_code[company_id-1] == us_code`.

### movie_info.movie_id, .info_type_id, .info, .note
- Files: `movie_info/{movie_id,info_type_id}.bin`, `info.off|.dat`, `note.off|.dat`
- Use: `info_type_id == it1_id`; note LIKE `%internet%` (memmem); info length>0 AND (LIKE `'USA:% 199%'` OR `'USA:% 200%'`) — `USA:` prefix memcmp, then memmem ` 199` or ` 200`.

### movie_keyword.movie_id, .keyword_id
- Files: `movie_keyword/{movie_id,keyword_id}.bin`
- Use: existence join, no keyword filter.

### title.id, .title, .kind_id, .production_year
- Files: `title/id.bin`, `title.off|.dat`, `kind_id.bin`, `production_year.bin`
- Use: driver; filter `kt_ids.contains(kind_id)` AND `production_year > 1990`. Project `MIN(t.title)`.

## Table Stats
| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| comp_cast_type | 4 | dim | id | — |
| company_type | 4 | dim | id | — |
| kind_type | 7 | dim | id | — |
| info_type | 113 | dim | id | — |
| keyword | 134,170 | dim (free) | id | 100000 |
| company_name | 234,997 | dim | id | 100000 |
| complete_cast | 135,086 | fact | movie_id | 100000 |
| title | 2,528,312 | driver | id | 100000 |
| movie_companies | 2,609,129 | fact | movie_id | 100000 |
| movie_keyword | 4,523,930 | fact | movie_id | 100000 |
| movie_info | 14,835,720 | fact | movie_id | 200000 |

## Query Analysis
- Same shape as Q23a but with broader filters: 4 kt ids and `year > 1990` ⇒ wider driver set.
- Resolve at startup: `cct1_id`, `us_code`, `it1_id`, `kt_ids` (4).
- Driver: t row v=1..2528312. `kt_ids.contains(kind_id)` AND `production_year > 1990`.
- Per-survivor: cc (cct1 selective), mk (free / existence), mc (us_code), mi (it1_id + note + info LIKE).
- MIN over (kt.kind, t.title). kt.kind varies across the 4 surviving kinds; running min keeps the lexicographic minimum.
- LIKE: mi.note → memmem; mi.info → memcmp(`USA:`) + memmem(` 199`) OR memmem(` 200`).

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
- No movie_info_idx involvement. No invented text indexes.
