# Q28a Guide

## SQL
```sql
SELECT MIN(cn.name) AS movie_company,
       MIN(mi_idx.info) AS rating,
       MIN(t.title) AS complete_euro_dark_movie
FROM complete_cast AS cc, comp_cast_type AS cct1, comp_cast_type AS cct2,
     company_name AS cn, company_type AS ct, info_type AS it1, info_type AS it2,
     keyword AS k, kind_type AS kt, movie_companies AS mc, movie_info AS mi,
     movie_info_idx AS mi_idx, movie_keyword AS mk, title AS t
WHERE cct1.kind = 'crew'
  AND cct2.kind != 'complete+verified'
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
  AND t.production_year > 2000
  AND kt.id = t.kind_id AND t.id = mi.movie_id AND t.id = mk.movie_id
  AND t.id = mi_idx.movie_id AND t.id = mc.movie_id AND t.id = cc.movie_id
  AND k.id = mk.keyword_id AND it1.id = mi.info_type_id
  AND it2.id = mi_idx.info_type_id AND ct.id = mc.company_type_id
  AND cn.id = mc.company_id AND cct1.id = cc.subject_id
  AND cct2.id = cc.status_id;
```

## Column Reference

### comp_cast_type.kind (filter, varlen)
- Files: `comp_cast_type/kind.off`, `comp_cast_type/kind.dat`; rows: 4; dense PK.
- Use: resolve `crew_id` (==`'crew'`); resolve `verified_id` (==`'complete+verified'`); `cct2_set = all ids ∈ {1..4} except verified_id` (with awareness of NULL — but cc.status_id is non-NULL by FK).

### company_name.name / .country_code (filter)
- Files: `company_name/name.off`+`.dat`; `company_name/country_code.bin`+`.dict.off`+`.dict.dat`; rows: 234997; dense PK.
- Use: resolve `us_code` from dict; `cn_set = {ids where country_code != us_code}` (~99% of cns). Name is not filtered here, but projected for MIN.

### company_type.kind (no filter; needed for join semantics?)
- Q28a has no `ct.kind = ...` predicate; ct is only joined `ct.id = mc.company_type_id`. Since company_type has 4 dense rows and mc.company_type_id is FK non-NULL, this join is always satisfied. No filter set needed.

### info_type.info (filter, varlen)
- Files: `info_type/info.off`, `info_type/info.dat`; rows: 113.
- Resolve `it1_id` for `'countries'` and `it2_id` for `'rating'`.

### keyword.keyword (filter, varlen)
- Files: `keyword/keyword.off`, `keyword/keyword.dat`; rows: 134170.
- Build `kw_set` (4 ids).

### kind_type.kind (filter, varlen)
- Files: `kind_type/kind.off`, `kind_type/kind.dat`; rows: 7.
- Resolve `movie_id` and `episode_id`; `kt_set={movie_id, episode_id}` (matching t.kind_id).

### movie_companies.movie_id / .company_id / .note (filter+join+project source)
- Files: `movie_companies/movie_id.bin`, `movie_companies/company_id.bin`, `movie_companies/company_type_id.bin`, `movie_companies/note.off`+`.dat`. Rows: 2609129; sorted.
- Use: range probe via `movie_companies__movie_id`. Filter: `company_id ∈ cn_set`, `note NOT NULL`, `memmem(note,"(USA)")==NULL`, `memmem(note,"(200")!=NULL` then ensure a closing `)` follows (LIKE `%(200%)%`). Lazily fetch `cn.name` for MIN.

### movie_info.movie_id / .info_type_id / .info
- Files: `movie_info/movie_id.bin`, `movie_info/info_type_id.bin`, `movie_info/info.off`+`.dat`. Rows: 14835720.
- Filter: `info_type_id==it1_id && info ∈ {10 country/lang literals}`.

### movie_info_idx.movie_id / .info_type_id / .info
- Files: `movie_info_idx/movie_id.bin`, `movie_info_idx/info_type_id.bin`, `movie_info_idx/info.off`+`.dat`. Rows: 1380035.
- Filter: `info_type_id==it2_id && lex(info) < "8.5"`. Project info for MIN.

### movie_keyword.movie_id / .keyword_id
- Files: `movie_keyword/movie_id.bin`, `movie_keyword/keyword_id.bin`.

### complete_cast.movie_id / .subject_id / .status_id
- Files: `complete_cast/movie_id.bin`, `complete_cast/subject_id.bin`, `complete_cast/status_id.bin`.
- Filter: `subject_id == crew_id` AND `status_id != verified_id`.

### title.id / .kind_id / .production_year / .title
- Files: `title/id.bin`, `title/kind_id.bin`, `title/production_year.bin`, `title/title.off`+`.dat`. Driver.

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
- Driver: title; pre-filter year>2000 and kind_id ∈ kt_set.
- Build once: `crew_id`, `verified_id`, `us_code`, `cn_set`, `it1_id`, `it2_id`, `kw_set`, `kt_set`, mi.info 10-element hashset.
- Probe order per t_id (most selective first): movie_keyword (kw_set, ~0.1% of movies) → complete_cast (subject==crew, status != verified) → movie_info_idx (it2 & lex<'8.5', ~70% of rating) → movie_companies (cn_set & note checks) → movie_info (it1 & info ∈ set).
- Selectivities: year>2000 ~25%; kind_type IN 2 covers ~85% of titles; cn `!= [us]` ~70% (US is dominant); mc.note LIKE `%(200%)%` AND NOT LIKE `%(USA)%` rare (~5%).
- MIN: 3 outputs.
- LIKE notes: `mc.note NOT LIKE '%(USA)%'` is a negative substring test; `%(200%)%` requires `(200` substring followed by a `)` (i.e. find `(200`, then ensure a `)` byte exists after that position).

## Indexes
- `cast_info` is NOT used here (Q28 has no cast_info join).
- `movie_keyword__movie_id` (offsets_only): `_idx/movie_keyword__movie_id__offsets.bin`.
- `complete_cast__movie_id` (offsets_only): `_idx/complete_cast__movie_id__offsets.bin`.
- `movie_info_idx__movie_id` (offsets_only): `_idx/movie_info_idx__movie_id__offsets.bin`.
- `movie_companies__movie_id` (offsets_only): `_idx/movie_companies__movie_id__offsets.bin`.
- `movie_info__movie_id` (offsets_only): `_idx/movie_info__movie_id__offsets.bin`.

Usage:
```cpp
int32_t lo = off[t_id], hi = off[t_id + 1];
for (int32_t r = lo; r < hi; ++r) { /* fact row r */ }
```

Rules: dict code 0 == NULL (matches `!=` literal naturally); never hardcode `[us]` code; varlen `.off`+`.dat`; index slot 0 = FK<1 count.
