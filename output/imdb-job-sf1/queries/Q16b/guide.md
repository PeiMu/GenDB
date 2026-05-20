## SQL

```sql
SELECT MIN(an.name) AS cool_actor_pseudonym,
       MIN(t.title) AS series_named_after_char
FROM aka_name AS an, cast_info AS ci, company_name AS cn, keyword AS k,
     movie_companies AS mc, movie_keyword AS mk, name AS n, title AS t
WHERE cn.country_code = '[us]'
  AND k.keyword = 'character-name-in-title'
  AND an.person_id = n.id
  AND n.id = ci.person_id
  AND ci.movie_id = t.id
  AND t.id = mk.movie_id AND mk.keyword_id = k.id
  AND t.id = mc.movie_id AND mc.company_id = cn.id;
```

## Column Reference

### company_name.country_code (int16 dict)
Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`. Resolve `us_code` at runtime; never hardcode.

### keyword.keyword (varlen)
Files: `keyword/keyword.off`, `keyword/keyword.dat`. Resolve `k_id = id of 'character-name-in-title'`.

### title.title (varlen)
Files: `title/title.off`, `title/title.dat` (length 2528312). No predicate on title columns in Q16b — every t row is candidate. (Note: episode_nr filter removed compared to Q16a.)

### movie_keyword.keyword_id, .movie_id (int32)
Files: `movie_keyword/keyword_id.bin`, `movie_keyword/movie_id.bin` (4523930; sorted by movie_id). CSR `movie_keyword__keyword_id` for k-driven enumeration.

### movie_companies.company_id, .movie_id (int32)
Files: `movie_companies/company_id.bin`, `movie_companies/movie_id.bin` (2609129).

### cast_info.movie_id, .person_id (int32)
Files: `cast_info/movie_id.bin`, `cast_info/person_id.bin` (36244344; sorted by movie_id). offsets_only on movie_id.

### name.id (int32), name.name (varlen)
Files: `name/id.bin` (identity), `name/name.{off,dat}` (4167491). No filter on name in Q16b.

### aka_name.person_id, aka_name.name (int32 / varlen)
Files: `aka_name/person_id.bin`, `aka_name/name.{off,dat}` (901343; sorted by person_id). offsets_only on person_id.

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| company_name | 234,997 | id | dense-PK; country_code int16 dict |
| keyword | 134,170 | id | dense-PK |
| name | 4,167,491 | id | dense-PK |
| title | 2,528,312 | id | dense-PK |
| aka_name | 901,343 | person_id | offsets_only on person_id |
| cast_info | 36,244,344 | movie_id | offsets_only on movie_id |
| movie_companies | 2,609,129 | movie_id | offsets_only on movie_id |
| movie_keyword | 4,523,930 | movie_id | CSR on keyword_id |

## Query Analysis

Join graph: identical to Q16a, but without the episode_nr filter. Result set is much larger.

MIN: `MIN(an.name)`, `MIN(t.title)` (both varlen).

Driver: keyword-driven via `movie_keyword__keyword_id`.
1. Resolve `us_code`, `k_id`.
2. CSR `movie_keyword__keyword_id[k_id]` → mk rows; collect unique `mv` values.
3. For each mv:
   - Probe `movie_companies__movie_id`; require any mc row with `cn_cc[company_id-1]==us_code`.
   - If qualifying, probe `cast_info__movie_id` for mv; for each ci row get `pid = person_id`.
   - For each pid, use `aka_name__person_id` offsets to fetch an rows; update MIN(an.name).
   - Update MIN(t.title) using `title.title[mv-1]` once.

Selectivities:
- `k.keyword='character-name-in-title'` → 1 of 134K keywords; mk_rows can be moderate (~thousands).
- `cn.country_code='[us]'` → ~30%.
- No episode filter — many more movies pass.
- Many ci rows per movie; many pids without an rows.

LIKE notes: none.

Optimization: since there is no name-filter, hold a global `min_an_name` (string) and `min_title` (string). Skip pid lookup if `min_an_name` is already lexicographically smallest possible empty-prefix candidate (only stop on early break of MIN aggregation — not a true short-circuit without ordering).

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
