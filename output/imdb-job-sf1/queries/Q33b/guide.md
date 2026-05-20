# Q33b Guide

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
WHERE cn1.country_code = '[nl]'
  AND it1.info='rating' AND it2.info='rating'
  AND kt1.kind IN ('tv series') AND kt2.kind IN ('tv series')
  AND lt.link LIKE '%follow%'
  AND mi_idx2.info < '3.0' AND t2.production_year = 2007
  AND lt.id = ml.link_type_id
  AND t1.id = ml.movie_id AND t2.id = ml.linked_movie_id
  AND it1.id = mi_idx1.info_type_id AND t1.id = mi_idx1.movie_id
  AND kt1.id = t1.kind_id AND cn1.id = mc1.company_id AND t1.id = mc1.movie_id
  AND it2.id = mi_idx2.info_type_id AND t2.id = mi_idx2.movie_id
  AND kt2.id = t2.kind_id AND cn2.id = mc2.company_id AND t2.id = mc2.movie_id;
```

## Column Reference

### company_name.country_code (int16 dict, rows=234997)
Files: `company_name/country_code.bin`, `country_code.dict.{off,dat}`. Resolve `'[nl]'` → `nl_code`; `cn1_ids = {i+1 : code[i]==nl_code}`.

### company_name.name (varlen, rows=234997)
Files: `company_name/name.{off,dat}`. Projected via MIN for cn1 and cn2 aliases.

### info_type.info (varlen, rows=113)
Files: `info_type/info.{off,dat}`. Resolve `'rating'` → `rating_id`.

### kind_type.kind (varlen, rows=7)
Files: `kind_type/kind.{off,dat}`. Resolve `'tv series'` → `tv_kind_id`.

### link_type.link (varlen, rows=18)
Files: `link_type/link.{off,dat}`. Filter LIKE `%follow%` via memmem on per-row slice (length ≥6). Collect `lt_ids` (likely 2-3 ids: `follows`, `followed by`). Projected via MIN as well — read text for surviving link ids.

### movie_link.movie_id, linked_movie_id, link_type_id (int32, rows=29997)
Files: `movie_link/{movie_id,linked_movie_id,link_type_id}.bin`. Sorted by movie_id.

### movie_companies.movie_id, company_id (int32, rows=2609129)
Files: `movie_companies/{movie_id,company_id}.bin`. Shared by mc1/mc2.

### movie_info_idx.movie_id, info_type_id, info (rows=1380035)
Files: `movie_info_idx/{movie_id,info_type_id}.bin`, `info.{off,dat}`. Shared by mi_idx1/mi_idx2. mi_idx2 needs lex compare `info < '3.0'`.

### title.id, kind_id, production_year, title (rows=2528312)
Files: `title/id.bin` identity; `title/kind_id.bin`; `title/production_year.bin`; `title/title.{off,dat}`. t2 filter: `kind_id==tv_kind_id AND production_year==2007`. t1 filter: `kind_id==tv_kind_id`.

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
- Same shape as Q33a but stricter: `[nl]` country, year=2007 exact, LIKE `%follow%` on link.
- Driver: ml is small (29,997 rows). Resolve `lt_ids` via link_type scan + memmem. Then scan ml (or drive via CSR `movie_link__link_type_id` for each lt_id).
- Per ml row with `link_type_id ∈ lt_ids`:
  1. `t1_id = movie_id[r]`, `t2_id = linked_movie_id[r]`.
  2. `title.kind_id[t1_id-1] == tv_kind_id` AND `title.kind_id[t2_id-1] == tv_kind_id`.
  3. `title.production_year[t2_id-1] == 2007`.
  4. mc1: `movie_companies__movie_id[t1_id..]` → find mc row with `company_id ∈ cn1_ids` (nl). Capture `cn1.name`.
  5. mc2: `movie_companies__movie_id[t2_id..]` → capture `cn2.name` (no country filter).
  6. mi_idx1: `movie_info_idx__movie_id[t1_id..]` → require `info_type_id==rating_id`; capture for MIN.
  7. mi_idx2: same range for `t2_id`; require `info_type_id==rating_id ∧ info<'3.0'` (lex); capture for MIN.
- Selectivities: nl-companies few; year=2007 strict; LIKE `%follow%` matches 2-3 of 18 link types.
- MIN aggregation: six running minima.
- LIKE notes: `lt.link LIKE '%follow%'` is substring; resolve once over only 18 rows (cheap). `mi_idx2.info < '3.0'` is byte-wise lex compare (NOT numeric); on rating strings of form `"X.Y"` (length ≤4), lex < `"3.0"` ≡ first byte `<'3'` OR (first byte=='3' AND second byte<'.') etc.

## Indexes
- `movie_link__movie_id` (offsets_only): `_idx/movie_link__movie_id__offsets.bin` (int32, 2528314).
- `movie_link__link_type_id` (CSR): `_idx/movie_link__link_type_id__offsets.bin` (int32, 20), `_idx/movie_link__link_type_id__rowids.bin` (int32, 29997).
  ```cpp
  int32_t lo = mll_off[lt_id], hi = mll_off[lt_id+1];
  for (int32_t k=lo; k<hi; ++k) { int32_t r = mll_row[k]; }
  ```
- `title__kind_id` (CSR): `_idx/title__kind_id__offsets.bin` (int32, 9), `_idx/title__kind_id__rowids.bin` (int32, 2528312). Optional pre-build tv-title bitmap.
- `movie_companies__movie_id` (offsets_only): used for both t1 and t2 lookups.
- `movie_info_idx__movie_id` (offsets_only): used for both mi_idx1 and mi_idx2 lookups.
- t2 dereferences title directly at row `t2_id-1`; no `movie_link__linked_movie_id` CSR needed (drive is t1→ml→t2).
- No invented indexes; country_code dict resolved at runtime.
