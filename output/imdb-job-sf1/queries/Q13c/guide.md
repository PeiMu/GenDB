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
  AND (t.title LIKE 'Champion%' OR t.title LIKE 'Loser%')
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
Files: `movie_info/movie_id.bin`, `info_type_id.bin`, `info.off|.dat`. Existence with `info_type_id == it2_id`.

### movie_info_idx (movie_id/info_type_id/info)
Files: `movie_info_idx/movie_id.bin`, `info_type_id.bin`, `info.off|.dat`. Filter `info_type_id == it_id`, capture for MIN.

### title (id/title/kind_id)
Files: `title/id.bin` identity, `title/title.off|.dat`, `title/kind_id.bin`. Predicates: kind=movie, title non-empty, title LIKE `Champion%` OR `Loser%` (anchored prefix).

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

Same as Q13b but anchored-prefix LIKEs (`Champion%`, `Loser%`) — even tighter title filter.

Driver: title scan. Use `memcmp` against `"Champion"` (8 bytes) and `"Loser"` (5 bytes) at the start of the title bytes.

Plan:

1. Resolve dims: `us_code`, `ct_id`, `it_id`, `it2_id`, `kt_id`.
2. Scan title: for each i, let mid=i+1. Test `kind_id==kt_id`, `title.off[i] < title.off[i+1]`, AND prefix match. Anchored prefix is cheap: `len >= 8 && memcmp(dat+off[i], "Champion", 8) == 0` OR `len >= 5 && memcmp(dat+off[i], "Loser", 5) == 0`. Collect mids `M`.
3. For each `mid ∈ M`:
   - Probe `movie_companies__movie_id` `[lo,hi)`: scan with `company_type_id == ct_id`; for each, load `company_id`, check `company_name.country_code[cid-1] == us_code`. Capture cn.name.
   - Probe `movie_info__movie_id` `[lo,hi)`: existence with `info_type_id == it2_id`.
   - Probe `movie_info_idx__movie_id` `[lo,hi)`: filter `info_type_id == it_id`; capture miidx.info.
4. Update MIN(cn.name), MIN(miidx.info), MIN(t.title).

MIN aggregation: byte-wise.

LIKE notes: anchored prefix — direct `memcmp`. No `memmem` needed.

Selectivities: anchored prefix is more selective than `%Champion%` in Q13b — title candidate set is smaller (~hundreds).

## Indexes

### movie_companies__movie_id (offsets_only)
- `<storage>/_idx/movie_companies__movie_id__offsets.bin`

```cpp
int32_t lo = mc_off[mid], hi = mc_off[mid+1];
for (int32_t r = lo; r < hi; ++r) {
    if (mc_company_type_id[r] != ct_id) continue;
    int32_t cid = mc_company_id[r];
    if (cn_cc[cid-1] != us_code) continue;
    /* qualifies — read cn_name varlen for MIN */
}
```

### movie_info__movie_id (offsets_only)
- `<storage>/_idx/movie_info__movie_id__offsets.bin`. Existence check on it2_id.

### movie_info_idx__movie_id (offsets_only)
- `<storage>/_idx/movie_info_idx__movie_id__offsets.bin`. Filter on it_id, read miidx.info.

Skipped: `title__kind_id` CSR (kind=movie is wide); CSRs on info_type_id; `movie_companies__company_id` CSR.
