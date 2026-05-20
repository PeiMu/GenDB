# Q22a Guide

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
  AND t.production_year > 2008
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

## Column Reference

### company_name.id (PK, int32_t)
- File: `company_name/id.bin` (rows = 234997); dense identity row i ↔ id (i+1).
- Use: target of `cn.id = mc.company_id`.

### company_name.name (varlen)
- Files: `company_name/name.off`, `company_name/name.dat` (rows = 234997)
- Use: projected via `MIN(cn.name)` for surviving cn ids.

### company_name.country_code (dict, int16_t)
- Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`
- Use: resolve dict code for `'[us]'` → `us_code` (see shared context). Predicate `!= '[us]'` ⇒ skip rows where `code == 0` (NULL) OR `code == us_code`.

### company_type.id (PK, int32_t)
- File: `company_type/id.bin` (rows = 4); identity. No kind predicate in Q22a — every ct id is acceptable, but `ct.id = mc.company_type_id` still requires `mc.company_type_id` be non-NULL and in [1..4]. Effectively a no-op join.

### info_type.id (PK, int32_t)
- File: `info_type/id.bin` (rows = 113); identity.
- Use: targets `it1_id` ('countries'), `it2_id` ('rating').

### info_type.info (varlen)
- Files: `info_type/info.off`, `info_type/info.dat` (rows = 113)
- Use: scan TWICE to resolve `it1_id = id where info=='countries'` and `it2_id = id where info=='rating'`. Two separate ids on the same dimension.

### keyword.id (PK, int32_t)
- File: `keyword/id.bin` (rows = 134170); identity.
- Use: produce set of keyword ids matching the IN-list.

### keyword.keyword (varlen)
- Files: `keyword/keyword.off`, `keyword/keyword.dat` (rows = 134170)
- Use: linear scan for each of {`murder`, `murder-in-title`, `blood`, `violence`} (early-exit per literal). Collect into `flat_hash_set<int32_t> kw_ids`.

### kind_type.id (PK, int32_t)
- File: `kind_type/id.bin` (rows = 7); identity.

### kind_type.kind (varlen)
- Files: `kind_type/kind.off`, `kind_type/kind.dat` (rows = 7)
- Use: scan to resolve {`movie`, `episode`} → `flat_hash_set<int32_t> kt_ids` (size 2).

### movie_companies.movie_id (FK to title, int32_t)
- File: `movie_companies/movie_id.bin` (rows = 2609129); FK-sorted.
- Use: iterate via offsets_only `movie_companies__movie_id`.

### movie_companies.company_id (FK to company_name, int32_t)
- File: `movie_companies/company_id.bin` (rows = 2609129)
- Use: read mc rows then test `country_code_bin[company_id-1]` against `us_code`/NULL.

### movie_companies.company_type_id (FK to company_type, int32_t)
- File: `movie_companies/company_type_id.bin` — used only as join key.

### movie_companies.note (varlen)
- Files: `movie_companies/note.off`, `movie_companies/note.dat`
- Use: two `memmem` calls per surviving row — NOT LIKE `'%(USA)%'` AND LIKE `'%(200%)%'`. NULL (empty slice) fails the positive LIKE → row drops.

### movie_info.movie_id (FK to title)
- File: `movie_info/movie_id.bin` (rows = 14835720); FK-sorted.
- Use: offsets_only `movie_info__movie_id`.

### movie_info.info_type_id
- File: `movie_info/info_type_id.bin`
- Use: filter `== it1_id`.

### movie_info.info (varlen)
- Files: `movie_info/info.off`, `movie_info/info.dat`
- Use: equality against the IN-set `{Germany,German,USA,American}` — build `flat_hash_set<string_view>`; length-prefilter then compare.

### movie_info_idx.movie_id (FK to title)
- File: `movie_info_idx/movie_id.bin` (rows = 1380035); FK-sorted.
- Use: offsets_only `movie_info_idx__movie_id`.

### movie_info_idx.info_type_id
- File: `movie_info_idx/info_type_id.bin`
- Use: filter `== it2_id`.

### movie_info_idx.info (varlen)
- Files: `movie_info_idx/info.off`, `movie_info_idx/info.dat`
- Use: lexicographic compare `< "7.0"` via `memcmp` on the slice (this is a string compare, not numeric). Also projected as `MIN(mi_idx.info)`.

### movie_keyword.movie_id (FK to title)
- File: `movie_keyword/movie_id.bin` (rows = 4523930); FK-sorted.
- Use: offsets_only `movie_keyword__movie_id`.

### movie_keyword.keyword_id
- File: `movie_keyword/keyword_id.bin`
- Use: test `kw_ids.contains(...)`.

### title.id, title.title, title.kind_id, title.production_year
- Files: `title/id.bin`, `title/title.off|.dat`, `title/kind_id.bin`, `title/production_year.bin` (rows = 2528312).
- Use: driver. Filter `kt_ids.contains(kind_id[v-1])` AND `production_year[v-1] > 2008` (treat `INT32_MIN` as fail). Project `MIN(t.title)`.

## Table Stats
| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| company_type | 4 | dim (free join) | id | — |
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
- Join graph: title is the hub; mc, mi, mi_idx, mk all join on movie_id. Then mc→cn, mc→ct, mk→k, mi→it1, mi_idx→it2, t→kt.
- Resolve before driver: `us_code` (cn.country_code dict), `it1_id` and `it2_id` (two linear scans of info_type/info), `kw_ids` (4 scans of keyword), `kt_ids` (1 scan of kind_type for 2 literals).
- Driver: iterate title rows v = 1..2528312. Cheap filters first: `production_year > 2008` then `kt_ids.contains(kind_id)`. For survivors, look up each fact via offsets_only index and AND-filter.
- Per-survivor order: mk (cheapest; small IN-set on keyword_id), then mi_idx (it2_id + lex compare), then mi (it1_id + IN-set on info), then mc (note 2× memmem, then company_id → country_code dict probe). Stop at first failing fact, but the SQL requires EXISTS across all four ⇒ all must have ≥1 surviving row.
- Selectivities: `production_year > 2008` ≈ recent slice; `kt_ids` 2/7; `mc.note` LIKE+NOT LIKE very selective; `cn.country_code != '[us]'` ≈ majority pass; `mi.info` ∈ 4-set is selective; `mi_idx.info < '7.0'` is lex string compare, ~moderate selectivity.
- MIN aggregation: keep running min for cn.name (varlen), mi_idx.info (varlen), t.title (varlen). Update only when all clauses for that combination satisfied. No GROUP BY.
- LIKE notes: `mc.note` requires both NOT-LIKE and LIKE → two `memmem` calls per row; empty slice (NULL) ⇒ drop.

## Indexes
- `movie_companies__movie_id` (offsets_only): `_idx/movie_companies__movie_id__offsets.bin` (int32, size 2528314). Slot 0 = count of mc with movie_id < 1.
- `movie_info__movie_id` (offsets_only): `_idx/movie_info__movie_id__offsets.bin` (size 2528314).
- `movie_info_idx__movie_id` (offsets_only): `_idx/movie_info_idx__movie_id__offsets.bin` (size 2528314).
- `movie_keyword__movie_id` (offsets_only): `_idx/movie_keyword__movie_id__offsets.bin` (size 2528314).

Query-time snippet (uniform for all four):
```cpp
int32_t lo = off[v], hi = off[v+1];
for (int32_t r = lo; r < hi; ++r) { /* child row r */ }
```
- No index used on cn.country_code, mc.note, mi.info, mi_idx.info, k.keyword, kt.kind, it.info — those are scanned/looked up by direct varlen access.
