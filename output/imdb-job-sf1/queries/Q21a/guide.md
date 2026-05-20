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
  AND mi.info IN ('Sweden','Norway','Germany','Denmark','Swedish','Denish','Norwegian','German')
  AND t.production_year BETWEEN 1950 AND 2000
  AND lt.id=ml.link_type_id AND ml.movie_id=t.id AND t.id=mk.movie_id
  AND mk.keyword_id=k.id AND t.id=mc.movie_id AND mc.company_type_id=ct.id
  AND mc.company_id=cn.id AND mi.movie_id=t.id;
```

## Column Reference

### company_type.kind (varlen, 4 rows)
Files: `company_type/kind.off`, `company_type/kind.dat`. Resolve `'production companies'` → `ct_pc`.

### keyword.keyword (varlen)
Files: `keyword/keyword.off`, `keyword/keyword.dat`. Resolve `'sequel'` → `k_sequel`. Then take `kw_seq_movies` set via mk CSR.

### link_type.link (varlen, 18 rows)
Files: `link_type/link.off`, `link_type/link.dat`. LIKE `%follow%` → set `LT_ids` (likely {'follows','followed by',...}).

### company_name.country_code (int16 dict)
Files: `company_name/country_code.bin` (234997), `.dict.off`, `.dict.dat`. Resolve `'[pl]'` → `cc_pl`. Filter is `!= cc_pl`. Treat NULL (code 0) as not equal so it passes (matches Postgres `<> '[pl]'` which excludes NULL; care: in Postgres NULL `!=` yields NULL → row excluded; mimic that by requiring code != 0 AND != cc_pl).

### company_name.name (varlen)
Files: `company_name/name.off`, `company_name/name.dat`. 234997 rows. LIKE `%Film%` OR `%Warner%` via `memmem`. Build dense `CN_ids` bitset (size 234998).

### movie_companies.note, movie_id, company_id, company_type_id (varlen + int32 ×3)
Files in `movie_companies/`. 2609129 rows; sorted by movie_id. `note IS NULL` ⇔ `note.off[i]==note.off[i+1]`.

### movie_info.info, movie_id (varlen / int32)
Files: `movie_info/info.off+.dat`, `.../movie_id.bin`. IN-list 8 strings; length-prune then equality.

### movie_keyword.movie_id, keyword_id (int32)
Files: `movie_keyword/movie_id.bin`, `.../keyword_id.bin`. 4523930 rows.

### movie_link.movie_id, linked_movie_id, link_type_id (int32)
Files: `movie_link/movie_id.bin`, `.../linked_movie_id.bin`, `.../link_type_id.bin`. 29997 rows; sorted by movie_id.

### title.id, title, production_year
`production_year BETWEEN 1950 AND 2000 AND != INT32_MIN`.

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| company_name | 234,997 | id | dense-PK; country_code int16 dict |
| company_type | 4 | id | dense-PK |
| keyword | 134,170 | id | dense-PK |
| link_type | 18 | id | dense-PK |
| movie_companies | 2,609,129 | movie_id | offsets_only movie_id; CSR company_id/company_type_id |
| movie_info | 14,835,720 | movie_id | offsets_only movie_id |
| movie_keyword | 4,523,930 | movie_id | offsets_only movie_id; CSR keyword_id |
| movie_link | 29,997 | movie_id | offsets_only movie_id; CSR link_type_id/linked_movie_id |
| title | 2,528,312 | id | dense-PK |

## Query Analysis

Join graph (movies are the hub):
```
cn --company_id-- mc --movie_id-- t --movie_id-- mk --keyword_id-- k
ct --company_type_id-- mc        |--movie_id-- mi
                                  +--movie_id-- ml --link_type_id-- lt
```

Aggregations: MIN(cn.name), MIN(lt.link), MIN(t.title).

Driver: movie_link has only 29997 rows — the smallest fact table by far. Combine with `lt.link LIKE '%follow%'` (already small subset).

Plan:
1. Resolve `ct_pc`, `k_sequel`, `cc_pl`, `LT_ids` (link types LIKE `%follow%`), `MI_set` = 8 country strings.
2. Build `CN_ids` bitset (cn.name LIKE Film/Warner AND country_code!=cc_pl AND !=NULL).
3. Walk `movie_link__link_type_id` CSR for each lt in LT_ids; collect `ml_rows` and their `mv=movie_id[r]`. Or scan ml linearly (29997 rows) and keep rows where `link_type_id[r] ∈ LT_ids`.
4. Build candidate `M`: from ml_rows, take mv. Filter: `title.production_year[mv-1] ∈ [1950,2000] AND != INT32_MIN`.
5. For each `mv` in M:
   - Walk `movie_keyword__movie_id` range; require a row with `keyword_id==k_sequel`. (Alternative: pre-compute `mk_sequel_movies` via `movie_keyword__keyword_id` CSR for `k_sequel`, store as bitset of size 2528313, then quick lookup per mv.)
   - Walk `movie_info__movie_id` range; require any row with `info ∈ MI_set` (length-prune+equality on each row's varlen slice).
   - Walk `movie_companies__movie_id` range; for each mc row require `note.off[r]==note.off[r+1]` (NULL), `company_type_id[r]==ct_pc`, `CN_ids[company_id[r]]`. If pass, update MIN(cn.name) using `cn.name[company_id[r]-1]`.
   - Update MIN(lt.link) using the matching ml.link_type_id → `link_type.link[lt_id-1]`. MIN(t.title) from `title[mv-1]`.

Selectivities:
- ml.link_type filtered to %follow% → small subset of 30K.
- production_year 1950..2000 → ~half titles.
- k.keyword='sequel' → small mk subset (~tens of K rows).
- mi.info IN 8 strings → small.
- cn.name Film/Warner AND country!=[pl] → small but not tiny.

LIKE notes:
- `%Film%`, `%Warner%`: `memmem`, length prune.
- `%follow%`: scan 18-row link_type at startup.
- `cn.country_code != '[pl]'`: int16 code compare.

## Indexes

### movie_link__link_type_id (CSR)
Files: `_idx/movie_link__link_type_id__offsets.bin` (int32, 20), `_idx/movie_link__link_type_id__rowids.bin` (int32, 29997). Drive from LT_ids.

### movie_keyword__keyword_id (CSR)
Files: `_idx/movie_keyword__keyword_id__{offsets,rowids}.bin`. Pre-compute sequel-tagged movie bitset.
```cpp
auto mkk_off = read_vec<int32_t>(store + "/_idx/movie_keyword__keyword_id__offsets.bin");
auto mkk_row = read_vec<int32_t>(store + "/_idx/movie_keyword__keyword_id__rowids.bin");
int32_t lo=mkk_off[k_sequel], hi=mkk_off[k_sequel+1];
std::vector<uint8_t> seq(2528313,0);
for (int32_t k2=lo; k2<hi; ++k2) seq[mk_mid[mkk_row[k2]]]=1;
```

### movie_companies__movie_id (offsets_only)
File: `_idx/movie_companies__movie_id__offsets.bin`.

### movie_info__movie_id (offsets_only)
File: `_idx/movie_info__movie_id__offsets.bin`.

### movie_link__movie_id (offsets_only)
File: `_idx/movie_link__movie_id__offsets.bin`. Use if driving by mv set.

### movie_keyword__movie_id (offsets_only)
File: `_idx/movie_keyword__movie_id__offsets.bin`.
