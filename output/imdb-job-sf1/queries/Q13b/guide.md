## SQL

```sql
SELECT MIN(cn.name) AS producing_company,
       MIN(miidx.info) AS rating,
       MIN(t.title) AS movie_about_winning
FROM company_name AS cn, company_type AS ct,
     info_type AS it, info_type AS it2,
     kind_type AS kt,
     movie_companies AS mc, movie_info AS mi, movie_info_idx AS miidx, title AS t
WHERE cn.country_code = '[us]'
  AND ct.kind = 'production companies'
  AND it.info = 'rating'
  AND it2.info = 'release dates'
  AND kt.kind = 'movie'
  AND t.title != ''
  AND (t.title LIKE '%Champion%' OR t.title LIKE '%Loser%')
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
Files: `title/id.bin` identity, `title/title.off|.dat`, `title/kind_id.bin`. Predicates: `kind_id == kt_id`, `title != ''` (off[i]<off[i+1]), and `title LIKE '%Champion%' OR LIKE '%Loser%'`.

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

Title.title LIKE `%Champion%` or `%Loser%` is the tightest predicate (~few thousand titles).

Driver: title scan.

Plan:

1. Resolve dims: `us_code`, `ct_id`, `it_id`, `it2_id`, `kt_id`.
2. Scan title once (2.5M rows): for each row i, let `mid = i+1`. Check `t_kind_id[i] == kt_id` AND `title.off[i] != title.off[i+1]` AND (`memmem` for `Champion` (8 bytes) OR `Loser` (5 bytes) in `[off[i], off[i+1])`). Length prune (need ≥5). Collect candidate mids `M`.
3. For each `mid ∈ M`:
   - Probe `movie_companies__movie_id` `[lo,hi)`: scan mc rows with `company_type_id == ct_id`. For each, load `company_id`, check `company_name.country_code[cid-1] == us_code`. Capture cn.name.
   - Probe `movie_info__movie_id` `[lo,hi)`: require at least one row with `info_type_id == it2_id`.
   - Probe `movie_info_idx__movie_id` `[lo,hi)`: scan rows with `info_type_id == it_id`. Capture miidx.info.
4. Update MIN(cn.name), MIN(miidx.info), MIN(t.title).

MIN aggregation: byte-wise.

LIKE notes: `%Champion%` (8 bytes) and `%Loser%` (5 bytes) — `memmem` on the title bytes. Apply length prune first.

Selectivities: title-LIKE+kind=movie → handful of thousands; subsequent joins are point-probes on offsets indexes.

## Indexes

### movie_companies__movie_id (offsets_only)
- `<storage>/_idx/movie_companies__movie_id__offsets.bin`
- `[lo,hi)` per mid; filter `company_type_id == ct_id`, lookup company_name.

### movie_info__movie_id (offsets_only)
- `<storage>/_idx/movie_info__movie_id__offsets.bin`
- Existence check for `info_type_id == it2_id`.

### movie_info_idx__movie_id (offsets_only)
- `<storage>/_idx/movie_info_idx__movie_id__offsets.bin`
- Filter `info_type_id == it_id`, read miidx.info.

Skipped: `title__kind_id` CSR (kind=movie is too wide compared to LIKE filter); `movie_companies__company_id` CSR (driver is title); info_type_id CSRs.
