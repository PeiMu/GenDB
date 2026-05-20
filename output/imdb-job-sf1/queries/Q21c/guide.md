## SQL

```sql
SELECT MIN(cn.name) AS company_name,
       MIN(lt.link) AS link_type,
       MIN(t.title) AS western_follow_up
FROM company_name AS cn, company_type AS ct, keyword AS k,
     link_type AS lt, movie_companies AS mc, movie_info AS mi,
     movie_keyword AS mk, movie_link AS ml, title AS t
WHERE cn.country_code!='[pl]'
  AND (cn.name LIKE '%Film%' OR cn.name LIKE '%Warner%')
  AND ct.kind='production companies'
  AND k.keyword='sequel'
  AND lt.link LIKE '%follow%'
  AND mc.note IS NULL
  AND mi.info IN ('Sweden','Norway','Germany','Denmark','Swedish','Denish','Norwegian','German','English')
  AND t.production_year BETWEEN 1950 AND 2010
  AND lt.id=ml.link_type_id AND ml.movie_id=t.id AND t.id=mk.movie_id
  AND mk.keyword_id=k.id AND t.id=mc.movie_id AND mc.company_type_id=ct.id
  AND mc.company_id=cn.id AND mi.movie_id=t.id;
```

## Column Reference

Same as Q21a/b. Differences:
- mi.info IN-list has 9 strings (adds `'English'`).
- production_year BETWEEN 1950 AND 2010 (widest of the three).

### company_type.kind, keyword.keyword, link_type.link, company_name.country_code
Resolve `ct_pc`, `k_sequel`, `LT_ids`, `cc_pl` as in Q21a.

### company_name.name (varlen)
LIKE `%Film%` OR `%Warner%` → `CN_ids` bitset.

### movie_companies.note, movie_id, company_id, company_type_id
Same files. `note IS NULL` ⇔ empty varlen.

### movie_info.info, movie_id
IN-list 9 strings; build `flat_hash_set<string_view>`.

### movie_keyword.movie_id, keyword_id
Same.

### movie_link.movie_id, linked_movie_id, link_type_id
Same; 29997 rows.

### title.id, title, production_year
`production_year ∈ [1950,2010] AND != INT32_MIN` — widest year window.

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

Join graph identical to Q21a/b. The widest year window and largest mi.info IN-list make this the broadest of the three; expect a larger intermediate.

Plan:
1. Resolve dim ids; build `CN_ids` bitset, `MI_set` (9 strings), `seq_movies` bitset (via `movie_keyword__keyword_id` for `k_sequel`).
2. Scan movie_link (29997 rows). For each ml row r: if `link_type_id[r] ∈ LT_ids`, capture `mv=movie_id[r]` and the lt id (for later MIN(lt.link)).
3. For each captured `mv`:
   - Year filter: `title.production_year[mv-1] ∈ [1950,2010] AND != INT32_MIN`.
   - `seq_movies[mv]` must be true.
   - Walk `movie_info__movie_id` range; require any row whose `info ∈ MI_set`.
   - Walk `movie_companies__movie_id` range; for each mc row require note empty, `company_type_id==ct_pc`, and `CN_ids[company_id]`. Track candidate `cn.name`.
   - If all pass, update MIN(cn.name), MIN(lt.link), MIN(t.title).

Selectivities:
- ml link_type IN %follow% set → small subset of 30K.
- year 1950..2010 → most titles.
- k_sequel bitset → small.
- mi.info IN 9 strings → still small.
- cn LIKE Film/Warner → small.

LIKE notes: `%Film%`, `%Warner%`, `%follow%` — `memmem`.

## Indexes

### movie_link__link_type_id (CSR)
Files: `_idx/movie_link__link_type_id__{offsets,rowids}.bin`. parent_max=18. Drive from LT_ids.
```cpp
auto mlt_off = read_vec<int32_t>(store + "/_idx/movie_link__link_type_id__offsets.bin");
auto mlt_row = read_vec<int32_t>(store + "/_idx/movie_link__link_type_id__rowids.bin");
for (int32_t lt_id : LT_ids) {
    int32_t lo=mlt_off[lt_id], hi=mlt_off[lt_id+1];
    for (int32_t k=lo; k<hi; ++k) { int32_t r=mlt_row[k]; int32_t mv=ml_mid[r]; /* ... */ }
}
```

### movie_keyword__keyword_id (CSR)
Files: `_idx/movie_keyword__keyword_id__{offsets,rowids}.bin`. Build sequel bitset.

### movie_info__movie_id (offsets_only)
File: `_idx/movie_info__movie_id__offsets.bin`.

### movie_companies__movie_id (offsets_only)
File: `_idx/movie_companies__movie_id__offsets.bin`.

### movie_link__movie_id (offsets_only)
File: `_idx/movie_link__movie_id__offsets.bin`. Alternative when driving by mv set.

### movie_keyword__movie_id (offsets_only)
File: `_idx/movie_keyword__movie_id__offsets.bin`. Useful for per-mv sequel check if bitset is skipped.
