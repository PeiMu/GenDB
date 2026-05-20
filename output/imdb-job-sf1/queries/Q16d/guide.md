## SQL

```sql
SELECT MIN(an.name) AS cool_actor_pseudonym,
       MIN(t.title) AS series_named_after_char
FROM aka_name AS an, cast_info AS ci, company_name AS cn, keyword AS k,
     movie_companies AS mc, movie_keyword AS mk, name AS n, title AS t
WHERE cn.country_code = '[us]'
  AND k.keyword = 'character-name-in-title'
  AND t.episode_nr >= 5
  AND t.episode_nr < 100
  AND an.person_id = n.id
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

### title.episode_nr, title.title (int32 nullable / varlen)
Files: `title/episode_nr.bin`, `title/title.{off,dat}` (2528312). NULL=INT32_MIN.
```cpp
auto t_ep = read_vec<int32_t>(store + "/title/episode_nr.bin");
auto t_ep_ok = [&](int32_t r){ return t_ep[r] != INT32_MIN && t_ep[r] >= 5 && t_ep[r] < 100; };
```

### movie_keyword.keyword_id, .movie_id (int32)
Files: `movie_keyword/keyword_id.bin`, `movie_keyword/movie_id.bin` (4523930; sorted by movie_id). CSR on keyword_id.

### movie_companies.company_id, .movie_id (int32)
Files: `movie_companies/company_id.bin`, `movie_companies/movie_id.bin` (2609129).

### cast_info.movie_id, .person_id (int32)
Files: `cast_info/movie_id.bin`, `cast_info/person_id.bin` (36244344). offsets_only on movie_id.

### name.id, name.name (int32 / varlen)
Files: `name/id.bin`, `name/name.{off,dat}` (4167491). No filter.

### aka_name.person_id, aka_name.name (int32 / varlen)
Files: `aka_name/person_id.bin`, `aka_name/name.{off,dat}` (901343; sorted by person_id).

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| company_name | 234,997 | id | dense-PK; country_code int16 dict |
| keyword | 134,170 | id | dense-PK |
| name | 4,167,491 | id | dense-PK |
| title | 2,528,312 | id | dense-PK |
| aka_name | 901,343 | person_id | offsets_only |
| cast_info | 36,244,344 | movie_id | offsets_only |
| movie_companies | 2,609,129 | movie_id | offsets_only |
| movie_keyword | 4,523,930 | movie_id | CSR on keyword_id |

## Query Analysis

Same graph as Q16a/b/c. Variant tightens episode_nr range to `[5, 100)`.

MIN: `MIN(an.name)`, `MIN(t.title)`.

Driver: keyword-driven via CSR `movie_keyword__keyword_id`.
1. Resolve `us_code`, `k_id`.
2. CSR enumerate mk rows for `k_id`; `mv = mk.movie_id[r]`.
3. Title filter: `t_ep[mv-1] != INT32_MIN && >= 5 && < 100`.
4. Probe `movie_companies__movie_id` for mv; require some mc row with `cn_cc[company_id-1]==us_code`.
5. Probe `cast_info__movie_id`; for each ci row get `pid`.
6. For each pid, use `aka_name__person_id` to fetch an rows; update `MIN(an.name)`.
7. Update `MIN(t.title)`.

Selectivities:
- character-name-in-title keyword → small set of mk rows.
- episode_nr ∈ [5,100) → episodes only; small fraction of all titles.
- cn '[us]' → ~30%.

LIKE notes: none.

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

### aka_name__person_id (offsets_only)
File: `_idx/aka_name__person_id__offsets.bin` (int32, 4167493).
