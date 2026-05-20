# Q2b Guide

## SQL
```sql
SELECT MIN(t.title) AS movie_title
FROM company_name AS cn,
     keyword AS k,
     movie_companies AS mc,
     movie_keyword AS mk,
     title AS t
WHERE cn.country_code ='[nl]'
  AND k.keyword ='character-name-in-title'
  AND cn.id = mc.company_id
  AND mc.movie_id = t.id
  AND t.id = mk.movie_id
  AND mk.keyword_id = k.id
  AND mc.movie_id = mk.movie_id;
```

## Column Reference

### company_name.id (PK, int32_t)
- File: `company_name/id.bin` (rows = 234997); dense identity.
- This query's use: join with `mc.company_id`.

### company_name.country_code (int16_t, dict-encoded)
- Files: `company_name/country_code.bin`, `company_name/country_code.dict.off`, `company_name/country_code.dict.dat` (rows = 234997). Code 0 = NULL.
- This query's use: resolve `'[nl]'` at runtime, then test `country_code_bin[i] == target_code`.
- Dict-resolution snippet:
```cpp
auto off = read_vec<int64_t>(store + "/company_name/country_code.dict.off");
std::string dat = read_file(store + "/company_name/country_code.dict.dat");
int16_t target_code = 0;
for (size_t i = 0; i + 1 < off.size(); ++i) {
    std::string_view s(dat.data() + off[i], off[i+1] - off[i]);
    if (s == "[nl]") { target_code = (int16_t)(i + 1); break; }
}
```

### keyword.id (PK, int32_t)
- File: `keyword/id.bin` (rows = 134170); dense identity.

### keyword.keyword (varlen)
- Files: `keyword/keyword.off`, `keyword/keyword.dat` (rows = 134170)
- This query's use: scan to resolve `'character-name-in-title'` → `target_k_id`.

### movie_companies.movie_id (FK, int32_t)
- File: `movie_companies/movie_id.bin` (rows = 2609129); FK-sorted by movie_id.
- This query's use: join with `t.id` via `movie_companies__movie_id` offsets_only.

### movie_companies.company_id (FK, int32_t)
- File: `movie_companies/company_id.bin` (rows = 2609129)
- This query's use: `company_id_bin[r]` must satisfy `cn_pass[cid]` (precomputed bitmap of cn ids with country_code == target_code).

### movie_keyword.movie_id (FK, int32_t)
- File: `movie_keyword/movie_id.bin` (rows = 4523930); FK-sorted.

### movie_keyword.keyword_id (FK, int32_t)
- File: `movie_keyword/keyword_id.bin` (rows = 4523930)
- This query's use: `keyword_id_bin[r] == target_k_id`. Use `movie_keyword__keyword_id` CSR to fetch only matching rows.

### title.id (PK, int32_t)
- File: `title/id.bin` (rows = 2528312); dense identity.

### title.title (varlen)
- Files: `title/title.off`, `title/title.dat` (rows = 2528312)
- This query's use: projected `MIN(t.title)`.

## Table Stats
| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| company_name | 234,997 | dimension | id | 100000 |
| keyword | 134,170 | dimension | id | 100000 |
| title | 2,528,312 | dimension/fact | id | 100000 |
| movie_companies | 2,609,129 | fact | movie_id | 100000 |
| movie_keyword | 4,523,930 | fact | movie_id | 100000 |

## Query Analysis
- Join graph: `cn ── mc.company_id`, `k ── mk.keyword_id`, `mc.movie_id = mk.movie_id = t.id`.
- Driver: enumerate mk rows for `target_k_id` via `movie_keyword__keyword_id` CSR (very small set). Collect distinct `movie_id`s. For each candidate movie, use `movie_companies__movie_id` offsets_only to fetch mc rows; accept if any mc has `cn_pass[company_id]==true`. Then read `t.title`.
- Filter selectivities: keyword equality is 1/134170; via CSR mk rows are small. `[nl]` is a rare country code → very few cn rows pass; cn_pass bitmap has small popcount, so the company_id check is highly selective.
- Aggregation: `MIN(t.title)` → single result row.
- LIKE patterns: none (pure equality).
- Output projection: `t.title` only — read varlen for surviving title rows.

## Indexes
- `movie_keyword__keyword_id` (CSR): `_idx/movie_keyword__keyword_id__offsets.bin` + `_idx/movie_keyword__keyword_id__rowids.bin`. Usage:
```cpp
int32_t lo = off[target_k_id]; int32_t hi = off[target_k_id + 1];
for (int32_t k_pos = lo; k_pos < hi; ++k_pos) {
    int32_t mk_row = rowids[k_pos];
    int32_t mv = movie_id_bin[mk_row];
    /* probe mc and t for mv */
}
```
- `movie_companies__movie_id` (offsets_only): `_idx/movie_companies__movie_id__offsets.bin`. Usage:
```cpp
int32_t lo = off[mv]; int32_t hi = off[mv+1];
for (int32_t r = lo; r < hi; ++r) { /* mc row r */ }
```

## Rules followed
- `[nl]` code resolved at runtime via dict scan.
- `target_k_id` resolved by scanning `keyword/keyword.dat`.
- No invented text indexes.
