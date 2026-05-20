# Q2a Guide

## SQL
```sql
SELECT MIN(t.title) AS movie_title
FROM company_name AS cn,
     keyword AS k,
     movie_companies AS mc,
     movie_keyword AS mk,
     title AS t
WHERE cn.country_code ='[de]'
  AND k.keyword ='character-name-in-title'
  AND cn.id = mc.company_id
  AND mc.movie_id = t.id
  AND t.id = mk.movie_id
  AND mk.keyword_id = k.id
  AND mc.movie_id = mk.movie_id;
```

## Column Reference

### company_name.id (PK, int32_t)
- File: `company_name/id.bin` (rows = 234997); dense identity, row i ↔ id (i+1).
- This query's use: join with `mc.company_id`.

### company_name.country_code (int16_t, dict-encoded)
- Files: `company_name/country_code.bin`, `company_name/country_code.dict.off`, `company_name/country_code.dict.dat` (rows = 234997)
- Code 0 = NULL; code c references dict entry c-1.
- This query's use: resolve `'[de]'` at runtime, then test `country_code_bin[i] == target_code`.
- Dict-resolution snippet (verbatim from shared context, adapted):
```cpp
auto off = read_vec<int64_t>(store + "/company_name/country_code.dict.off");
std::string dat = read_file(store + "/company_name/country_code.dict.dat");
int16_t target_code = 0;
for (size_t i = 0; i + 1 < off.size(); ++i) {
    std::string_view s(dat.data() + off[i], off[i+1] - off[i]);
    if (s == "[de]") { target_code = (int16_t)(i + 1); break; }
}
```

### keyword.id (PK, int32_t)
- File: `keyword/id.bin` (rows = 134170); dense identity.
- This query's use: join with `mk.keyword_id`.

### keyword.keyword (varlen)
- Files: `keyword/keyword.off`, `keyword/keyword.dat` (rows = 134170)
- This query's use: scan to resolve literal `'character-name-in-title'` → `target_k_id` (id↔row-index identity).

### movie_companies.movie_id (FK to title, int32_t)
- File: `movie_companies/movie_id.bin` (rows = 2609129); FK-sorted by movie_id.
- This query's use: join with `t.id` via `movie_companies__movie_id` offsets_only.

### movie_companies.company_id (FK to company_name, int32_t)
- File: `movie_companies/company_id.bin` (rows = 2609129)
- This query's use: `cn.id = mc.company_id` → test `company_id_bin[r]` corresponds to a `cn` row whose `country_code == target_code`. Implementation: precompute boolean `cn_pass[cid]` for ids 1..234997.

### movie_keyword.movie_id (FK to title, int32_t)
- File: `movie_keyword/movie_id.bin` (rows = 4523930); FK-sorted by movie_id.
- This query's use: join with `t.id` via `movie_keyword__movie_id` offsets_only.

### movie_keyword.keyword_id (FK to keyword, int32_t)
- File: `movie_keyword/keyword_id.bin` (rows = 4523930)
- This query's use: `keyword_id_bin[r] == target_k_id`. Alternative: use `movie_keyword__keyword_id` CSR to skip directly to the small list of mk rows for `target_k_id`.

### title.id (PK, int32_t)
- File: `title/id.bin` (rows = 2528312); dense identity.

### title.title (varlen)
- Files: `title/title.off`, `title/title.dat` (rows = 2528312)
- This query's use: projected `MIN(t.title)`; read for surviving rows only.

## Table Stats
| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| company_name | 234,997 | dimension | id | 100000 |
| keyword | 134,170 | dimension | id | 100000 |
| title | 2,528,312 | dimension/fact | id | 100000 |
| movie_companies | 2,609,129 | fact | movie_id | 100000 |
| movie_keyword | 4,523,930 | fact | movie_id | 100000 |

## Query Analysis
- Join graph: `cn ── mc.company_id`; `k ── mk.keyword_id`; `mc.movie_id = t.id = mk.movie_id`. Title is the central PK.
- Recommended driver: probe via `movie_keyword__keyword_id` CSR with `target_k_id` to get a small set of mk rowids; collect their `movie_id` set. For each candidate movie id, use `movie_companies__movie_id` offsets_only to fetch mc rows and test `cn_pass[mc.company_id]`. Read `t.title` for surviving movies.
- Filter selectivities: `k.keyword='character-name-in-title'` selects 1 of 134170 keywords; via CSR, mk rows fetched are tiny (thousands). `cn.country_code='[de]'` selects ~1% of company_name. Combined selectivity is small.
- Aggregation: `MIN(t.title)` → single output row.
- LIKE patterns: none in Q2a (pure equality).
- Output projection: `t.title` only — read from `title/title.off`+`.dat` for surviving title ids.

## Indexes
- `movie_keyword__keyword_id` (CSR): files `_idx/movie_keyword__keyword_id__offsets.bin` (int32, 134172) + `_idx/movie_keyword__keyword_id__rowids.bin` (int32, 4523930). Query-time:
```cpp
int32_t lo = off[target_k_id]; int32_t hi = off[target_k_id + 1];
for (int32_t k_pos = lo; k_pos < hi; ++k_pos) {
    int32_t mk_row = rowids[k_pos];
    int32_t mv = movie_id_bin[mk_row];
    /* probe mc and title for mv */
}
```
- `movie_companies__movie_id` (offsets_only): `_idx/movie_companies__movie_id__offsets.bin`. Usage:
```cpp
int32_t lo = off[mv]; int32_t hi = off[mv+1];
for (int32_t r = lo; r < hi; ++r) { /* mc row r */ }
```

## Rules followed
- `target_code` for `[de]` resolved at runtime via dict scan; never hardcoded.
- `target_k_id` resolved by scanning `keyword/keyword.dat`.
- Only indexes declared in storage_design.json used.
- No claim of an index on `keyword.keyword` text.
