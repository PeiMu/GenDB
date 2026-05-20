## SQL

```sql
SELECT MIN(an.name) AS cool_actor_pseudonym,
       MIN(t.title) AS series_named_after_char
FROM aka_name AS an, cast_info AS ci, company_name AS cn, keyword AS k,
     movie_companies AS mc, movie_keyword AS mk, name AS n, title AS t
WHERE cn.country_code = '[us]'
  AND k.keyword = 'character-name-in-title'
  AND t.episode_nr >= 50
  AND t.episode_nr < 100
  AND an.person_id = n.id
  AND n.id = ci.person_id
  AND ci.movie_id = t.id
  AND t.id = mk.movie_id
  AND mk.keyword_id = k.id
  AND t.id = mc.movie_id
  AND mc.company_id = cn.id;
```

## Column Reference

### company_name.country_code (int16 dict)
Files: `company_name/country_code.bin` (int16, 234997), `.dict.off`, `.dict.dat`. Resolve `us_code` at runtime.

### keyword.keyword (varlen)
Files: `keyword/keyword.off`, `keyword/keyword.dat`. Resolve `k_id = id of 'character-name-in-title'` (single match).
```cpp
auto k_off = read_vec<int64_t>(store + "/keyword/keyword.off");
std::string k_dat = read_file(store + "/keyword/keyword.dat");
int32_t k_id = 0;
std::string_view kw = "character-name-in-title";
for (size_t i=0; i+1<k_off.size(); ++i) {
    std::string_view s(k_dat.data()+k_off[i], k_off[i+1]-k_off[i]);
    if (s == kw) { k_id = (int32_t)(i+1); break; }
}
```

### title.episode_nr, title.title (int32 nullable / varlen)
Files: `title/episode_nr.bin` (int32, 2528312, NULL=INT32_MIN), `title/title.{off,dat}`. CRITICAL nullability:
```cpp
auto t_ep = read_vec<int32_t>(store + "/title/episode_nr.bin");
auto t_ep_ok = [&](int32_t r){ return t_ep[r] != INT32_MIN && t_ep[r] >= 50 && t_ep[r] < 100; };
```

### movie_keyword.keyword_id, movie_keyword.movie_id (int32)
Files: `movie_keyword/keyword_id.bin`, `movie_keyword/movie_id.bin` (4523930; sorted by movie_id). Use CSR `movie_keyword__keyword_id` for k_id-driven enumeration.

### movie_companies.company_id, .movie_id (int32)
Files: `movie_companies/company_id.bin`, `movie_companies/movie_id.bin` (2609129). Probe by movie_id offsets_only.

### cast_info.movie_id, .person_id (int32)
Files: `cast_info/movie_id.bin`, `cast_info/person_id.bin` (36244344; sorted by movie_id). Probe by movie_id offsets_only for given mv.

### name.id, name.name (int32 / varlen)
Files: `name/id.bin` (identity), `name/name.{off,dat}` (4167491). MIN over varlen.

### aka_name.person_id, aka_name.name (int32 / varlen)
Files: `aka_name/person_id.bin`, `aka_name/name.{off,dat}` (901343; sorted by person_id). Use offsets_only on person_id.

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| company_name | 234,997 | id | dense-PK; country_code int16 dict |
| keyword | 134,170 | id | dense-PK |
| name | 4,167,491 | id | dense-PK |
| title | 2,528,312 | id | dense-PK |
| aka_name | 901,343 | person_id | offsets_only on person_id |
| cast_info | 36,244,344 | movie_id | offsets_only on movie_id; CSR on person_id |
| movie_companies | 2,609,129 | movie_id | offsets_only on movie_id |
| movie_keyword | 4,523,930 | movie_id | offsets_only; CSR on keyword_id |

## Query Analysis

Join graph (star around title):
```
k --keyword_id-- mk --movie_id-- t --id-- mc --company_id-- cn
                                  t --id-- ci --person_id-- n --id-- an
```

MIN: `MIN(an.name)`, `MIN(t.title)` (both varlen).

Driver: keyword-driven via CSR `movie_keyword__keyword_id`.
1. Resolve `us_code` (cn), `k_id` (character-name-in-title).
2. CSR `movie_keyword__keyword_id[k_id]` → enumerate mk rows; get `mv = mk.movie_id[r]`.
3. For each mv (likely deduped via small set), apply title filter: `t_ep[mv-1] != INT32_MIN && t_ep[mv-1] >= 50 && t_ep[mv-1] < 100`.
4. Probe `movie_companies__movie_id` for mv; require any mc row with `cn_cc[company_id-1]==us_code` (existence).
5. Probe `cast_info__movie_id` for mv; for each ci row r' get `pid = ci.person_id[r']`.
6. For each pid, use `aka_name__person_id` offsets_only to enumerate an rows; update `MIN(an.name)` over those entries. Update `MIN(t.title)`.

Selectivities:
- `k.keyword='character-name-in-title'` → small fraction of keywords (1 of 134K) but mk rows are non-trivial.
- `episode_nr ∈ [50,100)` → mostly NULL for non-episodes; filters heavily.
- cn '[us]' → ~30% of companies; ~30% of mc rows.
- cast_info per movie can be many.

LIKE notes: none in Q16a.

Note: `aka_name` row's "existence" is required because the SQL ties `an.person_id = n.id = ci.person_id`. If a person has no aka_name row, that person contributes nothing to MIN(an.name).

## Indexes

### movie_keyword__keyword_id (CSR)
Files: `_idx/movie_keyword__keyword_id__offsets.bin` (int32, 134172), `_idx/movie_keyword__keyword_id__rowids.bin` (int32, 4523930).
```cpp
auto mk_k_off = read_vec<int32_t>(store + "/_idx/movie_keyword__keyword_id__offsets.bin");
auto mk_k_row = read_vec<int32_t>(store + "/_idx/movie_keyword__keyword_id__rowids.bin");
int32_t lo = mk_k_off[k_id], hi = mk_k_off[k_id+1];
for (int32_t k=lo; k<hi; ++k) { int32_t r = mk_k_row[k]; /* mk row r */ }
```

### movie_companies__movie_id (offsets_only)
File: `_idx/movie_companies__movie_id__offsets.bin` (int32, 2528314).

### cast_info__movie_id (offsets_only)
File: `_idx/cast_info__movie_id__offsets.bin` (int32, 2528314).

### aka_name__person_id (offsets_only)
File: `_idx/aka_name__person_id__offsets.bin` (int32, 4167493). aka_name sorted by person_id.
```cpp
auto an_pid = read_vec<int32_t>(store + "/_idx/aka_name__person_id__offsets.bin");
int32_t lo = an_pid[pid], hi = an_pid[pid+1];
for (int32_t r=lo; r<hi; ++r) { /* an row r */ }
```
