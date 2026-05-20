# Q23a Guide

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
  AND kt.kind IN ('movie')
  AND mi.note LIKE '%internet%'
  AND mi.info IS NOT NULL
  AND (mi.info LIKE 'USA:% 199%' OR mi.info LIKE 'USA:% 200%')
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

## Column Reference

### complete_cast.movie_id (FK to title, int32_t, nullable)
- File: `complete_cast/movie_id.bin` (rows = 135086); FK-sorted.
- Use: offsets_only `complete_cast__movie_id`.

### complete_cast.status_id (FK to comp_cast_type, int32_t)
- File: `complete_cast/status_id.bin`
- Use: filter `status_id == cct1_id`.

### comp_cast_type.id, .kind
- Files: `comp_cast_type/id.bin`, `comp_cast_type/kind.off|.dat` (rows = 4); identity id.
- Use: scan kind to resolve `cct1_id` for literal `'complete+verified'`.

### company_name.id (PK, int32_t)
- File: `company_name/id.bin` (rows = 234997); identity.

### company_name.country_code (dict, int16_t)
- Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`
- Use: resolve `us_code` for `'[us]'`. Filter `code == us_code` (drops NULL/code 0 implicitly).

### company_type.id (PK, int32_t)
- File: `company_type/id.bin` (rows = 4); identity. Trivial join — no `ct.kind` predicate.

### info_type.id, .info
- Files: `info_type/id.bin`, `info_type/info.off|.dat` (rows = 113)
- Use: scan info to resolve `it1_id` for `'release dates'`. (Only one info_type used in Q23a.)

### keyword.id, .keyword
- Files: `keyword/id.bin`, `keyword/keyword.off|.dat` (rows = 134170)
- Use: trivial join — no `k.keyword` predicate; any mk.keyword_id matching a keyword.id (always true since FK).

### kind_type.id, .kind
- Files: `kind_type/id.bin`, `kind_type/kind.off|.dat` (rows = 7)
- Use: scan to resolve target id for `'movie'` → `kt_id`. Project `MIN(kt.kind)`.

### movie_companies.movie_id, .company_id, .company_type_id
- Files: `movie_companies/{movie_id,company_id,company_type_id}.bin` (rows = 2609129)
- Use: iterate via offsets_only; check `country_code_bin[company_id-1] == us_code`.

### movie_info.movie_id, .info_type_id, .info, .note
- Files: `movie_info/{movie_id,info_type_id}.bin`, `info.off|.dat`, `note.off|.dat` (rows = 14835720)
- Use: filter `info_type_id == it1_id`. Then `note` LIKE `%internet%` (one `memmem`); empty note (NULL) fails. Then `info` IS NOT NULL (length > 0) AND (LIKE `'USA:% 199%'` OR LIKE `'USA:% 200%'`). The `'USA:'` prefix is a fast 4-byte memcmp check; remaining `% 199%` / `% 200%` need one `memmem` each.

### movie_keyword.movie_id, .keyword_id
- Files: `movie_keyword/{movie_id,keyword_id}.bin` (rows = 4523930)
- Use: existence join only — any mk row for the movie satisfies (no k.keyword filter).

### title.id, .title, .kind_id, .production_year
- Files: `title/id.bin`, `title/title.off|.dat`, `title/kind_id.bin`, `title/production_year.bin` (rows = 2528312)
- Use: driver; filter `kind_id == kt_id` AND `production_year > 2000`. Project `MIN(t.title)`.

## Table Stats
| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| comp_cast_type | 4 | dim | id | — |
| company_type | 4 | dim | id | — |
| kind_type | 7 | dim | id | — |
| info_type | 113 | dim | id | — |
| keyword | 134,170 | dim (free join) | id | 100000 |
| company_name | 234,997 | dim | id | 100000 |
| complete_cast | 135,086 | fact | movie_id | 100000 |
| title | 2,528,312 | driver | id | 100000 |
| movie_companies | 2,609,129 | fact | movie_id | 100000 |
| movie_keyword | 4,523,930 | fact | movie_id | 100000 |
| movie_info | 14,835,720 | fact | movie_id | 200000 |

## Query Analysis
- Join graph: title hub; mc, mi, mk, cc all on movie_id. mc→cn (us), cc→cct1 (complete+verified), mi→it1 (release dates).
- Resolve before driver: `cct1_id`, `us_code`, `it1_id`, `kt_id` (single 'movie'). No `k.keyword` literal, so any mk row passes.
- Driver: title v=1..2528312. Filter `kind_id == kt_id` (single match — 'movie') AND `production_year > 2000` (skip INT32_MIN).
- Per survivor: lookup cc range → filter status_id == cct1_id (very selective: 'complete+verified' is rare); mk range non-empty; mc range → filter company_id's country_code == us_code; mi range → it1_id then note LIKE `%internet%` and info LIKE prefix `'USA:'` + remainder.
- Order facts cheap-first: cc (smallest, ~135K total; status filter very selective), then mk, then mc (note column unused), then mi (largest; note + info LIKE most expensive).
- MIN aggregation over (kt.kind, t.title). Since exactly one kt id passes ('movie'), `MIN(kt.kind)` is a constant; still emit only if at least one row produced.
- LIKE: mi.note (one `memmem`), mi.info (4-byte prefix memcmp + memmem). Empty slice = NULL = fail.

## Indexes
- `movie_companies__movie_id` (offsets_only): `_idx/movie_companies__movie_id__offsets.bin` (int32, 2528314).
- `movie_info__movie_id` (offsets_only): `_idx/movie_info__movie_id__offsets.bin`.
- `movie_keyword__movie_id` (offsets_only): `_idx/movie_keyword__movie_id__offsets.bin`.
- `complete_cast__movie_id` (offsets_only): `_idx/complete_cast__movie_id__offsets.bin` (int32, 2528314). Slot 0 = count of cc rows with movie_id < 1 (incl. NULLs).

Snippet:
```cpp
int32_t lo = off[v], hi = off[v+1];
for (int32_t r = lo; r < hi; ++r) { /* fact row r */ }
```
- No movie_info_idx involvement.
- No invented indexes on note/info text.
