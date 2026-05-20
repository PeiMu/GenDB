## SQL

```sql
SELECT MIN(cn.name) AS from_company,
       MIN(mc.note) AS production_note,
       MIN(t.title) AS movie_based_on_book
FROM company_name AS cn, company_type AS ct, keyword AS k, link_type AS lt,
     movie_companies AS mc, movie_keyword AS mk, movie_link AS ml, title AS t
WHERE cn.country_code != '[pl]'
  AND ct.kind != 'production companies' AND ct.kind IS NOT NULL
  AND k.keyword IN ('sequel','revenge','based-on-novel')
  AND mc.note IS NOT NULL
  AND t.production_year > 1950
  AND lt.id = ml.link_type_id AND ml.movie_id = t.id
  AND t.id = mk.movie_id AND mk.keyword_id = k.id
  AND t.id = mc.movie_id AND mc.company_type_id = ct.id
  AND mc.company_id = cn.id;
```

## Column Reference

### company_name.country_code (int16 dict)
Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`. Resolve `[pl]` → `pl_code`. Predicate `cc != pl_code && cc != 0`.

### company_name.name (varlen)
Files: `company_name/name.off|.dat`. No filter in Q11d; only used for MIN.

### company_name.id (dense PK)
Identity.

### company_type.kind (varlen)
Files: `company_type/kind.off|.dat`. Resolve `production companies` → `pc_id`; allow `ct_id ∈ {1..4} \ {pc_id}`.

### keyword.keyword (varlen)
Files: `keyword/keyword.off|.dat`. Resolve ids for `sequel`, `revenge`, `based-on-novel` → set `K`.

### link_type (structural join, no predicate)
Not filtered; join `lt.id = ml.link_type_id` is satisfied by any non-NULL link_type_id.

### movie_companies (movie_id/company_id/company_type_id/note)
Files: `movie_companies/movie_id.bin`, `company_id.bin`, `company_type_id.bin`, `note.off|.dat`.

### movie_keyword (movie_id/keyword_id)
Files: `movie_keyword/movie_id.bin`, `keyword_id.bin`.

### movie_link (movie_id/link_type_id)
Files: `movie_link/movie_id.bin`, `link_type_id.bin`.

### title (id/title/production_year)
Files: `title/id.bin` identity, `title/title.off|.dat`, `title/production_year.bin`.

## Table Stats

| Table | Rows | Sort |
|---|---|---|
| company_name | 234997 | id |
| company_type | 4 | id |
| keyword | 134170 | id |
| link_type | 18 | id |
| movie_companies | 2609129 | movie_id |
| movie_keyword | 4523930 | movie_id |
| movie_link | 29997 | movie_id |
| title | 2528312 | id |

## Query Analysis

Same shape as Q11c, but without the `cn.name LIKE` prefix filter. The most selective predicate becomes `mk.keyword_id ∈ K` (3 keywords out of 134170 → moderate, but movie_keyword has 4.5M rows).

Driver: `movie_link` (29997 rows) is the smallest movie-side table, and every result must have a movie_link row. Plan:

1. Resolve `pl_code`, `pc_id`, `K`.
2. Build `K_movies` = set of movie_ids by walking, for each k_id ∈ K, CSR `movie_keyword__keyword_id`. Result is small-to-moderate (a few hundred thousand).
3. Scan `movie_link.movie_id` once; for each row, candidate `mid` is qualifying iff `mid ∈ K_movies`. (29997 lookups into hashset.) Collect distinct `mid` candidates.
4. For each `mid`:
   - title row mid-1: require `production_year != INT32_MIN && py > 1950`.
   - Probe `movie_companies__movie_id` offsets: iterate mc rows, require `note` non-empty, `company_type_id != pc_id && != 0`. Then look up `company_name` row (cid-1): require `cc != pl_code && cc != 0`. Capture cn.name and mc.note for MINs.
5. Update MIN aggregates.

Alternative driver: scan movie_keyword (4.5M) filtering by `keyword_id ∈ K` → produce `mid` set, then probe ml existence. Pick whichever fits cache better; ml-driven is leaner.

MIN aggregation: byte-wise. cn.name has no filter, but is only captured when full join qualifies — still potentially many candidates; updating MIN is O(1) per candidate with `memcmp`.

LIKE notes: none in Q11d.

Selectivity: ~70% of company_name passes the country filter; movie_link is 29997; K_movies via CSR is the funnel.

## Indexes

### movie_keyword__keyword_id (CSR)
- `<storage>/_idx/movie_keyword__keyword_id__offsets.bin` (int32, 134172)
- `<storage>/_idx/movie_keyword__keyword_id__rowids.bin` (int32, 4523930)

```cpp
for (int32_t kid : K) {
    int32_t lo = mk_off[kid], hi = mk_off[kid+1];
    for (int32_t i = lo; i < hi; ++i) {
        int32_t mk_row = mk_rowids[i];
        K_movies.insert(mk_movie_id[mk_row]);
    }
}
```

### movie_link sequential scan (no index needed)
29997 int32 reads — cheaper than maintaining an extra structure.

### movie_companies__movie_id (offsets_only)
- `<storage>/_idx/movie_companies__movie_id__offsets.bin`
- Per qualifying mid, scan mc rows for note + ct_id predicates.

No need for `movie_companies__company_id` here because we drive from movie side.
