# Q30c Guide

## SQL
```sql
SELECT MIN(mi.info) AS movie_budget, MIN(mi_idx.info) AS movie_votes,
       MIN(n.name) AS writer, MIN(t.title) AS complete_violent_movie
FROM complete_cast cc, comp_cast_type cct1, comp_cast_type cct2,
     cast_info ci, info_type it1, info_type it2, keyword k,
     movie_info mi, movie_info_idx mi_idx, movie_keyword mk,
     name n, title t
WHERE cct1.kind = 'cast' AND cct2.kind='complete+verified'
  AND ci.note IN ('(writer)','(head writer)','(written by)','(story)','(story editor)')
  AND it1.info='genres' AND it2.info='votes'
  AND k.keyword IN ('murder','violence','blood','gore','death','female-nudity','hospital')
  AND mi.info IN ('Horror','Action','Sci-Fi','Thriller','Crime','War')
  AND n.gender='m'
  AND t.id=mi.movie_id AND t.id=mi_idx.movie_id AND t.id=ci.movie_id
  AND t.id=mk.movie_id AND t.id=cc.movie_id
  AND n.id=ci.person_id
  AND it1.id=mi.info_type_id AND it2.id=mi_idx.info_type_id
  AND k.id=mk.keyword_id
  AND cct1.id=cc.subject_id AND cct2.id=cc.status_id;
```

## Column Reference

### comp_cast_type.kind (varlen, rows=4)
Files: `comp_cast_type/kind.{off,dat}`. Resolve `cast`→`cct1_id` (single), `complete+verified`→`cct2_id` (single).

### cast_info.note (varlen, rows=36244344)
Files: `cast_info/note.{off,dat}`. Per-row equality vs 5 writer-note literals.

### cast_info.movie_id, person_id (int32, rows=36244344)
Files: `cast_info/{movie_id,person_id}.bin`. Sorted by movie_id; offsets_only index.

### info_type.info (varlen, rows=113)
Files: `info_type/info.{off,dat}`. Resolve `genres`→`it1_id`, `votes`→`it2_id`.

### keyword.keyword (varlen, rows=134170)
Files: `keyword/keyword.{off,dat}`. Resolve 7-literal IN-set → `k_ids`.

### movie_info.movie_id, info_type_id, info (rows=14835720)
Files: `movie_info/{movie_id,info_type_id}.bin`, `info.{off,dat}`. Filter `info_type_id==it1_id`, then info∈{Horror,Action,Sci-Fi,Thriller,Crime,War}. Build `flat_hash_set<string_view>` for the 6-literal set; length prefilter (3–8 bytes).

### movie_info_idx.movie_id, info_type_id, info (rows=1380035)
Files: `movie_info_idx/{movie_id,info_type_id}.bin`, `info.{off,dat}`. Filter `info_type_id==it2_id`. Projected for MIN.

### movie_keyword.movie_id, keyword_id (int32, rows=4523930)
Files: `movie_keyword/{movie_id,keyword_id}.bin`.

### complete_cast.movie_id, subject_id, status_id (int32, rows=135086)
Files: `complete_cast/{movie_id,subject_id,status_id}.bin`. cct1.kind='cast' (singleton) → `subject_id==cct1_id`.

### name.gender (int8 dict, rows=4167491)
Files: `name/gender.bin`, `name/gender.dict.{off,dat}`. Resolve `m`→`m_code`.

### name.name (varlen, rows=4167491)
Files: `name/name.{off,dat}`. Projected via MIN.

### title.id, title.title (rows=2528312)
Files: `title/id.bin` (identity), `title/title.{off,dat}`. No production_year predicate in Q30c.

## Table Stats
| Table | Rows | Role | Sort |
|---|---|---|---|
| comp_cast_type | 4 | dim | id |
| info_type | 113 | dim | id |
| keyword | 134,170 | dim | id |
| name | 4,167,491 | dim | id |
| title | 2,528,312 | dim | id |
| complete_cast | 135,086 | fact/driver | movie_id |
| cast_info | 36,244,344 | fact | movie_id |
| movie_info | 14,835,720 | fact | movie_id |
| movie_info_idx | 1,380,035 | fact | movie_id |
| movie_keyword | 4,523,930 | fact | movie_id |

## Query Analysis
- Star schema with `t.id` central. cct1='cast' is a single id (stricter than Q30a/b's IN-2).
- Driver: scan `complete_cast` (135K rows). Filter `subject_id==cct1_id ∧ status_id==cct2_id`; collect distinct `mv` set (~10K). No title-year/title-LIKE filter to bound the scan further.
- Per `mv`:
  1. `movie_keyword__movie_id` → require any `keyword_id ∈ k_ids`.
  2. `movie_info__movie_id` → require row with `info_type_id==it1_id ∧ info∈{6-set}`; capture for MIN.
  3. `movie_info_idx__movie_id` → require row with `info_type_id==it2_id`; capture for MIN.
  4. `cast_info__movie_id` → for each ci with writer-note, fetch `pid`; `gender[pid-1]==m_code`; capture `name.name` for MIN.
- Selectivities: cct1='cast' alone narrower than IN; mi 6-genre set wider than Q30a/b; no year cutoff.
- MIN aggregation: four running minima.
- No LIKE in Q30c.

## Indexes
- `complete_cast__movie_id` offsets_only `_idx/complete_cast__movie_id__offsets.bin` (int32, 2528314). Driver iteration scans the full child array, not via parent slot.
- `movie_keyword__movie_id` offsets_only `_idx/movie_keyword__movie_id__offsets.bin`.
- `movie_info__movie_id` offsets_only `_idx/movie_info__movie_id__offsets.bin`.
- `movie_info_idx__movie_id` offsets_only `_idx/movie_info_idx__movie_id__offsets.bin`.
- `cast_info__movie_id` offsets_only `_idx/cast_info__movie_id__offsets.bin`.
- Snippet:
  ```cpp
  int32_t lo = off[mv], hi = off[mv+1];
  for (int32_t r = lo; r < hi; ++r) { /* child row r */ }
  ```
- No invented indexes (no `cast_info__note`, no text indexes); dict code for gender resolved at runtime; no hardcoded ids.
