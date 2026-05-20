# Q30b Guide

## SQL
```sql
SELECT MIN(mi.info) AS movie_budget, MIN(mi_idx.info) AS movie_votes,
       MIN(n.name) AS writer, MIN(t.title) AS complete_gore_movie
FROM complete_cast cc, comp_cast_type cct1, comp_cast_type cct2,
     cast_info ci, info_type it1, info_type it2, keyword k,
     movie_info mi, movie_info_idx mi_idx, movie_keyword mk,
     name n, title t
WHERE cct1.kind IN ('cast','crew') AND cct2.kind='complete+verified'
  AND ci.note IN ('(writer)','(head writer)','(written by)','(story)','(story editor)')
  AND it1.info='genres' AND it2.info='votes'
  AND k.keyword IN ('murder','violence','blood','gore','death','female-nudity','hospital')
  AND mi.info IN ('Horror','Thriller')
  AND n.gender='m' AND t.production_year > 2000
  AND (t.title LIKE '%Freddy%' OR t.title LIKE '%Jason%' OR t.title LIKE 'Saw%')
  AND t.id=mi.movie_id AND t.id=mi_idx.movie_id AND t.id=ci.movie_id
  AND t.id=mk.movie_id AND t.id=cc.movie_id
  AND n.id=ci.person_id
  AND it1.id=mi.info_type_id AND it2.id=mi_idx.info_type_id
  AND k.id=mk.keyword_id
  AND cct1.id=cc.subject_id AND cct2.id=cc.status_id;
```

## Column Reference

### comp_cast_type.kind (varlen, rows=4)
Files: `comp_cast_type/kind.{off,dat}`. Resolve IN-set `{cast,crew}` → `cct1_ids`; `complete+verified` → `cct2_id`.

### cast_info.note (varlen, rows=36244344)
Files: `cast_info/note.{off,dat}`. Equality test against 5-literal writer-set per row.

### cast_info.movie_id, person_id (int32, rows=36244344)
Files: `cast_info/movie_id.bin`, `cast_info/person_id.bin`. Sorted by movie_id; offsets_only.

### info_type.info (varlen, rows=113)
Files: `info_type/info.{off,dat}`. Resolve `genres`→`it1_id`, `votes`→`it2_id`.

### keyword.keyword (varlen, rows=134170)
Files: `keyword/keyword.{off,dat}`. Resolve 7-literal IN-set → `k_ids`.

### movie_info.movie_id, info_type_id, info (rows=14835720)
Files: `movie_info/{movie_id,info_type_id}.bin`, `movie_info/info.{off,dat}`. Filter `info_type_id==it1_id`, then info∈{Horror,Thriller}. Projected for MIN.

### movie_info_idx.movie_id, info_type_id, info (rows=1380035)
Files: `movie_info_idx/{movie_id,info_type_id}.bin`, `info.{off,dat}`. Filter `info_type_id==it2_id`. Projected for MIN.

### movie_keyword.movie_id, keyword_id (int32, rows=4523930)
Files: `movie_keyword/{movie_id,keyword_id}.bin`. Offsets_only on movie_id.

### complete_cast.movie_id, subject_id, status_id (int32, rows=135086)
Files: `complete_cast/{movie_id,subject_id,status_id}.bin`. movie_id is nullable (`INT32_MIN`).

### name.gender (int8 dict, rows=4167491)
Files: `name/gender.bin`, `name/gender.dict.{off,dat}`. Resolve `m`→`m_code`.

### name.name (varlen, rows=4167491)
Files: `name/name.{off,dat}`. Projected via MIN; row index = pid-1.

### title.id (int32, rows=2528312)
File: `title/id.bin` identity, row i ↔ id (i+1).

### title.production_year (int32 nullable, rows=2528312)
File: `title/production_year.bin`. Filter `> 2000` (skip `INT32_MIN`).

### title.title (varlen, rows=2528312)
Files: `title/title.{off,dat}`. LIKE `%Freddy%` OR `%Jason%` OR `Saw%`. Use `memmem` on slice for two substring patterns; prefix `Saw%` is `memcmp` against first 3 bytes with `len>=3`. Projected via MIN.

## Table Stats
| Table | Rows | Role | Sort |
|---|---|---|---|
| comp_cast_type | 4 | dim | id |
| info_type | 113 | dim | id |
| keyword | 134,170 | dim | id |
| name | 4,167,491 | dim | id |
| title | 2,528,312 | driver | id |
| complete_cast | 135,086 | fact | movie_id |
| cast_info | 36,244,344 | fact | movie_id |
| movie_info | 14,835,720 | fact | movie_id |
| movie_info_idx | 1,380,035 | fact | movie_id |
| movie_keyword | 4,523,930 | fact | movie_id |

## Query Analysis
- Same star as Q30a plus title-LIKE filter that is highly selective.
- Driver: scan `title.title.{off,dat}` first to collect `mv_set` matching `%Freddy% ∨ %Jason% ∨ Saw%` AND `production_year[mv-1]>2000`. Likely <1K titles.
- Per surviving `mv`:
  1. `complete_cast__movie_id` range → require row with `subject_id∈cct1_ids ∧ status_id==cct2_id`.
  2. `movie_keyword__movie_id` → require `keyword_id ∈ k_ids`.
  3. `movie_info__movie_id` → require `info_type_id==it1_id ∧ info∈{Horror,Thriller}`; capture for MIN.
  4. `movie_info_idx__movie_id` → require `info_type_id==it2_id`; capture for MIN.
  5. `cast_info__movie_id` → for each writer-note row, fetch `pid`; gender check; capture name.
- Selectivities: title LIKE set tiny (~hundreds); each subsequent probe further prunes.
- MIN: four running minima.
- LIKE: `%Freddy%`/`%Jason%` via `memmem` (length prefilter ≥5); `Saw%` is prefix → memcmp first 3 bytes.

## Indexes
- `movie_keyword__movie_id` offsets_only `_idx/movie_keyword__movie_id__offsets.bin` (int32, 2528314).
- `movie_info__movie_id` offsets_only `_idx/movie_info__movie_id__offsets.bin`.
- `movie_info_idx__movie_id` offsets_only `_idx/movie_info_idx__movie_id__offsets.bin`.
- `cast_info__movie_id` offsets_only `_idx/cast_info__movie_id__offsets.bin`.
- `complete_cast__movie_id` offsets_only `_idx/complete_cast__movie_id__offsets.bin`.
- Snippet:
  ```cpp
  int32_t lo = off[mv], hi = off[mv+1];
  for (int32_t r = lo; r < hi; ++r) { /* child row r */ }
  ```
- No invented index on title text; LIKE handled by linear scan only.
