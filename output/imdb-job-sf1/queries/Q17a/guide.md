## SQL

```sql
SELECT MIN(n.name) AS member_in_charnamed_american_movie,
       MIN(n.name) AS a1
FROM cast_info AS ci, company_name AS cn, keyword AS k,
     movie_companies AS mc, movie_keyword AS mk, name AS n, title AS t
WHERE cn.country_code = '[us]'
  AND k.keyword = 'character-name-in-title'
  AND n.name LIKE 'B%'
  AND n.id = ci.person_id
  AND ci.movie_id = t.id
  AND t.id = mk.movie_id AND mk.keyword_id = k.id
  AND t.id = mc.movie_id AND mc.company_id = cn.id;
```

## Column Reference

### company_name.country_code (int16 dict)
Files: `company_name/country_code.bin` (int16, 234997), `.dict.off`, `.dict.dat`. Resolve `us_code` at runtime.

### keyword.keyword (varlen)
Files: `keyword/keyword.off`, `keyword/keyword.dat`. Resolve `k_id` of 'character-name-in-title'.

### name.id, name.name (int32 / varlen)
Files: `name/id.bin` (identity 1..4167491), `name/name.off`, `name/name.dat`. LIKE `'B%'` is prefix match.
```cpp
auto n_name_off = read_vec<int64_t>(store + "/name/name.off");
std::string n_name_dat = read_file(store + "/name/name.dat");
auto n_name_ok = [&](int32_t i){  // i is 0-based row index (id = i+1)
    int64_t a=n_name_off[i], b=n_name_off[i+1];
    return (b-a) >= 1 && n_name_dat[a] == 'B';
};
```
Build a set of candidate person_ids (i+1 where n_name_ok).

### title.title (varlen)
Files: `title/title.off`, `title/title.dat` (2528312). No filter on title in Q17a.

### movie_keyword.keyword_id, .movie_id (int32)
Files: `movie_keyword/keyword_id.bin`, `movie_keyword/movie_id.bin` (4523930; sorted by movie_id). CSR on keyword_id.

### movie_companies.company_id, .movie_id (int32)
Files: `movie_companies/company_id.bin`, `movie_companies/movie_id.bin` (2609129).

### cast_info.movie_id, .person_id (int32)
Files: `cast_info/movie_id.bin`, `cast_info/person_id.bin` (36244344; sorted by movie_id). offsets_only on movie_id.

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| company_name | 234,997 | id | dense-PK; country_code int16 dict |
| keyword | 134,170 | id | dense-PK |
| name | 4,167,491 | id | dense-PK |
| title | 2,528,312 | id | dense-PK |
| cast_info | 36,244,344 | movie_id | offsets_only on movie_id |
| movie_companies | 2,609,129 | movie_id | offsets_only on movie_id |
| movie_keyword | 4,523,930 | movie_id | CSR on keyword_id |

## Query Analysis

Join graph:
```
k --keyword_id-- mk --movie_id-- t --id-- mc --company_id-- cn
                                  t --id-- ci --person_id-- n
```

MIN aggregates: both outputs are `MIN(n.name)` (varlen). Only one MIN needed effectively.

Driver: keyword-driven via CSR `movie_keyword__keyword_id`.
1. Resolve `us_code`, `k_id`.
2. Pre-build bitset of names with prefix `'B'` (~hundreds of thousands of names).
3. CSR `movie_keyword__keyword_id[k_id]` → mk rows; `mv = mk.movie_id`.
4. For each unique mv:
   - Probe `movie_companies__movie_id[mv]`; require any mc with `cn_cc[company_id-1]==us_code`.
   - Probe `cast_info__movie_id[mv]`; for each ci row r, check `b_prefix[ci.person_id[r]-1]`. If yes, update `MIN(n.name)`.

Selectivities:
- `n.name LIKE 'B%'` → roughly 7-8% of 4.17M names (~300K).
- `k.keyword='character-name-in-title'` → small mk subset.
- `cn '[us]'` → ~30% of companies.
- ci rows per movie can be tens.

LIKE notes: `'B%'` → 1-byte prefix check; no `memmem` needed. Build a per-name boolean array `b_prefix[N]` once.

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
