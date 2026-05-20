## SQL

```sql
SELECT MIN(cn.name) AS producing_company,
       MIN(miidx.info) AS rating,
       MIN(t.title) AS movie
FROM company_name AS cn, company_type AS ct,
     info_type AS it, info_type AS it2,
     kind_type AS kt,
     movie_companies AS mc, movie_info AS mi, movie_info_idx AS miidx, title AS t
WHERE cn.country_code = '[us]'
  AND ct.kind = 'production companies'
  AND it.info = 'rating'
  AND it2.info = 'release dates'
  AND kt.kind = 'movie'
  AND mi.movie_id = t.id AND it2.id = mi.info_type_id
  AND kt.id = t.kind_id
  AND mc.movie_id = t.id AND cn.id = mc.company_id AND ct.id = mc.company_type_id
  AND miidx.movie_id = t.id AND it.id = miidx.info_type_id;
```

## Column Reference

### company_name.country_code (int16 dict)
Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`. Resolve `[us]` → `us_code`.

### company_name.name (varlen)
Files: `company_name/name.off|.dat`. Used in MIN.

### company_name.id (dense PK)
Identity.

### company_type.kind (varlen)
Files: `company_type/kind.off|.dat`. Resolve `production companies` → `ct_id`.

### info_type.info (varlen)
Files: `info_type/info.off|.dat`. Resolve `rating` → `it_id`; `release dates` → `it2_id`.

### kind_type.kind (varlen)
Files: `kind_type/kind.off|.dat`. Resolve `movie` → `kt_id`.

### movie_companies (movie_id/company_id/company_type_id)
Files: `movie_companies/movie_id.bin`, `company_id.bin`, `company_type_id.bin`.

### movie_info (movie_id/info_type_id/info)
Files: `movie_info/movie_id.bin`, `info_type_id.bin`, `info.off|.dat`. Predicate `info_type_id == it2_id`; existence required.

### movie_info_idx (movie_id/info_type_id/info)
Files: `movie_info_idx/movie_id.bin`, `info_type_id.bin`, `info.off|.dat`. Predicate `info_type_id == it_id`; capture for MIN.

### title (id/title/kind_id)
Files: `title/id.bin` identity, `title/title.off|.dat`, `title/kind_id.bin`. Only predicate: `kind_id == kt_id`. No title-pattern filter and no production_year filter.

## Table Stats

| Table | Rows | Sort |
|---|---|---|
| company_name | 234997 | id |
| company_type | 4 | id |
| info_type | 113 | id |
| kind_type | 7 | id |
| movie_companies | 2609129 | movie_id |
| movie_info | 14835720 | movie_id |
| movie_info_idx | 1380035 | movie_id |
| title | 2528312 | id |

## Query Analysis

Q13d has no title text or year filter, so title is wide. Selectivity comes from `cn.country_code = [us]` + `ct.kind = production companies`.

Driver: company-side. US production-companies-mc edges are the funnel.

Plan:

1. Resolve dims: `us_code`, `ct_id`, `it_id`, `it2_id`, `kt_id`.
2. Scan `company_name.country_code` (234997 int16): collect `cn_us` = ids where `cc == us_code`. (~30–40k.)
3. For each `cn_id ∈ cn_us`, use CSR `movie_companies__company_id` to enumerate mc rows. Filter `company_type_id == ct_id`. Capture mid and mc_row → cn.name for MIN.
4. For each `mid`:
   - title row mid-1: require `kind_id == kt_id`.
   - Probe `movie_info__movie_id` `[lo,hi)`: existence with `info_type_id == it2_id`.
   - Probe `movie_info_idx__movie_id` `[lo,hi)`: filter rows with `info_type_id == it_id`; capture miidx.info.
5. Update MIN(cn.name), MIN(miidx.info), MIN(t.title).

MIN aggregation: byte-wise. Many qualifying tuples expected — keep three string_view best values.

LIKE notes: none.

Selectivities: us_code matches ~15% of company_name → ~35k cn_ids. CSR expansion yields lots of mc rows; ct filter cuts to ~25%. kind=movie keeps most. Heavy fan-out compared to Q13b/c.

## Indexes

### movie_companies__company_id (CSR)
- `<storage>/_idx/movie_companies__company_id__offsets.bin` (int32, 234999)
- `<storage>/_idx/movie_companies__company_id__rowids.bin` (int32, 2609129)

```cpp
for (int32_t cn_id : cn_us) {
    int32_t lo = mc_cid_off[cn_id], hi = mc_cid_off[cn_id+1];
    for (int32_t k = lo; k < hi; ++k) {
        int32_t mc_row = mc_cid_rowids[k];
        if (mc_company_type_id[mc_row] != ct_id) continue;
        int32_t mid = mc_movie_id[mc_row];
        if (t_kind_id[mid-1] != kt_id) continue;
        /* probe mi (existence), miidx (capture info) */
    }
}
```

### movie_info__movie_id (offsets_only)
- `<storage>/_idx/movie_info__movie_id__offsets.bin`. Existence with `info_type_id == it2_id`.

### movie_info_idx__movie_id (offsets_only)
- `<storage>/_idx/movie_info_idx__movie_id__offsets.bin`. Filter on it_id, read miidx.info.

Skipped: `title__kind_id` CSR (kind=movie too wide); info_type_id CSRs (driving from companies is already focused).
