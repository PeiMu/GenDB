# Q31a Guide

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
  AND mi.info IN ('Horror','Thriller')
  AND n.gender='m'
  AND t.id=mi.movie_id AND t.id=mi_idx.movie_id AND t.id=ci.movie_id
  AND t.id=mk.movie_id AND t.id=mc.movie_id
  AND n.id=ci.person_id
  AND it1.id=mi.info_type_id AND it2.id=mi_idx.info_type_id
  AND k.id=mk.keyword_id AND cn.id=mc.company_id;
```

## Column Reference

### company_name.name (varlen, rows=234997)
Files: `company_name/name.{off,dat}`. Scan for `Lionsgate%` prefix: per row `i`, test `len = off[i+1]-off[i] >= 9 && memcmp(dat+off[i], "Lionsgate", 9)==0`. Collected ids = `i+1` (dense PK) → `cn_ids` (handful).

### cast_info.note (varlen, rows=36244344)
Files: `cast_info/note.{off,dat}`. 5-literal writer set membership.

### cast_info.movie_id, person_id (int32, rows=36244344)
Files: `cast_info/{movie_id,person_id}.bin`. Sorted by movie_id; offsets_only index.

### info_type.info (varlen, rows=113)
Files: `info_type/info.{off,dat}`. Resolve `genres`→`it1_id`, `votes`→`it2_id`.

### keyword.keyword (varlen, rows=134170)
Files: `keyword/keyword.{off,dat}`. Resolve 7-literal IN-set → `k_ids`.

### movie_companies.movie_id, company_id (int32, rows=2609129)
Files: `movie_companies/{movie_id,company_id}.bin`. Sorted by movie_id; offsets_only `movie_companies__movie_id`. Alternative drive path via CSR `movie_companies__company_id` from `cn_ids` is more selective (Lionsgate set tiny).

### movie_info.movie_id, info_type_id, info (rows=14835720)
Files: `movie_info/{movie_id,info_type_id}.bin`, `info.{off,dat}`. Filter `info_type_id==it1_id` AND `info∈{Horror,Thriller}`. Captured for MIN.

### movie_info_idx.movie_id, info_type_id, info (rows=1380035)
Files: `movie_info_idx/{movie_id,info_type_id}.bin`, `info.{off,dat}`. Filter `info_type_id==it2_id`. Captured for MIN.

### movie_keyword.movie_id, keyword_id (int32, rows=4523930)
Files: `movie_keyword/{movie_id,keyword_id}.bin`. Offsets_only on movie_id; CSR `movie_keyword__keyword_id` available if driving from k_ids.

### name.gender (int8 dict, rows=4167491)
Files: `name/gender.bin`, `name/gender.dict.{off,dat}`. Resolve `m`→`m_code`.

### name.name (varlen, rows=4167491)
Files: `name/name.{off,dat}`. Projected via MIN.

### title.id, title.title (rows=2528312)
Files: `title/id.bin` (identity), `title/title.{off,dat}`. No year filter in Q31a.

## Table Stats
| Table | Rows | Role | Sort |
|---|---|---|---|
| company_name | 234,997 | dim/driver | id |
| info_type | 113 | dim | id |
| keyword | 134,170 | dim | id |
| name | 4,167,491 | dim | id |
| title | 2,528,312 | dim | id |
| movie_companies | 2,609,129 | fact | movie_id |
| cast_info | 36,244,344 | fact | movie_id |
| movie_info | 14,835,720 | fact | movie_id |
| movie_info_idx | 1,380,035 | fact | movie_id |
| movie_keyword | 4,523,930 | fact | movie_id |

## Query Analysis
- Join graph: `cn -- mc -- t -- {mi,mi_idx,mk,ci}`; `it1 -- mi`, `it2 -- mi_idx`, `k -- mk`, `n -- ci`.
- Driver: `cn.name LIKE 'Lionsgate%'` is the most selective filter (anchored prefix). Steps:
  1. Scan `company_name/name.{off,dat}` for prefix `Lionsgate` → `cn_ids` (very small).
  2. For each `cid ∈ cn_ids`: walk CSR `movie_companies__company_id` to get mc rowids → `mv = movie_id[r]` → candidate movie set.
- Per candidate `mv`:
  1. `movie_keyword__movie_id` → require `keyword_id ∈ k_ids`.
  2. `movie_info__movie_id` → require `info_type_id==it1_id ∧ info∈{Horror,Thriller}`; capture for MIN.
  3. `movie_info_idx__movie_id` → require `info_type_id==it2_id`; capture for MIN.
  4. `cast_info__movie_id` → for each ci with writer-note, fetch `pid`, gender check, capture name for MIN.
- Selectivities: Lionsgate prefix → ~10s of cn ids; mc per Lionsgate company ~10²–10³ movies; mi Horror/Thriller after it1 narrow is ~1–2%; mk over k_ids low.
- MIN aggregation: four running minima.
- LIKE notes: `'Lionsgate%'` is a prefix anchored at start → use memcmp on first 9 bytes (not memmem). No other LIKE in Q31a.

## Indexes
- `movie_companies__company_id` (CSR): `_idx/movie_companies__company_id__offsets.bin` (int32, 234999), `_idx/movie_companies__company_id__rowids.bin` (int32, 2609129).
  ```cpp
  int32_t lo = mcc_off[cid], hi = mcc_off[cid+1];
  for (int32_t k=lo; k<hi; ++k) { int32_t r = mcc_row[k]; int32_t mv = mc_movie_id[r]; }
  ```
- `movie_keyword__movie_id` offsets_only `_idx/movie_keyword__movie_id__offsets.bin` (int32, 2528314).
- `movie_info__movie_id` offsets_only `_idx/movie_info__movie_id__offsets.bin`.
- `movie_info_idx__movie_id` offsets_only `_idx/movie_info_idx__movie_id__offsets.bin`.
- `cast_info__movie_id` offsets_only `_idx/cast_info__movie_id__offsets.bin`.
- (`movie_keyword__keyword_id` CSR optional alt-driver from k_ids; not primary here.)
- No invented indexes; no hardcoded ids; gender dict resolved at runtime.
