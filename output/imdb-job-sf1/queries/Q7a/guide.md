# Q7a Guide

## SQL
```sql
SELECT MIN(n.name) AS of_person,
       MIN(t.title) AS biography_movie
FROM aka_name AS an,
     cast_info AS ci,
     info_type AS it,
     link_type AS lt,
     movie_link AS ml,
     name AS n,
     person_info AS pi,
     title AS t
WHERE an.name LIKE '%a%'
  AND it.info ='mini biography'
  AND lt.link ='features'
  AND n.name_pcode_cf BETWEEN 'A' AND 'F'
  AND (n.gender='m'
       OR (n.gender = 'f'
           AND n.name LIKE 'B%'))
  AND pi.note ='Volker Boehm'
  AND t.production_year BETWEEN 1980 AND 1995
  AND n.id = an.person_id
  AND n.id = pi.person_id
  AND ci.person_id = n.id
  AND t.id = ci.movie_id
  AND ml.linked_movie_id = t.id
  AND lt.id = ml.link_type_id
  AND it.id = pi.info_type_id
  AND pi.person_id = an.person_id
  AND pi.person_id = ci.person_id
  AND an.person_id = ci.person_id
  AND ci.movie_id = ml.linked_movie_id;
```

## Column Reference

### info_type.info (filter, varlen)
- Files: `info_type/info.off`, `info_type/info.dat`
- Row count: 113 (dense PK).
- Use: scan once for `target_it_id` where bytes == `"mini biography"`.

### info_type.id (join key, int32_t)
- File: `info_type/id.bin`; identity.

### link_type.link (filter, varlen)
- Files: `link_type/link.off`, `link_type/link.dat`
- Row count: 18 (dense PK).
- Use: scan once for `target_lt_id` where bytes == `"features"`.

### link_type.id (join key, int32_t)
- File: `link_type/id.bin`; identity.

### person_info.info_type_id (filter+join, int32_t)
- File: `person_info/info_type_id.bin`
- Row count: 2963664; sorted by person_id.
- Use: prefer CSR `person_info__info_type_id` to enumerate the small slice with `info_type_id == target_it_id` (~few thousand rows).

### person_info.note (filter, varlen)
- Files: `person_info/note.off`, `person_info/note.dat`
- Use: equality `"Volker Boehm"`; length prefilter (12 bytes), then memcmp.

### person_info.person_id (join key, int32_t)
- File: `person_info/person_id.bin`; sorted by person_id.
- Use: after pi filter, surviving rows yield candidate `pid`s.

### name.name_pcode_cf (filter, varlen)
- Files: `name/name_pcode_cf.off`, `name/name_pcode_cf.dat`
- Row count: 4167491 (dense PK).
- Use: `'A' <= s <= 'F'` lexicographic; per pid: read at row `pid-1`, compare. Pre-scan once to build allowed-pid set is overkill; lookup per candidate pid is cheaper since pi filter is selective.

### name.gender (filter, int8_t, dict-encoded)
- Files: `name/gender.bin`, `name/gender.dict.off`, `name/gender.dict.dat`
- Use: resolve codes at runtime:
  ```cpp
  auto off = read_vec<int64_t>(store + "/name/gender.dict.off");
  std::string dat = read_file(store + "/name/gender.dict.dat");
  int8_t code_m = 0, code_f = 0;
  for (size_t i = 0; i + 1 < off.size(); ++i) {
      std::string_view s(dat.data() + off[i], off[i+1] - off[i]);
      if (s == "m") code_m = (int8_t)(i + 1);
      if (s == "f") code_f = (int8_t)(i + 1);
  }
  ```
  Then test: `g = gender_bin[pid-1]; (g == code_m) || (g == code_f && name_LIKE_B(pid))`.

### name.name (filter+output, varlen)
- Files: `name/name.off`, `name/name.dat`
- Use: for `f` branch, `LIKE 'B%'` ⇒ first byte == 'B'. MIN aggregate over surviving names.

### name.id (join key, int32_t)
- File: `name/id.bin`; identity. `n_id = pid`.

### aka_name.person_id (join key, int32_t)
- File: `aka_name/person_id.bin`; sorted by person_id.
- Use: for each candidate pid, probe offsets_only `aka_name__person_id` for aka rows; if range empty ⇒ skip.

### aka_name.name (filter, varlen)
- Files: `aka_name/name.off`, `aka_name/name.dat`
- Use: per aka row, `len > 0 && memmem(name, "a")`. aka_name is sorted by person_id → `aka_name__person_id` offsets-only index is for name→aka_name lookup; aka_name.name itself is a varlen column, scan in row order over the pid's range.

### cast_info.person_id (join key, int32_t)
- File: `cast_info/person_id.bin`
- Row count: 36244344; sorted by movie_id, so use CSR `cast_info__person_id` to enumerate ci rows per pid.

### cast_info.movie_id (join key, int32_t)
- File: `cast_info/movie_id.bin`
- Use: per ci row, derive `t_id = ci_movie_id[ci_row]`.

### movie_link.linked_movie_id (join key, int32_t)
- File: `movie_link/linked_movie_id.bin`
- Row count: 29997; sorted by movie_id (not linked_movie_id).
- Use: CSR `movie_link__linked_movie_id` to find ml rows where `linked_movie_id == t_id`.

### movie_link.link_type_id (filter+join, int32_t)
- File: `movie_link/link_type_id.bin`
- Use: per ml row, `ml_lt[r] == target_lt_id`.

### title.id (join key, int32_t)
- File: `title/id.bin`; dense PK 2528312.

### title.production_year (filter, int32_t)
- File: `title/production_year.bin`
- Use: `1980 <= production_year_bin[t_id-1] <= 1995 && != INT32_MIN`.

### title.title (output, varlen)
- Files: `title/title.off`, `title/title.dat`
- Use: MIN aggregate.

## Table Stats
| Table | Rows | Role | Sort Order | Block Size |
|---|---|---|---|---|
| info_type | 113 | dim | id | n/a |
| link_type | 18 | dim | id | n/a |
| person_info | 2963664 | driver | person_id | 100000 |
| name | 4167491 | dim (filter+proj) | id | 100000 |
| aka_name | 901343 | semi-join filter | person_id | 100000 |
| cast_info | 36244344 | fact | movie_id | 200000 |
| movie_link | 29997 | fact | movie_id | 50000 |
| title | 2528312 | fact | id | 100000 |

## Query Analysis
- Join graph: hub on `n.id == an.person_id == pi.person_id == ci.person_id`; movie hub on `t.id == ci.movie_id == ml.linked_movie_id`. `it.id=pi.info_type_id`, `lt.id=ml.link_type_id`.
- Driver: start from `person_info` filtered by `info_type_id == target_it_id` AND `note == 'Volker Boehm'`. This is extremely selective — typically a small handful of pids.
- For each surviving pid:
  1. Check `name.name_pcode_cf[pid-1]` ∈ ['A','F'].
  2. Check gender predicate (using dict-resolved codes).
  3. Probe `aka_name__person_id` for any aka row whose name contains 'a' (length>0 + memmem). If none ⇒ skip.
  4. Probe CSR `cast_info__person_id` to enumerate ci rows for pid → list of t_ids.
  5. For each t_id: year ∈ [1980,1995]; then CSR `movie_link__linked_movie_id` → ml rows with `link_type_id == target_lt_id`. If any ⇒ tuple survives.
- Filter selectivities: pi.note='Volker Boehm' ≪ 1%; it.info=1; lt.link=1; year range ~5%; name_pcode_cf 'A'..'F' ~25%; gender filter ~70%.
- MIN aggregations: 2 outputs (n.name, t.title).
- LIKE patterns: `an.name LIKE '%a%'`, optional `n.name LIKE 'B%'` under gender='f'.
- IN: none.
- Output projection: read t.title and n.name varlen only for surviving rows.

## Indexes
- `person_info__info_type_id` (CSR)
  - Files: `_idx/person_info__info_type_id__offsets.bin` (int32 length `115`), `_idx/person_info__info_type_id__rowids.bin` (int32 length 2963664).
  - Empty: `off[v]==off[v+1]`.
  - Access:
    ```cpp
    int32_t lo = pi_it_off[target_it_id], hi = pi_it_off[target_it_id + 1];
    for (int32_t j = lo; j < hi; ++j) {
        int32_t pi_row = pi_it_rowids[j];
        int32_t pid = pi_person[pi_row];
        /* test pi_note[pi_row] == 'Volker Boehm' */
    }
    ```
- `aka_name__person_id` (offsets_only)
  - File: `_idx/aka_name__person_id__offsets.bin`, int32 length `4167493`.
  - Access:
    ```cpp
    int32_t lo = an_off[pid], hi = an_off[pid + 1];
    for (int32_t r = lo; r < hi; ++r) { /* aka_name row r */ }
    ```
- `cast_info__person_id` (CSR)
  - Files: `_idx/cast_info__person_id__offsets.bin` (int32 length `4167493`), `_idx/cast_info__person_id__rowids.bin` (int32 length 36244344).
  - Access:
    ```cpp
    int32_t lo = ci_p_off[pid], hi = ci_p_off[pid + 1];
    for (int32_t j = lo; j < hi; ++j) {
        int32_t ci_row = ci_p_rowids[j];
        int32_t t_id  = ci_movie[ci_row];
    }
    ```
- `movie_link__linked_movie_id` (CSR)
  - Files: `_idx/movie_link__linked_movie_id__offsets.bin` (int32 length `2528314`), `_idx/movie_link__linked_movie_id__rowids.bin` (int32 length 29997).
  - Access:
    ```cpp
    int32_t lo = ml_lm_off[t_id], hi = ml_lm_off[t_id + 1];
    for (int32_t j = lo; j < hi; ++j) {
        int32_t ml_row = ml_lm_rowids[j];
        if (ml_link_type[ml_row] == target_lt_id) { /* matched */ }
    }
    ```

## Rules
- Reference shared context for verbatim build code; only quote query-time usage.
- Never invent dictionary code values; load `name/gender.dict.*` at runtime to resolve 'm','f' codes.
- Never invent indexes not in build_indexes.cpp.
- varlen columns use `.off` + `.dat`, not `.bin`.
- aka_name is sorted by person_id → `aka_name__person_id` offsets-only index is for name→aka_name lookup; aka_name.name itself is a varlen column, scan in row order over the pid's range.
