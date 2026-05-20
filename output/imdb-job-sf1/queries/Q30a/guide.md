# Q30a Guide

## SQL
```sql
SELECT MIN(mi.info) AS movie_budget, MIN(mi_idx.info) AS movie_votes,
       MIN(n.name) AS writer, MIN(t.title) AS complete_violent_movie
FROM complete_cast cc, comp_cast_type cct1, comp_cast_type cct2,
     cast_info ci, info_type it1, info_type it2, keyword k,
     movie_info mi, movie_info_idx mi_idx, movie_keyword mk,
     name n, title t
WHERE cct1.kind IN ('cast','crew')
  AND cct2.kind ='complete+verified'
  AND ci.note IN ('(writer)','(head writer)','(written by)','(story)','(story editor)')
  AND it1.info = 'genres' AND it2.info = 'votes'
  AND k.keyword IN ('murder','violence','blood','gore','death','female-nudity','hospital')
  AND mi.info IN ('Horror','Thriller')
  AND n.gender = 'm' AND t.production_year > 2000
  AND t.id = mi.movie_id AND t.id = mi_idx.movie_id AND t.id = ci.movie_id
  AND t.id = mk.movie_id AND t.id = cc.movie_id
  AND n.id = ci.person_id
  AND it1.id = mi.info_type_id AND it2.id = mi_idx.info_type_id
  AND k.id = mk.keyword_id
  AND cct1.id = cc.subject_id AND cct2.id = cc.status_id;
```

## Column Reference

### comp_cast_type.kind (varlen, rows=4)
Files: `comp_cast_type/kind.off`, `comp_cast_type/kind.dat`. Scan to resolve `'cast'`,`'crew'` → `cct1_ids` (set of 1-2) and `'complete+verified'` → `cct2_id` (single). Pattern: row i ↔ id (i+1).

### cast_info.note (varlen, rows=36244344)
Files: `cast_info/note.off`, `cast_info/note.dat`. Build `flat_hash_set<string_view>` of 5 literals; per row test slice `[off[r],off[r+1])` equality.

### cast_info.movie_id, cast_info.person_id (int32, rows=36244344)
Files: `cast_info/movie_id.bin`, `cast_info/person_id.bin`. Sorted by movie_id; iterate via offsets_only `cast_info__movie_id`.

### info_type.info (varlen, rows=113)
Files: `info_type/info.off`, `info_type/info.dat`. Resolve `'genres'` → `it1_id`, `'votes'` → `it2_id`.

### keyword.keyword (varlen, rows=134170)
Files: `keyword/keyword.off`, `keyword/keyword.dat`. Resolve the 7-literal IN-set → `k_ids` (≤7 ids).

### movie_info.movie_id, info_type_id (int32, rows=14835720)
Files: `movie_info/movie_id.bin`, `movie_info/info_type_id.bin`. Sorted by movie_id; offsets_only `movie_info__movie_id`.

### movie_info.info (varlen, rows=14835720)
Files: `movie_info/info.off`, `movie_info/info.dat`. After narrowing to `info_type_id==it1_id`, equality check against {`Horror`,`Thriller`}; length prefilter (5–8 bytes).

### movie_info_idx.movie_id, info_type_id (int32, rows=1380035)
Files: `movie_info_idx/movie_id.bin`, `movie_info_idx/info_type_id.bin`. Offsets_only `movie_info_idx__movie_id`.

### movie_info_idx.info (varlen, rows=1380035)
Files: `movie_info_idx/info.off`, `movie_info_idx/info.dat`. Projected via `MIN`; no predicate beyond `it2_id` join here.

### movie_keyword.movie_id, keyword_id (int32, rows=4523930)
Files: `movie_keyword/movie_id.bin`, `movie_keyword/keyword_id.bin`. Offsets_only `movie_keyword__movie_id`.

### complete_cast.movie_id, subject_id, status_id (int32, rows=135086)
Files: `complete_cast/movie_id.bin`, `subject_id.bin`, `status_id.bin`. Offsets_only `complete_cast__movie_id`. Nullable movie_id (`INT32_MIN`).

### name.gender (int8 dict, rows=4167491)
Files: `name/gender.bin`, `name/gender.dict.off`, `name/gender.dict.dat`. Resolve `'m'` → `m_code` once; test `gender_bin[pid-1] == m_code`.

### name.name (varlen, rows=4167491)
Files: `name/name.off`, `name/name.dat`. Projected via `MIN(n.name)`; row index = `pid - 1` (dense PK).

### title.id (int32, rows=2528312)
File: `title/id.bin` (identity). Row i ↔ id (i+1).

### title.production_year (int32 nullable, rows=2528312)
File: `title/production_year.bin`. Filter `> 2000`; skip `INT32_MIN`.

### title.title (varlen, rows=2528312)
Files: `title/title.off`, `title/title.dat`. Projected via `MIN(t.title)`.

## Table Stats
| Table | Rows | Role | Sort | Block |
|---|---|---|---|---|
| comp_cast_type | 4 | dim | id | — |
| info_type | 113 | dim | id | — |
| keyword | 134,170 | dim | id | 100000 |
| name | 4,167,491 | dim | id | 100000 |
| title | 2,528,312 | dim/driver | id | 100000 |
| complete_cast | 135,086 | fact | movie_id | 100000 |
| cast_info | 36,244,344 | fact | movie_id | 200000 |
| movie_info | 14,835,720 | fact | movie_id | 200000 |
| movie_info_idx | 1,380,035 | fact | movie_id | 100000 |
| movie_keyword | 4,523,930 | fact | movie_id | 100000 |

## Query Analysis
- Join graph (star on `t.id` = movie_id):
  ```
  cct1 -- cc(subject_id), cct2 -- cc(status_id)
  k -- mk -- t -- ci -- n
                 -- mi -- it1
                 -- mi_idx -- it2
                 -- cc
  ```
- Driver: `complete_cast` is by far the smallest fact (135K rows). Two viable plans:
  (1) Scan `complete_cast` linearly, filter on `subject_id∈cct1_ids ∧ status_id==cct2_id`, take `mv=movie_id`, then probe other facts via offsets_only.
  (2) Iterate `mv=1..2528312` with `production_year>2000` filter first, then probe `complete_cast__movie_id` to require non-empty match.
  Plan (1) prunes hardest first (cct1/cct2 each cut ~75% of cc rows).
- Per surviving `mv`:
  1. `title.production_year[mv-1] > 2000`.
  2. `movie_keyword__movie_id` → require any row with `keyword_id ∈ k_ids`.
  3. `movie_info__movie_id` → require row with `info_type_id==it1_id ∧ info∈{Horror,Thriller}`; capture `mi.info` for MIN.
  4. `movie_info_idx__movie_id` → require row with `info_type_id==it2_id`; capture `mi_idx.info` for MIN.
  5. `cast_info__movie_id` → for each ci row with `note∈writer_set`, fetch `pid=person_id`; test `name.gender[pid-1]==m_code`; capture `name.name` for MIN.
- Selectivities: `production_year>2000` ~30% titles; mi `Horror/Thriller` after it1 narrow is ~1–2% of mi rows; k_ids hit on mk is small; cct1×cct2 cuts cc to ~25%.
- MIN aggregation: maintain four running minima (3 varlen lex-min, 1 implicit none). Only read varlen `.dat` slices for surviving combinations.
- No LIKE in Q30a.

## Indexes
- `complete_cast__movie_id` (offsets_only): `_idx/complete_cast__movie_id__offsets.bin` (int32, size 2528314).
  ```cpp
  int32_t lo = cc_off[mv], hi = cc_off[mv+1];
  for (int32_t r = lo; r < hi; ++r) { /* cc row r */ }
  ```
- `movie_keyword__movie_id` (offsets_only): `_idx/movie_keyword__movie_id__offsets.bin` (int32, 2528314).
- `movie_info__movie_id` (offsets_only): `_idx/movie_info__movie_id__offsets.bin` (int32, 2528314).
- `movie_info_idx__movie_id` (offsets_only): `_idx/movie_info_idx__movie_id__offsets.bin` (int32, 2528314).
- `cast_info__movie_id` (offsets_only): `_idx/cast_info__movie_id__offsets.bin` (int32, 2528314).
- Slot 0 holds count of FK<1 (NULLs); real parents start at slot 1.
- No invented indexes; dict codes resolved at runtime; no hardcoded ids.
