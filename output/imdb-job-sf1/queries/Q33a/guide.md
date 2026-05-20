# Q33a Guide

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
WHERE cn1.country_code = '[us]'
  AND it1.info='rating' AND it2.info='rating'
  AND kt1.kind IN ('tv series') AND kt2.kind IN ('tv series')
  AND lt.link IN ('sequel','follows','followed by')
  AND mi_idx2.info < '3.0'
  AND t2.production_year BETWEEN 2005 AND 2008
  AND lt.id = ml.link_type_id
  AND t1.id = ml.movie_id AND t2.id = ml.linked_movie_id
  AND it1.id = mi_idx1.info_type_id AND t1.id = mi_idx1.movie_id
  AND kt1.id = t1.kind_id AND cn1.id = mc1.company_id AND t1.id = mc1.movie_id
  AND it2.id = mi_idx2.info_type_id AND t2.id = mi_idx2.movie_id
  AND kt2.id = t2.kind_id AND cn2.id = mc2.company_id AND t2.id = mc2.movie_id;
```

## Column Reference

### company_name.country_code (int16 dict, rows=234997)
Files: `company_name/country_code.bin`, `country_code.dict.{off,dat}`. Resolve `'[us]'` → `us_code` at runtime; build `cn1_ids = {i+1 : code[i]==us_code}`. (cn2 has no country_code filter in Q33a → all company ids.)

### company_name.name (varlen, rows=234997)
Files: `company_name/name.{off,dat}`. Projected for MIN(cn1.name) and MIN(cn2.name). Same file used for both aliases; row index = `company_id - 1`.

### info_type.info (varlen, rows=113)
Files: `info_type/info.{off,dat}`. Resolve `'rating'` → single `rating_id`; both `it1_id` and `it2_id` equal that id.

### kind_type.kind (varlen, rows=7)
Files: `kind_type/kind.{off,dat}`. Resolve `'tv series'` → `tv_kind_id`. Both `kt1_id` and `kt2_id` equal that id.

### link_type.link (varlen, rows=18)
Files: `link_type/link.{off,dat}`. Resolve IN-set `{sequel,follows,followed by}` → `lt_ids` (≤3).

### movie_link.movie_id, linked_movie_id, link_type_id (int32, rows=29997)
Files: `movie_link/{movie_id,linked_movie_id,link_type_id}.bin`. Sorted by movie_id; offsets_only `movie_link__movie_id` for t1 side. Self-join bridge.

### movie_companies.movie_id, company_id (int32, rows=2609129)
Files: `movie_companies/{movie_id,company_id}.bin`. Shared by mc1 and mc2 aliases. Use offsets_only `movie_companies__movie_id` for per-movie lookup.

### movie_info_idx.movie_id, info_type_id, info (rows=1380035)
Files: `movie_info_idx/{movie_id,info_type_id}.bin`, `info.{off,dat}`. Shared by mi_idx1 and mi_idx2 aliases. Use offsets_only `movie_info_idx__movie_id` for per-movie lookup with `info_type_id == rating_id` filter. `mi_idx2.info < '3.0'` is lexicographic string-compare (NOT numeric): memcmp the slice against `"3.0"` with min(len,3) tiebreak then length tiebreak; on this column values are like `"1.7"`, `"2.8"`, `"3.1"`, etc., so lex compare matches numeric intent for one-digit prefixes.

### title.id, kind_id, production_year, title (rows=2528312)
Files: `title/id.bin` identity; `title/kind_id.bin`; `title/production_year.bin` (nullable, `INT32_MIN`); `title/title.{off,dat}`. Both t1 and t2 share these files. t1: filter `kind_id==tv_kind_id`. t2: filter `kind_id==tv_kind_id ∧ production_year BETWEEN 2005 AND 2008`.

## Table Stats
| Table | Rows | Role | Sort |
|---|---|---|---|
| company_name | 234,997 | dim | id |
| info_type | 113 | dim | id |
| kind_type | 7 | dim | id |
| link_type | 18 | dim | id |
| title | 2,528,312 | dense-PK (t1+t2) | id |
| movie_companies | 2,609,129 | fact (mc1+mc2) | movie_id |
| movie_info_idx | 1,380,035 | fact (mi_idx1+mi_idx2) | movie_id |
| movie_link | 29,997 | self-join bridge | movie_id |

## Query Analysis
- Two parallel fact-arms over the same physical tables, connected by `ml`:
  ```
  cn1 -- mc1 -- t1 -- mi_idx1 -- it1
                kt1 -- t1
                            ml.movie_id = t1.id
                            ml.linked_movie_id = t2.id
  cn2 -- mc2 -- t2 -- mi_idx2 -- it2
                kt2 -- t2
  ```
- Driver: `movie_link` is tiny (29,997 rows). Scan ml linearly OR drive from `lt_ids` via CSR `movie_link__link_type_id`. The latter restricts to ~3 of 18 link types; cheap.
- For each ml row in `link_type_id ∈ lt_ids`:
  1. `t1_id = ml.movie_id[r]`; `t2_id = ml.linked_movie_id[r]`.
  2. t1 checks: `title.kind_id[t1_id-1] == tv_kind_id`.
  3. t2 checks: `title.kind_id[t2_id-1] == tv_kind_id` AND `production_year[t2_id-1] ∈ [2005,2008]`.
  4. mc1: walk `movie_companies__movie_id`[t1_id] → for some mc row, `company_id ∈ cn1_ids` (i.e., country_code==us_code). Capture `cn1.name` at `company_id-1` for MIN.
  5. mc2: walk `movie_companies__movie_id`[t2_id] → for some mc row, capture `cn2.name` at `company_id-1` for MIN (no country filter).
  6. mi_idx1: walk `movie_info_idx__movie_id`[t1_id] → require `info_type_id==rating_id`; capture `info` for MIN.
  7. mi_idx2: walk `movie_info_idx__movie_id`[t2_id] → require `info_type_id==rating_id ∧ info < '3.0'` (lex); capture `info` for MIN.
- mi_idx has TWO logical roles. Same storage files; plan applies separate offsets-lookups per side.
- Selectivities: `lt_ids` cuts ml to ~3/18; kt tv_kind cuts ~75% of titles on each side; year window narrow on t2; `< '3.0'` rare ratings.
- MIN aggregation: six running minima.
- LIKE: none in Q33a. Use string-lex compare for `mi_idx2.info < '3.0'` — this is byte-wise, e.g. `"10.0"` < `"3.0"` lexicographically (string-compare, not numeric). Per IMDB rating data, values are normally `"X.Y"` (4 chars max), so lex < `"3.0"` reliably matches intended `{0.0..2.9}` range.

## Indexes
- `movie_link__movie_id` (offsets_only): `_idx/movie_link__movie_id__offsets.bin` (int32, 2528314). Used for t1 side; not needed if scanning ml linearly.
- `movie_link__link_type_id` (CSR): `_idx/movie_link__link_type_id__offsets.bin` (int32, 20), `_idx/movie_link__link_type_id__rowids.bin` (int32, 29997). Drive from `lt_ids`.
  ```cpp
  int32_t lo = mll_off[lt_id], hi = mll_off[lt_id+1];
  for (int32_t k=lo; k<hi; ++k) { int32_t r = mll_row[k]; /* ml row r */ }
  ```
- `title__kind_id` (CSR): `_idx/title__kind_id__offsets.bin` (int32, 9), `_idx/title__kind_id__rowids.bin` (int32, 2528312). Optional pre-prune: build `tv_title_set` once from `tv_kind_id`, then test `t1_id`/`t2_id` membership (bitmap of size 2528312 cheap).
- `movie_companies__movie_id` (offsets_only): `_idx/movie_companies__movie_id__offsets.bin` (int32, 2528314). Used twice (t1 and t2 lookups).
- `movie_info_idx__movie_id` (offsets_only): `_idx/movie_info_idx__movie_id__offsets.bin` (int32, 2528314). Used twice.
- t2-side dereference into title goes directly (dense PK); no need for `movie_link__linked_movie_id` CSR here.
- No invented indexes; country_code dict resolved at runtime; no hardcoded ids.
