# Q3b Guide

## SQL
```sql
SELECT MIN(t.title) AS movie_title
FROM keyword AS k,
     movie_info AS mi,
     movie_keyword AS mk,
     title AS t
WHERE k.keyword LIKE '%sequel%'
  AND mi.info IN ('Bulgaria')
  AND t.production_year > 2010
  AND t.id = mi.movie_id
  AND t.id = mk.movie_id
  AND mk.movie_id = mi.movie_id
  AND k.id = mk.keyword_id;
```

## Column Reference

### keyword.id (PK, int32_t)
- File: `keyword/id.bin` (rows = 134170); dense identity.

### keyword.keyword (varlen)
- Files: `keyword/keyword.off`, `keyword/keyword.dat` (rows = 134170)
- This query's use: `LIKE '%sequel%'` → scan via `memmem`. Collect `seq_ids` (small set).

### movie_info.movie_id (FK to title, int32_t)
- File: `movie_info/movie_id.bin` (rows = 14835720); FK-sorted by movie_id.

### movie_info.info (varlen)
- Files: `movie_info/info.off`, `movie_info/info.dat` (rows = 14835720)
- This query's use: `mi.info = 'Bulgaria'` (IN with one literal). movie_info is FK-sorted by movie_id; scan its varlen column in row order of the FK-sorted child for each candidate movie range. Length prefilter: only check rows whose length == 8. No index on `movie_info.info`.

### movie_keyword.movie_id (FK to title, int32_t)
- File: `movie_keyword/movie_id.bin` (rows = 4523930); FK-sorted.

### movie_keyword.keyword_id (FK to keyword, int32_t)
- File: `movie_keyword/keyword_id.bin` (rows = 4523930)
- This query's use: equality membership in `seq_ids`. Use `movie_keyword__keyword_id` CSR over each kid in `seq_ids`.

### title.id (PK, int32_t)
- File: `title/id.bin` (rows = 2528312); dense identity.

### title.title (varlen)
- Files: `title/title.off`, `title/title.dat` (rows = 2528312)
- This query's use: projected `MIN(t.title)`.

### title.production_year (int32_t, nullable)
- File: `title/production_year.bin` (rows = 2528312); NULL = `INT32_MIN`.
- This query's use: `production_year > 2010`. C++: `y != INT32_MIN && y > 2010`.

## Table Stats
| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| keyword | 134,170 | dimension | id | 100000 |
| title | 2,528,312 | dimension/fact | id | 100000 |
| movie_keyword | 4,523,930 | fact | movie_id | 100000 |
| movie_info | 14,835,720 | fact | movie_id | 200000 |

## Query Analysis
- Join graph: `k ── mk.keyword_id`; `mk.movie_id = mi.movie_id = t.id` (star around title).
- Driver: collect `seq_ids` (keyword ids LIKE `%sequel%`). For each kid, walk `movie_keyword__keyword_id` CSR to extract candidate movie ids. Filter candidates by `t.production_year > 2010`. For each surviving movie, fetch `mi` range via `movie_info__movie_id` offsets_only and scan `info` for equality to `'Bulgaria'`. Existence semantics (break on first match).
- Filter selectivities: `%sequel%` → small keyword set; production_year > 2010 narrows ~10–12% of titles; `mi.info='Bulgaria'` is highly rare (likely only a handful of mi rows overall).
- Aggregation: `MIN(t.title)` → single result row.
- LIKE pattern: `%sequel%` on `keyword.keyword` — full varlen scan via `memmem`; no text index.
- IN pattern: degenerates to equality with one literal; still use a length prefilter (==8) and `std::memcmp`.
- Output projection: read `t.title` varlen only for surviving rows.

## Indexes
- `movie_keyword__keyword_id` (CSR): `_idx/movie_keyword__keyword_id__offsets.bin` + `_idx/movie_keyword__keyword_id__rowids.bin`. Usage:
```cpp
for (int32_t kid : seq_ids) {
    int32_t lo = off[kid]; int32_t hi = off[kid + 1];
    for (int32_t k_pos = lo; k_pos < hi; ++k_pos) {
        int32_t mk_row = rowids[k_pos];
        int32_t mv = movie_id_bin[mk_row];
        candidate_movies.insert(mv);
    }
}
```
- `movie_info__movie_id` (offsets_only): `_idx/movie_info__movie_id__offsets.bin`. Usage:
```cpp
int32_t lo = off[mv]; int32_t hi = off[mv+1];
for (int32_t r = lo; r < hi; ++r) {
    /* test mi.info via .off+.dat[r] for equality to 'Bulgaria' */
}
```

## Rules followed
- `%sequel%` evaluated by scanning `keyword.keyword` varlen; no fabricated text index.
- `mi.info` is varlen on FK-sorted child; scanned in row order within candidate movie ranges.
- Only declared offsets_only / CSR indexes used.
