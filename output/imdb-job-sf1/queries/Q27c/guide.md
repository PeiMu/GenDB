# Q27c Guide

## SQL
```sql
SELECT MIN(cn.name) AS producing_company,
       MIN(lt.link) AS link_type,
       MIN(t.title) AS complete_western_sequel
FROM complete_cast AS cc, comp_cast_type AS cct1, comp_cast_type AS cct2,
     company_name AS cn, company_type AS ct, keyword AS k, link_type AS lt,
     movie_companies AS mc, movie_info AS mi, movie_keyword AS mk,
     movie_link AS ml, title AS t
WHERE cct1.kind = 'cast'
  AND cct2.kind LIKE 'complete%'
  AND cn.country_code != '[pl]'
  AND (cn.name LIKE '%Film%' OR cn.name LIKE '%Warner%')
  AND ct.kind = 'production companies'
  AND k.keyword = 'sequel'
  AND lt.link LIKE '%follow%'
  AND mc.note IS NULL
  AND mi.info IN ('Sweden','Norway','Germany','Denmark',
                  'Swedish','Denish','Norwegian','German','English')
  AND t.production_year BETWEEN 1950 AND 2010
  AND lt.id = ml.link_type_id AND ml.movie_id = t.id
  AND t.id = mk.movie_id AND mk.keyword_id = k.id
  AND t.id = mc.movie_id AND mc.company_type_id = ct.id AND mc.company_id = cn.id
  AND mi.movie_id = t.id AND t.id = cc.movie_id
  AND cct1.id = cc.subject_id AND cct2.id = cc.status_id;
```

## Column Reference

### comp_cast_type.kind (filter, varlen)
- Files: `comp_cast_type/kind.off`, `comp_cast_type/kind.dat`; rows: 4; dense PK.
- Use: resolve `cast_id` (==`'cast'`); build `complete_prefix_set` of ids whose kind STARTS WITH `complete` (LIKE 'complete%'). Note prefix vs %complete% — use `memcmp(s,"complete",8)==0`.

### company_name.name (filter, varlen)
- Files: `company_name/name.off`, `company_name/name.dat`; rows: 234997.
- Use: build `cn_set` of ids whose name contains `Film` or `Warner`.

### company_name.country_code (filter, int16 dict)
- Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`.
- Use: resolve `pl_code`, exclude during set build.

### company_type.kind (filter, varlen)
- Files: `company_type/kind.off`, `company_type/kind.dat`; rows: 4.
- Resolve `ct_id`.

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
- 9-element literal set; note `'Denish'` is a benchmark typo for Danish, keep verbatim.

### movie_keyword.movie_id / .keyword_id
- Files: `movie_keyword/movie_id.bin`, `movie_keyword/keyword_id.bin`.

### movie_link.movie_id / .link_type_id
- Files: `movie_link/movie_id.bin`, `movie_link/link_type_id.bin`.

### complete_cast.movie_id / .subject_id / .status_id
- Files: `complete_cast/movie_id.bin`, `complete_cast/subject_id.bin`, `complete_cast/status_id.bin`.

### title.id / .production_year / .title
- Files: `title/id.bin`, `title/production_year.bin`, `title/title.off`+`.dat`.
- Filter year BETWEEN 1950 AND 2010.

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
- Drive from `sequel` keyword via CSR (small set). For each candidate t_id, check year, then probe ml, mc, cc, mi.
- Build once: `cast_id`, `complete_prefix_set`, `pl_code`, `cn_set`, `ct_id`, `sequel_kw_id`, `lt_set`, mi.info 9-literal hashset.
- Per t_id: year ∈ [1950,2010] → ml (link∈lt_set) → mc (ct, cn_set, note NULL) → cc (subject==cast_id, status∈complete_prefix_set) → mi (info∈set).
- Selectivities: sequel keyword ~0.06% of movies; year window very broad (~95%); mc filters cut hard via cn_set.
- LIKE notes: `complete%` is prefix only (not substring); `%Film%`/`%Warner%` substring; `%follow%` substring on 18-row link_type.

## Indexes
- `movie_keyword__keyword_id` (CSR)
  - Files: `_idx/movie_keyword__keyword_id__offsets.bin`, `_idx/movie_keyword__keyword_id__rowids.bin`.
  - Use: enumerate sequel movies via rowids; `t_id = mk_movie_id[rowids[k]]`.
- `movie_link__movie_id` (offsets_only): `_idx/movie_link__movie_id__offsets.bin`.
- `movie_companies__movie_id` (offsets_only): `_idx/movie_companies__movie_id__offsets.bin`.
- `complete_cast__movie_id` (offsets_only): `_idx/complete_cast__movie_id__offsets.bin`.
- `movie_info__movie_id` (offsets_only): `_idx/movie_info__movie_id__offsets.bin`.

Usage:
```cpp
int32_t lo = off[t_id], hi = off[t_id + 1];
for (int32_t r = lo; r < hi; ++r) { /* row r */ }
```

Rules: index slot 0 holds NULL/<1 counts; varlen `.off`+`.dat`; never invent dict codes; resolve `[pl]` at runtime.
