## SQL

```sql
SELECT MIN(n.name) AS member_in_charnamed_movie,
       MIN(n.name) AS a1
FROM cast_info AS ci, company_name AS cn, keyword AS k,
     movie_companies AS mc, movie_keyword AS mk, name AS n, title AS t
WHERE k.keyword = 'character-name-in-title'
  AND n.name LIKE 'X%'
  AND n.id = ci.person_id
  AND ci.movie_id = t.id
  AND t.id = mk.movie_id AND mk.keyword_id = k.id
  AND t.id = mc.movie_id AND mc.company_id = cn.id;
```

## Column Reference

### company_name (joined, no filter)
No predicate on `cn.country_code`; cn is joined only for `mc.company_id = cn.id`. Existence trivially satisfied by mc rows.

### keyword.keyword (varlen)
Files: `keyword/keyword.off`, `keyword/keyword.dat`. Resolve `k_id` of 'character-name-in-title'.

### name.id, name.name (int32 / varlen)
Files: `name/id.bin` (identity), `name/name.off`, `name/name.dat`. LIKE `'X%'` → first byte 'X'.
```cpp
auto n_name_off = read_vec<int64_t>(store + "/name/name.off");
std::string n_name_dat = read_file(store + "/name/name.dat");
auto n_name_ok = [&](int32_t i){
    int64_t a=n_name_off[i], b=n_name_off[i+1];
    return (b-a) >= 1 && n_name_dat[a] == 'X';
};
```

### title.title (varlen)
Files: `title/title.off`, `title/title.dat`. No filter on title.

### movie_keyword.keyword_id, .movie_id (int32)
Files: `movie_keyword/keyword_id.bin`, `movie_keyword/movie_id.bin`. CSR on keyword_id.

### movie_companies.movie_id (int32)
Files: `movie_companies/movie_id.bin` (2609129). Existence-only via offsets_only.

### cast_info.movie_id, .person_id (int32)
Files: `cast_info/movie_id.bin`, `cast_info/person_id.bin` (36244344). offsets_only on movie_id.

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| company_name | 234,997 | id | dense-PK |
| keyword | 134,170 | id | dense-PK |
| name | 4,167,491 | id | dense-PK |
| title | 2,528,312 | id | dense-PK |
| cast_info | 36,244,344 | movie_id | offsets_only |
| movie_companies | 2,609,129 | movie_id | offsets_only |
| movie_keyword | 4,523,930 | movie_id | CSR on keyword_id |

## Query Analysis

Same shape as Q17b with prefix `'X%'`. Result count is even smaller (X-names are rarer than Z-names).

MIN: `MIN(n.name)`.

Driver: keyword-driven CSR.
1. Resolve `k_id`.
2. Build `x_prefix[N]` boolean array.
3. CSR `movie_keyword__keyword_id[k_id]` → mk rows; `mv = mk.movie_id`.
4. For each unique mv:
   - Probe `movie_companies__movie_id[mv]`; require non-empty range.
   - Probe `cast_info__movie_id[mv]`; for each ci row check `x_prefix[person_id-1]`; if yes update `MIN(n.name)`.

Selectivities:
- `n.name LIKE 'X%'` → very small (~0.1% of names).
- character-name-in-title keyword → small mk slice.

LIKE notes: 1-byte prefix check.

## Indexes

### movie_keyword__keyword_id (CSR)
Files: `_idx/movie_keyword__keyword_id__offsets.bin` (int32, 134172), `_idx/movie_keyword__keyword_id__rowids.bin` (int32, 4523930).
```cpp
auto mk_k_off = read_vec<int32_t>(store + "/_idx/movie_keyword__keyword_id__offsets.bin");
auto mk_k_row = read_vec<int32_t>(store + "/_idx/movie_keyword__keyword_id__rowids.bin");
int32_t lo = mk_k_off[k_id], hi = mk_k_off[k_id+1];
for (int32_t k=lo; k<hi; ++k) { int32_t r = mk_k_row[k]; }
```

### movie_companies__movie_id (offsets_only)
File: `_idx/movie_companies__movie_id__offsets.bin` (int32, 2528314).

### cast_info__movie_id (offsets_only)
File: `_idx/cast_info__movie_id__offsets.bin` (int32, 2528314).
