# Q33c Guide

## SQL
```sql
SELECT MIN(cn1.name) AS first_company, MIN(cn2.name) AS second_company,
       MIN(mi_idx1.info) AS first_rating, MIN(mi_idx2.info) AS second_rating,
       MIN(t1.title) AS first_movie, MIN(t2.title) AS second_movie
FROM company_name cn1, company_name cn2, info_type it1, info_type it2,
     kind_type kt1, kind_type kt2, link_type lt,
     movie_companies mc1, movie_companies mc2,
     movie_info_idx mi_idx1, movie_info_idx mi_idx2,
     movie_link ml, title t1, title t2
WHERE cn1.country_code != '[us]'
  AND it1.info='rating' AND it2.info='rating'
  AND kt1.kind IN ('tv series','episode')
  AND kt2.kind IN ('tv series','episode')
  AND lt.link IN ('sequel','follows','followed by')
  AND mi_idx2.info < '3.5'
  AND t2.production_year BETWEEN 2000 AND 2010
  AND lt.id = ml.link_type_id
  AND t1.id = ml.movie_id AND t2.id = ml.linked_movie_id
  AND it1.id = mi_idx1.info_type_id AND t1.id = mi_idx1.movie_id
  AND kt1.id = t1.kind_id AND cn1.id = mc1.company_id AND t1.id = mc1.movie_id
  AND it2.id = mi_idx2.info_type_id AND t2.id = mi_idx2.movie_id
  AND kt2.id = t2.kind_id AND cn2.id = mc2.company_id AND t2.id = mc2.movie_id;
```

## Column Reference

### company_name.country_code (int16 dict, rows=234997)
Files: `company_name/country_code.bin`, `country_code.dict.{off,dat}`. Resolve `'[us]'` → `us_code`; build `cn1_ids = {i+1 : code[i] != us_code && code[i] != 0}` (NULLs excluded by SQL `!=`). Inequality → bitmap of ~most company ids. (Note: SQL `!= '[us]'` typically excludes NULL too in IMDB ratings; treat code 0 as not-matching.)

### company_name.name (varlen, rows=234997)
Files: `company_name/name.{off,dat}`. Projected for MIN(cn1.name) and MIN(cn2.name) (cn2 has no country filter).

### info_type.info (varlen, rows=113)
Files: `info_type/info.{off,dat}`. Resolve `'rating'` → single `rating_id` (used for both it1, it2).

### kind_type.kind (varlen, rows=7)
Files: `kind_type/kind.{off,dat}`. Resolve `'tv series'` and `'episode'` → `kt_ids` (2 ids). Both kt1 and kt2 use same set.

### link_type.link (varlen, rows=18)
Files: `link_type/link.{off,dat}`. Resolve `{sequel,follows,followed by}` → `lt_ids` (≤3).

### movie_link.movie_id, linked_movie_id, link_type_id (int32, rows=29997)
Files: `movie_link/{movie_id,linked_movie_id,link_type_id}.bin`. Sorted by movie_id.

### movie_companies.movie_id, company_id (int32, rows=2609129)
Files: `movie_companies/{movie_id,company_id}.bin`. Shared by mc1 and mc2 aliases.

### movie_info_idx.movie_id, info_type_id, info (rows=1380035)
Files: `movie_info_idx/{movie_id,info_type_id}.bin`, `info.{off,dat}`. Shared by mi_idx1/mi_idx2. `mi_idx2.info < '3.5'` is byte-wise lex compare.

### title.id, kind_id, production_year, title (rows=2528312)
Files: `title/id.bin` identity; `title/kind_id.bin`; `title/production_year.bin`; `title/title.{off,dat}`. t1: `kind_id ∈ kt_ids`. t2: `kind_id ∈ kt_ids AND production_year ∈ [2000,2010]`.

## Table Stats
| Table | Rows | Role | Sort |
|---|---|---|---|
| company_name | 234,997 | dim | id |
| info_type | 113 | dim | id |
| kind_type | 7 | dim | id |
| link_type | 18 | dim | id |
| title | 2,528,312 | dense-PK (t1+t2) | id |
| movie_companies | 2,609,129 | fact | movie_id |
| movie_info_idx | 1,380,035 | fact | movie_id |
| movie_link | 29,997 | self-join bridge | movie_id |

## Query Analysis
- Loosest variant: 2-set on kt, year window 2000–2010, lex<'3.5', `!= '[us]'` (broad).
- Driver: still ml-driven. ml is small. Use CSR `movie_link__link_type_id` for each `lt_id ∈ lt_ids` to get the few hundred candidate ml rows.
- Per ml row:
  1. `t1_id = movie_id[r]`, `t2_id = linked_movie_id[r]`.
  2. `title.kind_id[t1_id-1] ∈ kt_ids` AND `title.kind_id[t2_id-1] ∈ kt_ids`.
  3. `title.production_year[t2_id-1] ∈ [2000,2010]`.
  4. mc1: walk `movie_companies__movie_id`[t1_id] → require some mc.company_id with `cn1_ids` membership (non-us non-null). Capture `cn1.name`.
  5. mc2: walk `movie_companies__movie_id`[t2_id] → capture `cn2.name` from any row.
  6. mi_idx1: walk `movie_info_idx__movie_id`[t1_id] → require `info_type_id==rating_id`; capture for MIN.
  7. mi_idx2: walk `movie_info_idx__movie_id`[t2_id] → require `info_type_id==rating_id ∧ info < '3.5'` (lex); capture for MIN.
- Selectivities: `!= '[us]'` is broad (~half company ids); kt 2-set wider; year 10-year window wider; lex < '3.5' covers ratings 0.0–3.4 (string-compare).
- MIN aggregation: six running minima.
- LIKE notes: none. `mi_idx2.info < '3.5'` is byte-wise lex compare, NOT numeric. For rating strings of form `"X.Y"` (length 3-4), lex < `"3.5"` matches values starting with `"0"`–`"2"` fully, plus `"3.0"`–`"3.4"` (lex first byte=='3', second=='.', third<'5'). Strings like `"10.0"` would lex-compare as < `"3.5"` (first byte '1' < '3'); IMDB rating data does not contain such values.

## Indexes
- `movie_link__movie_id` (offsets_only): `_idx/movie_link__movie_id__offsets.bin` (int32, 2528314). Used if iterating from t1 side.
- `movie_link__link_type_id` (CSR): `_idx/movie_link__link_type_id__offsets.bin` (int32, 20), `_idx/movie_link__link_type_id__rowids.bin` (int32, 29997).
  ```cpp
  for (int32_t lt_id : lt_ids) {
      int32_t lo = mll_off[lt_id], hi = mll_off[lt_id+1];
      for (int32_t k=lo; k<hi; ++k) { int32_t r = mll_row[k]; /* ml row r */ }
  }
  ```
- `title__kind_id` (CSR): `_idx/title__kind_id__offsets.bin` (int32, 9), `_idx/title__kind_id__rowids.bin` (int32, 2528312). Useful to pre-build kt-title bitmap from `kt_ids` (2 ids).
- `movie_companies__movie_id` (offsets_only): used twice.
- `movie_info_idx__movie_id` (offsets_only): used twice.
- t2 → title is direct dereference at row `t2_id-1` (dense PK); no `movie_link__linked_movie_id` CSR used.
- No invented indexes; country_code dict resolved at runtime; no hardcoded ids.
