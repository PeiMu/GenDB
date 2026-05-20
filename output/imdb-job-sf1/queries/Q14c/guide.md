## SQL

```sql
SELECT MIN(mi_idx.info) AS rating,
       MIN(t.title) AS north_european_dark_production
FROM info_type AS it1, info_type AS it2, keyword AS k, kind_type AS kt,
     movie_info AS mi, movie_info_idx AS mi_idx, movie_keyword AS mk, title AS t
WHERE it1.info = 'countries'
  AND it2.info = 'rating'
  AND k.keyword IS NOT NULL
  AND k.keyword IN ('murder','murder-in-title','blood','violence')
  AND kt.kind IN ('movie','episode')
  AND mi.info IN ('Sweden','Norway','Germany','Denmark','Swedish','Danish','Norwegian','German','USA','American')
  AND mi_idx.info < '8.5'
  AND t.production_year > 2005
  AND kt.id = t.kind_id
  AND t.id = mi.movie_id AND t.id = mk.movie_id AND t.id = mi_idx.movie_id
  AND k.id = mk.keyword_id
  AND it1.id = mi.info_type_id AND it2.id = mi_idx.info_type_id;
```

## Column Reference

### info_type.info (varlen)
Files: `info_type/info.off`, `info_type/info.dat`. Resolve `it1_id` ('countries') and `it2_id` ('rating').

### kind_type.kind (varlen)
Files: `kind_type/kind.off`, `kind_type/kind.dat`. Resolve set `kt_ids = {id of 'movie', id of 'episode'}` (small set of 2).

### keyword.keyword (varlen)
Files: `keyword/keyword.off`, `keyword/keyword.dat`. Build `k_ids` set from the 4 IN values. NULL check `IS NOT NULL` corresponds to non-empty entry (`off[i+1]-off[i] > 0`); in practice all IDs in keyword have non-empty text.

### title.kind_id, title.production_year, title.title (int32/int32/varlen)
Files: `title/kind_id.bin`, `title/production_year.bin` (NULL=INT32_MIN), `title/title.off`, `title/title.dat`. Filter `production_year[r] != INT32_MIN && production_year[r] > 2005 && kt_ids.contains(kind_id[r])`.

### movie_keyword.keyword_id, movie_keyword.movie_id (int32)
Files: `movie_keyword/keyword_id.bin`, `movie_keyword/movie_id.bin` (4523930). Sorted by movie_id.

### movie_info.info_type_id, movie_info.info, movie_info.movie_id
Files: `movie_info/info_type_id.bin`, `movie_info/info.off`, `movie_info/info.dat`, `movie_info/movie_id.bin` (14835720). Build hash set of 10 country names (note `'Danish'` here, not `'Denish'` as in Q14a).

### movie_info_idx.info_type_id, movie_info_idx.info, movie_info_idx.movie_id
Files: `movie_info_idx/*` (1380035). STRING compare `lex_lt(info, "8.5")`.
```cpp
auto lex_lt = [](std::string_view a, std::string_view b){
    size_t n = std::min(a.size(), b.size());
    int c = memcmp(a.data(), b.data(), n);
    return c < 0 || (c==0 && a.size()<b.size());
};
```

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| info_type | 113 | id | dense-PK |
| kind_type | 7 | id | dense-PK |
| keyword | 134,170 | id | dense-PK |
| title | 2,528,312 | id | dense-PK |
| movie_keyword | 4,523,930 | movie_id | offsets_only on movie_id |
| movie_info | 14,835,720 | movie_id | offsets_only on movie_id |
| movie_info_idx | 1,380,035 | movie_id | offsets_only on movie_id |

## Query Analysis

Same star as Q14a; broader filters:
- `kt.kind IN ('movie','episode')` (2 kind ids)
- `production_year > 2005` (more years)
- `mi.info` set differs in spelling.

MIN: `MIN(mi_idx.info)` lex, `MIN(t.title)` lex.

Driver: title-driven scan over 2.5M titles.
1. Resolve `it1_id`, `it2_id`, `kt_ids` (2 ids), `k_ids` (4 ids).
2. For each title r: check kind_id ∈ kt_ids and production_year filter.
3. Use `movie_keyword__movie_id` to find any mk row with `keyword_id ∈ k_ids`.
4. Use `movie_info__movie_id` to find any mi row with `info_type_id==it1_id` and `info` ∈ country set.
5. Use `movie_info_idx__movie_id` to scan mi_idx rows with `info_type_id==it2_id` and `lex_lt(info,"8.5")`. Update MINs.

Selectivities:
- production_year > 2005 → ~25% of titles.
- kind IN(movie,episode) → ~25% of titles.
- mi.info ∈ countries + it1='countries' → small.
- mi_idx.info < '8.5' → majority of ratings.

LIKE notes: none in Q14c.

## Indexes

### movie_keyword__movie_id (offsets_only)
File: `_idx/movie_keyword__movie_id__offsets.bin` (int32, 2528314).
```cpp
auto mkmid = read_vec<int32_t>(store + "/_idx/movie_keyword__movie_id__offsets.bin");
int32_t lo = mkmid[mv], hi = mkmid[mv+1];
```

### movie_info__movie_id (offsets_only)
File: `_idx/movie_info__movie_id__offsets.bin` (int32, 2528314).

### movie_info_idx__movie_id (offsets_only)
File: `_idx/movie_info_idx__movie_id__offsets.bin` (int32, 2528314).
