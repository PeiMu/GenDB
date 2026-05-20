## SQL

```sql
SELECT MIN(n.name) AS member_in_charnamed_movie,
       MIN(n.name) AS a1
FROM cast_info AS ci, company_name AS cn, keyword AS k,
     movie_companies AS mc, movie_keyword AS mk, name AS n, title AS t
WHERE k.keyword = 'character-name-in-title'
  AND n.name LIKE 'Z%'
  AND n.id = ci.person_id
  AND ci.movie_id = t.id
  AND t.id = mk.movie_id AND mk.keyword_id = k.id
  AND t.id = mc.movie_id AND mc.company_id = cn.id;
```

## Column Reference

### company_name (joined, no filter)
Files: `company_name/id.bin` (identity). Q17b has NO `cn.country_code` filter; `cn` is joined only via `mc.company_id = cn.id`, so any mc row qualifies (cn.id existence is automatic given mc.company_id present).

### keyword.keyword (varlen)
Files: `keyword/keyword.off`, `keyword/keyword.dat`. Resolve `k_id` of 'character-name-in-title'.

### name.id, name.name (int32 / varlen)
Files: `name/id.bin` (identity), `name/name.off`, `name/name.dat`. LIKE `'Z%'` → first byte == 'Z'.
```cpp
auto n_name_off = read_vec<int64_t>(store + "/name/name.off");
std::string n_name_dat = read_file(store + "/name/name.dat");
auto n_name_ok = [&](int32_t i){
    int64_t a=n_name_off[i], b=n_name_off[i+1];
    return (b-a) >= 1 && n_name_dat[a] == 'Z';
};
```

### title.title (varlen)
Files: `title/title.off`, `title/title.dat` (2528312). No filter on t columns.

### movie_keyword.keyword_id, .movie_id (int32)
Files: `movie_keyword/keyword_id.bin`, `movie_keyword/movie_id.bin` (4523930). CSR on keyword_id.

### movie_companies.movie_id (int32)
Files: `movie_companies/movie_id.bin` (2609129). offsets_only on movie_id; only existence required.

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

Same graph as Q17a minus country_code filter. Looser overall.

MIN: `MIN(n.name)` (both columns alias same expression).

Driver: keyword-driven CSR.
1. Resolve `k_id`.
2. Pre-build bitset `z_prefix[N]` for n.name starting with 'Z' (very small — `Z` is rare, ~tens of thousands).
3. CSR `movie_keyword__keyword_id[k_id]` → mk rows; `mv = mk.movie_id`.
4. For each unique mv:
   - Probe `movie_companies__movie_id[mv]`; require range non-empty (existence; any mc row OK).
   - Probe `cast_info__movie_id[mv]`; for each ci row r check `z_prefix[ci.person_id[r]-1]`. If yes, update `MIN(n.name)`.

Selectivities:
- `n.name LIKE 'Z%'` → very small fraction (~1% of names).
- character-name-in-title → ~few thousand mk rows.
- No `cn.country_code` filter → more permissive than Q17a.

LIKE notes: `'Z%'` → 1-byte prefix.

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
