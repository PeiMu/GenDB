# Q27b Guide

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
  AND t.production_year = 1998
  AND lt.id = ml.link_type_id AND ml.movie_id = t.id
  AND t.id = mk.movie_id AND mk.keyword_id = k.id
  AND t.id = mc.movie_id AND mc.company_type_id = ct.id AND mc.company_id = cn.id
  AND mi.movie_id = t.id AND t.id = cc.movie_id
  AND cct1.id = cc.subject_id AND cct2.id = cc.status_id;
```

## Column Reference

### comp_cast_type.kind (filter, varlen)
- Files: `comp_cast_type/kind.off`, `comp_cast_type/kind.dat`; rows: 4.
- Resolve `cast_id`, `crew_id` → `cct1_set`; `complete_id` (`=='complete'`, exact match — single id).

### company_name.name (filter, varlen)
- Files: `company_name/name.off`, `company_name/name.dat`; rows: 234997; dense PK.
- Use: scan, push ids whose name contains `Film` or `Warner` (memmem) into `cn_set`.

### company_name.country_code (filter, int16 dict)
- Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`.
- Use: resolve `pl_code` from dict; while building `cn_set`, require `code != pl_code`.

### company_type.kind (filter, varlen)
- Files: `company_type/kind.off`, `company_type/kind.dat`; rows: 4.
- Resolve `ct_id` for `'production companies'`.

### keyword.keyword (filter, varlen)
- Files: `keyword/keyword.off`, `keyword/keyword.dat`; rows: 134170.
- Resolve `sequel_kw_id`.

### link_type.link (filter, varlen)
- Files: `link_type/link.off`, `link_type/link.dat`; rows: 18.
- Build `lt_set` (link contains `follow`).

### movie_companies.movie_id / .company_type_id / .company_id / .note
- Files: `movie_companies/movie_id.bin`, `movie_companies/company_type_id.bin`, `movie_companies/company_id.bin`, `movie_companies/note.off`+`.dat`.

### movie_info.movie_id / .info
- Files: `movie_info/movie_id.bin`, `movie_info/info.off`+`.dat`.
- Test info ∈ 4-element set.

### movie_keyword.movie_id / .keyword_id
- Files: `movie_keyword/movie_id.bin`, `movie_keyword/keyword_id.bin`.

### movie_link.movie_id / .link_type_id
- Files: `movie_link/movie_id.bin`, `movie_link/link_type_id.bin`.

### complete_cast.movie_id / .subject_id / .status_id
- Files: `complete_cast/movie_id.bin`, `complete_cast/subject_id.bin`, `complete_cast/status_id.bin`.

### title.id / .production_year / .title
- Files: `title/id.bin`, `title/production_year.bin`, `title/title.off`+`.dat`.
- Filter: `production_year == 1998` (single year, very selective).

## Table Stats
| Table | Rows | Role | Sort Order | Block Size |
|---|---|---|---|---|
| comp_cast_type | 4 | dim | id | n/a |
| company_type | 4 | dim | id | n/a |
| link_type | 18 | dim | id | n/a |
| keyword | 134170 | dim | id | 100000 |
| company_name | 234997 | dim (filter set) | id | 100000 |
| title | 2528312 | candidate filter | id | 100000 |
| movie_link | 29997 | fact | movie_id | 50000 |
| complete_cast | 135086 | fact | movie_id | 100000 |
| movie_companies | 2609129 | fact | movie_id | 100000 |
| movie_keyword | 4523930 | driver via sequel | movie_id | 100000 |
| movie_info | 14835720 | fact | movie_id | 200000 |

## Query Analysis
- `year=1998` AND `keyword='sequel'` are the two strongest filters. Best driver: iterate `movie_keyword` for sequel via CSR `movie_keyword__keyword_id`. For each candidate t_id, then test title year first.
- Build once: `cct1_set`, `complete_id`, `pl_code`, `cn_set`, `ct_id`, `sequel_kw_id`, `lt_set`, mi.info 4-literal set.
- Per t_id: title.production_year==1998 → ml has link∈lt_set → mc has ct/cn/note=NULL → cc has subject∈cct1_set & status==complete_id → mi has info in set.
- Selectivities: year=1998 ~1.5%; sequel keyword ~0.06% of movies; mc.note IS NULL ~50%; mi.info IN 4 ~0.02%.
- MIN: 3 outputs.

## Indexes
- `movie_keyword__keyword_id` (CSR)
  - Files: `_idx/movie_keyword__keyword_id__offsets.bin`, `_idx/movie_keyword__keyword_id__rowids.bin`.
  - Use: `lo=off[sequel_kw_id]; hi=off[sequel_kw_id+1]; for k in [lo,hi): mk_row=rowids[k]; t_id=mk_movie_id[mk_row]`.
- `movie_link__movie_id` (offsets_only): `_idx/movie_link__movie_id__offsets.bin`.
- `movie_companies__movie_id` (offsets_only): `_idx/movie_companies__movie_id__offsets.bin`.
- `complete_cast__movie_id` (offsets_only): `_idx/complete_cast__movie_id__offsets.bin`.
- `movie_info__movie_id` (offsets_only): `_idx/movie_info__movie_id__offsets.bin`.

Usage:
```cpp
int32_t lo = off[t_id], hi = off[t_id + 1];
for (int32_t r = lo; r < hi; ++r) { /* row r */ }
```

Rules: NULL varlen via empty entry; dict code 0 = NULL; resolve `[pl]` from `.dict.*`; never invent index files.
