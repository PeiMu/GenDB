## SQL

```sql
SELECT MIN(n.name) AS member_in_charnamed_movie
FROM cast_info AS ci, company_name AS cn, keyword AS k,
     movie_companies AS mc, movie_keyword AS mk, name AS n, title AS t
WHERE cn.country_code = '[us]'
  AND k.keyword = 'character-name-in-title'
  AND n.id = ci.person_id
  AND ci.movie_id = t.id
  AND t.id = mk.movie_id AND mk.keyword_id = k.id
  AND t.id = mc.movie_id AND mc.company_id = cn.id;
```

## Column Reference

### company_name.country_code (int16 dict)
Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`. Resolve `us_code` at runtime; never hardcode.

### keyword.keyword (varlen)
Files: `keyword/keyword.off`, `keyword/keyword.dat`. Resolve `k_id` of 'character-name-in-title'.

### name.id, name.name (int32 / varlen)
Files: `name/id.bin` (identity), `name/name.off`, `name/name.dat`. No LIKE filter in Q17e — every name is a candidate. For MIN(n.name) we just need to access n.name at qualifying person_ids.

### title.title (varlen)
Files: `title/title.off`, `title/title.dat`. No filter on title.

### movie_keyword.keyword_id, .movie_id (int32)
Files: `movie_keyword/keyword_id.bin`, `movie_keyword/movie_id.bin` (4523930). CSR on keyword_id.

### movie_companies.company_id, .movie_id (int32)
Files: `movie_companies/company_id.bin`, `movie_companies/movie_id.bin` (2609129).

### cast_info.movie_id, .person_id (int32)
Files: `cast_info/movie_id.bin`, `cast_info/person_id.bin` (36244344). offsets_only on movie_id.

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| company_name | 234,997 | id | dense-PK; country_code int16 dict |
| keyword | 134,170 | id | dense-PK |
| name | 4,167,491 | id | dense-PK |
| title | 2,528,312 | id | dense-PK |
| cast_info | 36,244,344 | movie_id | offsets_only |
| movie_companies | 2,609,129 | movie_id | offsets_only |
| movie_keyword | 4,523,930 | movie_id | CSR on keyword_id |

## Query Analysis

Same shape as Q17a/b/c/d but no LIKE on n.name. Country filter present.

MIN: `MIN(n.name)` (single output).

Driver: keyword-driven CSR.
1. Resolve `us_code`, `k_id`.
2. CSR `movie_keyword__keyword_id[k_id]` → mk rows; `mv = mk.movie_id`.
3. For each unique mv:
   - Probe `movie_companies__movie_id[mv]`; require some mc row with `cn_cc[company_id-1]==us_code`.
   - Probe `cast_info__movie_id[mv]`; for each ci row r get `pid = person_id[r]`; update `MIN(n.name)` using `name.name[pid-1]`.

Selectivities:
- No name filter → all persons in qualifying movies contribute.
- character-name-in-title → small mk slice.
- cn '[us]' → ~30% of mc rows.

LIKE notes: none.

Performance: lots of MIN updates per pid; can accelerate by tracking smallest n.name string seen so far and only comparing per pid (avoid re-comparing). For large pid lists pre-sort by n.name lex order — but since dense-PK is by id (not by name), do a per-row lex compare with running min.

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
