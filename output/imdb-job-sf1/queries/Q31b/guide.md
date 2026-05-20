# Q31b Guide

## SQL
```sql
SELECT MIN(mi.info) AS movie_budget, MIN(mi_idx.info) AS movie_votes,
       MIN(n.name) AS writer, MIN(t.title) AS violent_liongate_movie
FROM cast_info ci, company_name cn, info_type it1, info_type it2,
     keyword k, movie_companies mc, movie_info mi, movie_info_idx mi_idx,
     movie_keyword mk, name n, title t
WHERE ci.note IN ('(writer)','(head writer)','(written by)','(story)','(story editor)')
  AND cn.name LIKE 'Lionsgate%'
  AND it1.info='genres' AND it2.info='votes'
  AND k.keyword IN ('murder','violence','blood','gore','death','female-nudity','hospital')
  AND mc.note LIKE '%(Blu-ray)%'
  AND mi.info IN ('Horror','Thriller')
  AND n.gender='m' AND t.production_year > 2000
  AND (t.title LIKE '%Freddy%' OR t.title LIKE '%Jason%' OR t.title LIKE 'Saw%')
  AND t.id=mi.movie_id AND t.id=mi_idx.movie_id AND t.id=ci.movie_id
  AND t.id=mk.movie_id AND t.id=mc.movie_id
  AND n.id=ci.person_id
  AND it1.id=mi.info_type_id AND it2.id=mi_idx.info_type_id
  AND k.id=mk.keyword_id AND cn.id=mc.company_id;
```

## Column Reference

### company_name.name (varlen, rows=234997)
Files: `company_name/name.{off,dat}`. Scan for `Lionsgate%` prefix → memcmp first 9 bytes. Collect `cn_ids`.

### cast_info.note (varlen, rows=36244344)
Files: `cast_info/note.{off,dat}`. Writer 5-literal IN.

### cast_info.movie_id, person_id (int32, rows=36244344)
Files: `cast_info/{movie_id,person_id}.bin`.

### info_type.info (varlen, rows=113)
Files: `info_type/info.{off,dat}`. Resolve `genres`→`it1_id`, `votes`→`it2_id`.

### keyword.keyword (varlen, rows=134170)
Files: `keyword/keyword.{off,dat}`. Resolve 7-literal IN-set → `k_ids`.

### movie_companies.movie_id, company_id (int32, rows=2609129)
Files: `movie_companies/{movie_id,company_id}.bin`.

### movie_companies.note (varlen, rows=2609129)
Files: `movie_companies/note.{off,dat}`. LIKE `%(Blu-ray)%` via `memmem` on row slice; length prefilter ≥9. NULL (empty) fails the LIKE.

### movie_info.movie_id, info_type_id, info (rows=14835720)
Files: `movie_info/{movie_id,info_type_id}.bin`, `info.{off,dat}`. Filter `it1_id`, then 2-set membership; project for MIN.

### movie_info_idx.movie_id, info_type_id, info (rows=1380035)
Files: `movie_info_idx/{movie_id,info_type_id}.bin`, `info.{off,dat}`. Filter `it2_id`; project for MIN.

### movie_keyword.movie_id, keyword_id (int32, rows=4523930)
Files: `movie_keyword/{movie_id,keyword_id}.bin`.

### name.gender, name.name (rows=4167491)
Files: `name/gender.bin` + `gender.dict.{off,dat}`; `name/name.{off,dat}`. Gender code resolved at runtime.

### title.id, production_year, title (rows=2528312)
Files: `title/id.bin` identity; `title/production_year.bin`; `title/title.{off,dat}`. Filter `production_year>2000` AND title LIKE one of {`%Freddy%`,`%Jason%`,`Saw%`}.

## Table Stats
| Table | Rows | Role | Sort |
|---|---|---|---|
| company_name | 234,997 | dim/driver | id |
| info_type | 113 | dim | id |
| keyword | 134,170 | dim | id |
| name | 4,167,491 | dim | id |
| title | 2,528,312 | filter | id |
| movie_companies | 2,609,129 | fact | movie_id |
| cast_info | 36,244,344 | fact | movie_id |
| movie_info | 14,835,720 | fact | movie_id |
| movie_info_idx | 1,380,035 | fact | movie_id |
| movie_keyword | 4,523,930 | fact | movie_id |

## Query Analysis
- Two highly selective filters: Lionsgate% on cn AND title-LIKE set on t. Either can drive.
- Recommended driver: title LIKE OR set + year>2000 → `mv_set` (hundreds). Smaller fan-out than Lionsgate→mc→mv.
- Per `mv`:
  1. `movie_companies__movie_id` → scan range; for each mc row require `company_id ∈ cn_ids` (precomputed from Lionsgate scan) AND `mc.note` matches `%(Blu-ray)%`.
  2. `movie_keyword__movie_id` → require `keyword_id ∈ k_ids`.
  3. `movie_info__movie_id` → require row with `it1_id ∧ info∈{Horror,Thriller}`; capture for MIN.
  4. `movie_info_idx__movie_id` → require `it2_id`; capture for MIN.
  5. `cast_info__movie_id` → writer-note rows; check gender for pid; capture name for MIN.
- Selectivities: title LIKE set + year>2000 → ~10²; Lionsgate cn_ids ~10s; mc.note Blu-ray rare overall (<<1%).
- MIN: four running minima.
- LIKE notes:
  - `cn.name LIKE 'Lionsgate%'` — prefix → memcmp first 9 bytes.
  - `mc.note LIKE '%(Blu-ray)%'` — substring → memmem (length prefix ≥9).
  - `t.title LIKE '%Freddy%' | '%Jason%'` — substring → memmem (length ≥5).
  - `t.title LIKE 'Saw%'` — prefix → memcmp first 3 bytes.

## Indexes
- `movie_companies__movie_id` offsets_only `_idx/movie_companies__movie_id__offsets.bin` (int32, 2528314).
- `movie_companies__company_id` (CSR) `_idx/movie_companies__company_id__offsets.bin` (234999), `_idx/movie_companies__company_id__rowids.bin` (2609129) — optional alt-drive from `cn_ids`.
- `movie_keyword__movie_id` offsets_only `_idx/movie_keyword__movie_id__offsets.bin`.
- `movie_info__movie_id` offsets_only `_idx/movie_info__movie_id__offsets.bin`.
- `movie_info_idx__movie_id` offsets_only `_idx/movie_info_idx__movie_id__offsets.bin`.
- `cast_info__movie_id` offsets_only `_idx/cast_info__movie_id__offsets.bin`.
- Snippet:
  ```cpp
  int32_t lo = off[mv], hi = off[mv+1];
  for (int32_t r = lo; r < hi; ++r) { /* child row r */ }
  ```
- No invented text indexes; LIKE/prefix done by linear scan.
