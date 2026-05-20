# Q28c Guide

## SQL
```sql
SELECT MIN(cn.name) AS movie_company,
       MIN(mi_idx.info) AS rating,
       MIN(t.title) AS complete_euro_dark_movie
FROM complete_cast AS cc, comp_cast_type AS cct1, comp_cast_type AS cct2,
     company_name AS cn, company_type AS ct, info_type AS it1, info_type AS it2,
     keyword AS k, kind_type AS kt, movie_companies AS mc, movie_info AS mi,
     movie_info_idx AS mi_idx, movie_keyword AS mk, title AS t
WHERE cct1.kind = 'cast'
  AND cct2.kind = 'complete'
  AND cn.country_code != '[us]'
  AND it1.info = 'countries'
  AND it2.info = 'rating'
  AND k.keyword IN ('murder','murder-in-title','blood','violence')
  AND kt.kind IN ('movie','episode')
  AND mc.note NOT LIKE '%(USA)%'
  AND mc.note LIKE '%(200%)%'
  AND mi.info IN ('Sweden','Norway','Germany','Denmark','Swedish','Danish',
                  'Norwegian','German','USA','American')
  AND mi_idx.info < '8.5'
  AND t.production_year > 2005
  AND kt.id = t.kind_id AND t.id = mi.movie_id AND t.id = mk.movie_id
  AND t.id = mi_idx.movie_id AND t.id = mc.movie_id AND t.id = cc.movie_id
  AND k.id = mk.keyword_id AND it1.id = mi.info_type_id
  AND it2.id = mi_idx.info_type_id AND ct.id = mc.company_type_id
  AND cn.id = mc.company_id AND cct1.id = cc.subject_id
  AND cct2.id = cc.status_id;
```

## Column Reference

### comp_cast_type.kind (filter, varlen)
- Files: `comp_cast_type/kind.off`, `comp_cast_type/kind.dat`; rows: 4.
- Resolve `cast_id` (==`'cast'`) and `complete_id` (==`'complete'`). Both are single equality.

### company_name.name / .country_code
- Files: `company_name/name.off`+`.dat`; `company_name/country_code.bin`+`.dict.off`+`.dict.dat`; rows: 234997.
- Resolve `us_code`; `cn_set = {ids where code != us_code}`.

### company_type.kind (join only)
- Files: `company_type/kind.off`, `company_type/kind.dat`. No filter; join always satisfied.

### info_type.info (filter, varlen)
- Files: `info_type/info.off`, `info_type/info.dat`; rows: 113.
- Resolve `it1_id` (`'countries'`), `it2_id` (`'rating'`).

### keyword.keyword (filter, varlen)
- Files: `keyword/keyword.off`, `keyword/keyword.dat`. `kw_set` 4 ids.

### kind_type.kind (filter, varlen)
- Files: `kind_type/kind.off`, `kind_type/kind.dat`. Resolve `kt_set={movie_id, episode_id}`.

### movie_companies.movie_id / .company_id / .company_type_id / .note
- Files: `movie_companies/movie_id.bin`, `movie_companies/company_id.bin`, `movie_companies/company_type_id.bin`, `movie_companies/note.off`+`.dat`.
- Filter: `company_id ∈ cn_set` AND note checks (`NOT LIKE '%(USA)%'` AND `LIKE '%(200%)%'`).

### movie_info.movie_id / .info_type_id / .info
- Files: `movie_info/movie_id.bin`, `movie_info/info_type_id.bin`, `movie_info/info.off`+`.dat`.
- Filter: `info_type_id == it1_id && info ∈ 10-literal set`.

### movie_info_idx.movie_id / .info_type_id / .info
- Files: `movie_info_idx/movie_id.bin`, `movie_info_idx/info_type_id.bin`, `movie_info_idx/info.off`+`.dat`.
- Filter: `info_type_id == it2_id && lex(info) < "8.5"`. Project for MIN.

### movie_keyword.movie_id / .keyword_id
- Files: `movie_keyword/movie_id.bin`, `movie_keyword/keyword_id.bin`.

### complete_cast.movie_id / .subject_id / .status_id
- Files: `complete_cast/movie_id.bin`, `complete_cast/subject_id.bin`, `complete_cast/status_id.bin`.
- Filter: `subject_id == cast_id && status_id == complete_id`.

### title.id / .kind_id / .production_year / .title
- Files: `title/id.bin`, `title/kind_id.bin`, `title/production_year.bin`, `title/title.off`+`.dat`. year>2005.

## Table Stats
| Table | Rows | Role | Sort Order | Block Size |
|---|---|---|---|---|
| comp_cast_type | 4 | dim | id | n/a |
| company_type | 4 | dim (join only) | id | n/a |
| info_type | 113 | dim | id | n/a |
| kind_type | 7 | dim | id | n/a |
| keyword | 134170 | dim | id | 100000 |
| company_name | 234997 | dim (filter) | id | 100000 |
| title | 2528312 | driver | id | 100000 |
| complete_cast | 135086 | fact | movie_id | 100000 |
| movie_info_idx | 1380035 | fact | movie_id | 100000 |
| movie_companies | 2609129 | fact | movie_id | 100000 |
| movie_keyword | 4523930 | fact | movie_id | 100000 |
| movie_info | 14835720 | fact | movie_id | 200000 |

## Query Analysis
- Identical structure to Q28a but with cct1='cast' (vs 'crew') and cct2='complete' equality (vs `!=`).
- Build once: `cast_id`, `complete_id`, `us_code`, `cn_set`, `it1_id`, `it2_id`, `kw_set`, `kt_set`, mi.info 10-literal hashset.
- Driver: title with year>2005 and kind_id ∈ kt_set.
- Probe order per t_id (most-selective first): movie_keyword (kw_set) → complete_cast (subject==cast_id & status==complete_id, very selective ~5% of cc) → movie_info_idx → movie_companies → movie_info.
- MIN: 3 outputs.
- LIKE notes: same `(USA)` / `(200X)` parse.

## Indexes
- `movie_keyword__movie_id` (offsets_only): `_idx/movie_keyword__movie_id__offsets.bin`.
- `complete_cast__movie_id` (offsets_only): `_idx/complete_cast__movie_id__offsets.bin`.
- `movie_info_idx__movie_id` (offsets_only): `_idx/movie_info_idx__movie_id__offsets.bin`.
- `movie_companies__movie_id` (offsets_only): `_idx/movie_companies__movie_id__offsets.bin`.
- `movie_info__movie_id` (offsets_only): `_idx/movie_info__movie_id__offsets.bin`.

Usage (all offsets_only):
```cpp
int32_t lo = off[t_id], hi = off[t_id + 1];
for (int32_t r = lo; r < hi; ++r) { /* row r */ }
```

Rules: dict code 0 == NULL (NULL country_code matches `!= '[us]'`); resolve `[us]` from dict at runtime; never invent indexes; varlen `.off`+`.dat`.
