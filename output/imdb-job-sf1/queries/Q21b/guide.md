## SQL

```sql
SELECT MIN(cn.name) AS company_name,
       MIN(lt.link) AS link_type,
       MIN(t.title) AS german_follow_up
FROM company_name AS cn, company_type AS ct, keyword AS k,
     link_type AS lt, movie_companies AS mc, movie_info AS mi,
     movie_keyword AS mk, movie_link AS ml, title AS t
WHERE cn.country_code!='[pl]'
  AND (cn.name LIKE '%Film%' OR cn.name LIKE '%Warner%')
  AND ct.kind='production companies'
  AND k.keyword='sequel'
  AND lt.link LIKE '%follow%'
  AND mc.note IS NULL
  AND mi.info IN ('Germany','German')
  AND t.production_year BETWEEN 2000 AND 2010
  AND lt.id=ml.link_type_id AND ml.movie_id=t.id AND t.id=mk.movie_id
  AND mk.keyword_id=k.id AND t.id=mc.movie_id AND mc.company_type_id=ct.id
  AND mc.company_id=cn.id AND mi.movie_id=t.id;
```

## Column Reference

Same columns as Q21a. Differences:
- mi.info IN-list is just 2 strings: `'Germany'`, `'German'`.
- production_year BETWEEN 2000 AND 2010 (narrower).

### company_type.kind, keyword.keyword, link_type.link, company_name.country_code
Resolve `ct_pc`, `k_sequel`, `LT_ids`, `cc_pl` as in Q21a.

### company_name.name (varlen)
LIKE `%Film%` OR `%Warner%` → `CN_ids` bitset (size 234998).

### movie_companies.note, movie_id, company_id, company_type_id
Same files as Q21a.

### movie_info.info, movie_id
IN-list 2 strings: build small hash set of 2 `string_view`s.

### movie_keyword.movie_id, keyword_id
Same.

### movie_link.movie_id, linked_movie_id, link_type_id
Same; 29997 rows.

### title.id, title, production_year
`production_year ∈ [2000,2010] AND != INT32_MIN`.

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| company_name | 234,997 | id | dense-PK |
| company_type | 4 | id | dense-PK |
| keyword | 134,170 | id | dense-PK |
| link_type | 18 | id | dense-PK |
| movie_companies | 2,609,129 | movie_id | offsets_only movie_id; CSR company_id/company_type_id |
| movie_info | 14,835,720 | movie_id | offsets_only movie_id |
| movie_keyword | 4,523,930 | movie_id | offsets_only movie_id; CSR keyword_id |
| movie_link | 29,997 | movie_id | offsets_only movie_id; CSR link_type_id |
| title | 2,528,312 | id | dense-PK |

## Query Analysis

Join graph same as Q21a.

Plan:
1. Resolve dim ids: `ct_pc`, `k_sequel`, `LT_ids`, `cc_pl`. Build `CN_ids` and `MI_set={"Germany","German"}`.
2. Build `seq_movies` bitset via `movie_keyword__keyword_id` CSR for `k_sequel`.
3. Walk `movie_link__link_type_id` CSR for each lt in `LT_ids`; collect ml rows and their `mv=movie_id[r]`. (Or scan ml linearly.)
4. For each candidate `mv`:
   - Year: `title.production_year[mv-1] ∈ [2000,2010] AND != INT32_MIN`.
   - `seq_movies[mv]` must be true.
   - Walk `movie_info__movie_id` range; require row with `info ∈ MI_set`.
   - Walk `movie_companies__movie_id` range; for each mc row require note empty, `company_type_id==ct_pc`, `CN_ids[company_id]`. Track best cn.name.
   - Update MIN(cn.name), MIN(lt.link), MIN(t.title).

Selectivities:
- `lt.link LIKE '%follow%'` → ~few link_type ids (e.g. follows, followed by, etc.). ml rows → small subset of 30K.
- year 2000..2010 → ~20% titles.
- k.keyword='sequel' → small bitset.
- mi.info IN {Germany, German} → very small subset.
- cn.name + country!=PL → small.

LIKE notes: `%Film%`, `%Warner%`, `%follow%` via `memmem`.

## Indexes

### movie_link__link_type_id (CSR)
Files: `_idx/movie_link__link_type_id__{offsets,rowids}.bin`. parent_max=18.

### movie_keyword__keyword_id (CSR)
Files: `_idx/movie_keyword__keyword_id__{offsets,rowids}.bin`. Pre-build `seq_movies` bitset.

### movie_info__movie_id (offsets_only)
File: `_idx/movie_info__movie_id__offsets.bin`.

### movie_companies__movie_id (offsets_only)
File: `_idx/movie_companies__movie_id__offsets.bin`.

### movie_link__movie_id (offsets_only)
File: `_idx/movie_link__movie_id__offsets.bin`. Alt driver when mv set is small.

### movie_keyword__movie_id (offsets_only)
File: `_idx/movie_keyword__movie_id__offsets.bin`. Alt if not using the bitset approach.
