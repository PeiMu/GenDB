# Q7b Guide

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
  AND n.name_pcode_cf LIKE 'D%'
  AND n.gender='m'
  AND pi.note ='Volker Boehm'
  AND t.production_year BETWEEN 1980 AND 1984
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
- Use: resolve `target_it_id` for `"mini biography"`.

### info_type.id (join key, int32_t)
- File: `info_type/id.bin`; identity.

### link_type.link (filter, varlen)
- Files: `link_type/link.off`, `link_type/link.dat`
- Row count: 18 (dense PK).
- Use: resolve `target_lt_id` for `"features"`.

### link_type.id (join key, int32_t)
- File: `link_type/id.bin`; identity.

### person_info.info_type_id (filter+join, int32_t)
- File: `person_info/info_type_id.bin`
- Row count: 2963664; sorted by person_id.
- Use: CSR `person_info__info_type_id` to enumerate the slice with `info_type_id == target_it_id`.

### person_info.note (filter, varlen)
- Files: `person_info/note.off`, `person_info/note.dat`
- Use: equality `"Volker Boehm"` (12 bytes); length prefilter then memcmp.

### person_info.person_id (join key, int32_t)
- File: `person_info/person_id.bin`; sorted by person_id.
- Use: each surviving pi row yields a `pid`.

### name.name_pcode_cf (filter, varlen)
- Files: `name/name_pcode_cf.off`, `name/name_pcode_cf.dat`
- Row count: 4167491 (dense PK).
- Use: `LIKE 'D%'` ⇒ `len > 0 && dat[off[pid-1]] == 'D'`.

### name.gender (filter, int8_t, dict-encoded)
- Files: `name/gender.bin`, `name/gender.dict.off`, `name/gender.dict.dat`
- Use: resolve `code_m` at runtime:
  ```cpp
  auto off = read_vec<int64_t>(store + "/name/gender.dict.off");
  std::string dat = read_file(store + "/name/gender.dict.dat");
  int8_t code_m = 0;
  for (size_t i = 0; i + 1 < off.size(); ++i) {
      std::string_view s(dat.data() + off[i], off[i+1] - off[i]);
      if (s == "m") { code_m = (int8_t)(i + 1); break; }
  }
  ```
  Test: `gender_bin[pid-1] == code_m`.

### name.name (output, varlen)
- Files: `name/name.off`, `name/name.dat`
- Use: MIN aggregate over surviving names.

### name.id (join key, int32_t)
- File: `name/id.bin`; identity.

### aka_name.person_id (join key, int32_t)
- File: `aka_name/person_id.bin`; sorted by person_id.
- Use: per pid, probe offsets_only `aka_name__person_id`.

### aka_name.name (filter, varlen)
- Files: `aka_name/name.off`, `aka_name/name.dat`
- Use: per aka row in pid range, `len > 0 && memmem(name, "a")`. aka_name sorted by person_id; varlen scanned in row order within range.

### cast_info.person_id (join key, int32_t)
- File: `cast_info/person_id.bin`
- Row count: 36244344; sorted by movie_id; use CSR `cast_info__person_id`.

### cast_info.movie_id (join key, int32_t)
- File: `cast_info/movie_id.bin`
- Use: per ci row, `t_id = ci_movie_id[ci_row]`.

### movie_link.linked_movie_id (join key, int32_t)
- File: `movie_link/linked_movie_id.bin`
- Row count: 29997; CSR index keyed by `linked_movie_id`.

### movie_link.link_type_id (filter+join, int32_t)
- File: `movie_link/link_type_id.bin`
- Use: per ml row, `ml_lt[r] == target_lt_id`.

### title.id (join key, int32_t)
- File: `title/id.bin`; dense PK 2528312.

### title.production_year (filter, int32_t)
- File: `title/production_year.bin`
- Use: `1980 <= production_year_bin[t_id-1] <= 1984 && != INT32_MIN`.

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
- Same join shape as Q7a, with stricter filters: `name_pcode_cf LIKE 'D%'`, `gender='m'` (no f-branch), year ∈ [1980,1984].
- Driver: pi rows with `info_type_id == target_it_id && note == 'Volker Boehm'` → pids (very few).
- Per pid: check `name_pcode_cf[pid-1]` starts with 'D'; check `gender_bin[pid-1] == code_m`.
- Aka existence: offsets_only `aka_name__person_id` range; scan names; require `%a%`.
- ci probe: CSR `cast_info__person_id` → t_ids.
- Per t_id: year ∈ [1980,1984] ~1%; CSR `movie_link__linked_movie_id` → ml rows; require `link_type_id == target_lt_id`.
- MIN aggregations: 2 outputs (n.name, t.title). 
- LIKE: `an.name LIKE '%a%'`, `n.name_pcode_cf LIKE 'D%'`.
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
- Never invent dictionary code values; load `name/gender.dict.*` at runtime.
- Never invent indexes not in build_indexes.cpp.
- varlen columns use `.off` + `.dat`, not `.bin`.
- aka_name is sorted by person_id → `aka_name__person_id` offsets-only index is for name→aka_name lookup; aka_name.name itself is a varlen column, scan in row order over the pid's range.
