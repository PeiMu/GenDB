# Q27a Guide

## SQL
```sql
SELECT MIN(cn.name) AS producing_company,
       MIN(lt.link) AS link_type,
       MIN(t.title) AS complete_western_sequel
FROM complete_cast AS cc, comp_cast_type AS cct1, comp_cast_type AS cct2,
     company_name AS cn, company_type AS ct, keyword AS k, link_type AS lt,
     movie_companies AS mc, movie_info AS mi, movie_keyword AS mk,
     movie_link AS ml, title AS t
WHERE cct1.kind IN ('cast','crew')
  AND cct2.kind = 'complete'
  AND cn.country_code != '[pl]'
  AND (cn.name LIKE '%Film%' OR cn.name LIKE '%Warner%')
  AND ct.kind = 'production companies'
  AND k.keyword = 'sequel'
  AND lt.link LIKE '%follow%'
  AND mc.note IS NULL
  AND mi.info IN ('Sweden','Germany','Swedish','German')
  AND t.production_year BETWEEN 1950 AND 2000
  AND lt.id = ml.link_type_id AND ml.movie_id = t.id
  AND t.id = mk.movie_id AND mk.keyword_id = k.id
  AND t.id = mc.movie_id AND mc.company_type_id = ct.id AND mc.company_id = cn.id
  AND mi.movie_id = t.id AND t.id = cc.movie_id
  AND cct1.id = cc.subject_id AND cct2.id = cc.status_id;
```

## Column Reference

### comp_cast_type.kind (filter, varlen)
- Files: `comp_cast_type/kind.off`, `comp_cast_type/kind.dat`; rows: 4.
- Use: resolve `cast_id` and `crew_id`; `cct1_set = {cast_id, crew_id}`. Resolve `complete_id` (==`'complete'`).

### company_name.name (filter, varlen)
- Files: `company_name/name.off`, `company_name/name.dat`; rows: 234997; dense PK.
- Use: scan to build `cn_set` of ids whose name has substring `Film` or `Warner` (case-sensitive memmem).

### company_name.country_code (filter, int16 dict)
- Files: `company_name/country_code.bin`, `company_name/country_code.dict.off`, `company_name/country_code.dict.dat`.
- Use: resolve `pl_code` for `[pl]` from dict; keep ids whose code `!= pl_code` (also code 0 = NULL is accepted by `!=`). Combine into `cn_set` filter.

### company_type.kind (filter, varlen)
- Files: `company_type/kind.off`, `company_type/kind.dat`; rows: 4; dense PK.
- Use: resolve `ct_id` for `'production companies'`.

### keyword.keyword (filter, varlen)
- Files: `keyword/keyword.off`, `keyword/keyword.dat`; rows: 134170.
- Use: resolve `sequel_kw_id` (single id).

### link_type.link (filter, varlen)
- Files: `link_type/link.off`, `link_type/link.dat`; rows: 18.
- Use: scan to build `lt_set` of ids whose link contains `follow` (memmem).

### movie_companies.movie_id / .company_type_id / .company_id / .note
- Files: `movie_companies/movie_id.bin`, `movie_companies/company_type_id.bin`, `movie_companies/company_id.bin`, `movie_companies/note.off`+`.dat`.
- Rows: 2609129; sorted by movie_id.
- Use: range probe via `movie_companies__movie_id`. Filter `company_type_id == ct_id`, `company_id ∈ cn_set`, `note` is NULL (`off[r]==off[r+1]`).

### movie_info.movie_id / .info
- Files: `movie_info/movie_id.bin`, `movie_info/info.off`+`.dat`; rows: 14835720; sorted.
- Use: range probe via `movie_info__movie_id`; test `info ∈ {Sweden,Germany,Swedish,German}` (hashset of string_views; length prefilter ≤7).

### movie_keyword.movie_id / .keyword_id
- Files: `movie_keyword/movie_id.bin`, `movie_keyword/keyword_id.bin`; rows: 4523930.
- Use: range probe; require `keyword_id == sequel_kw_id`. Alternative driver: use `movie_keyword__keyword_id` CSR to get all `sequel` movies directly.

### movie_link.movie_id / .link_type_id
- Files: `movie_link/movie_id.bin`, `movie_link/link_type_id.bin`; rows: 29997; sorted.
- Use: range probe via `movie_link__movie_id`; test `link_type_id ∈ lt_set`.

### complete_cast.movie_id / .subject_id / .status_id
- Files: `complete_cast/movie_id.bin`, `complete_cast/subject_id.bin`, `complete_cast/status_id.bin`.
- Filter: `subject_id ∈ cct1_set` AND `status_id == complete_id`.

### title.id / .production_year / .title
- Files: `title/id.bin`, `title/production_year.bin`, `title/title.off`+`.dat`.
- Driver. Year between 1950 and 2000 inclusive.

## Table Stats
| Table | Rows | Role | Sort Order | Block Size |
|---|---|---|---|---|
| comp_cast_type | 4 | dim | id | n/a |
| company_type | 4 | dim | id | n/a |
| link_type | 18 | dim | id | n/a |
| keyword | 134170 | dim | id | 100000 |
| company_name | 234997 | dim (filter set) | id | 100000 |
| title | 2528312 | driver | id | 100000 |
| movie_link | 29997 | fact | movie_id | 50000 |
| complete_cast | 135086 | fact | movie_id | 100000 |
| movie_companies | 2609129 | fact | movie_id | 100000 |
| movie_keyword | 4523930 | fact | movie_id | 100000 |
| movie_info | 14835720 | fact | movie_id | 200000 |

## Query Analysis
- Most selective entry: `keyword='sequel'` → very small movie set via `movie_keyword__keyword_id` CSR. Use it as driver: iterate sequel movies, derive `t_id`s, then probe other facts.
- Build once: `cast_id`,`crew_id` and union → `cct1_set`; `complete_id`; `pl_code`; `cn_set`; `ct_id`; `sequel_kw_id`; `lt_set`; mi.info hashset (4 literals).
- Per candidate t_id:
  1. title row r=t_id-1: check production_year BETWEEN 1950 AND 2000.
  2. movie_link range: any row with link_type_id ∈ lt_set?
  3. movie_companies range: row with `company_type_id==ct_id && company_id∈cn_set && note IS NULL`.
  4. complete_cast range: row with `subject_id∈cct1_set && status_id==complete_id`.
  5. movie_info range: row with info ∈ literal set.
- Selectivities: sequel keyword very small (<2000 movies); year 1950-2000 ~60%; mi.info IN ~0.05% per movie; `cn.country_code != '[pl]'` ~99.9%.
- MIN: 3 outputs (`cn.name`, `lt.link`, `t.title`). cn fetched per surviving mc row, lt per surviving ml row.
- LIKE: `%Film%`/`%Warner%` memmem on company_name; `%follow%` memmem on the 18-row link_type.

## Indexes
- `movie_keyword__keyword_id` (CSR)
  - Files: `_idx/movie_keyword__keyword_id__offsets.bin` (int32, length 134172), `_idx/movie_keyword__keyword_id__rowids.bin` (int32, length 4523930).
  - Use: `lo=off[sequel_kw_id]; hi=off[sequel_kw_id+1]; for k in [lo,hi): mk_row=rowids[k]; t_id=mk_movie_id[mk_row]`.
- `movie_link__movie_id` (offsets_only): `_idx/movie_link__movie_id__offsets.bin` (int32, 2528314).
- `movie_companies__movie_id` (offsets_only): `_idx/movie_companies__movie_id__offsets.bin` (int32, 2528314).
- `complete_cast__movie_id` (offsets_only): `_idx/complete_cast__movie_id__offsets.bin`.
- `movie_info__movie_id` (offsets_only): `_idx/movie_info__movie_id__offsets.bin`.

Usage:
```cpp
int32_t lo = off[t_id], hi = off[t_id + 1];
for (int32_t r = lo; r < hi; ++r) { /* child row r */ }
```

Rules: NULL varlen detected via `off[r]==off[r+1]`; dict code 0 = NULL; never hardcode dict codes; resolve `[pl]` from country_code.dict.*.
