#!/usr/bin/env python3
"""Generate per-query guide.md files that exactly match build_indexes.cpp and ingest.cpp.

Authoritative facts (from build_indexes.cpp / ingest.cpp):
  * Index directory: <storage>/indexes/  (NOT _idx/)
  * Primary CSR file: indexes/<table>__<sortcol>__offsets.bin (uint64[max+2])
  * Aux CSR files:    indexes/<table>__<col>__offsets.bin (uint64[max+2])
                    + indexes/<table>__<col>__rowids.bin  (int32[N_child])
  * PK pos file:      indexes/<table>__id__pos.bin (int32[max_id+2], -1 = absent)
  * Varlen files:     <table>/<col>.offsets.bin (uint64[N+1]) + <table>/<col>.data.bin
  * Fixed int32 cols: <table>/<col>.bin (int32[N])
  * Fixed char1 cols: <table>/<col>.bin (uint8[N], 0 = NULL)
  * Row count:        <table>/__row_count.bin (uint64)

NO hash functions are used: all index probes are direct-address into dense int32/uint64 arrays
keyed by the dimension id itself.

NO dictionary encoding is applied to any column — all text columns (incl. name.gender as char1,
company_name.country_code as varlen) are stored raw.

NO id-is-dense assumption: pk_pos_dense arrays are built explicitly to map id -> row position.
"""

import os, re, json, sys

OUTDIR = "/home/pei/Project/GenDB/output/imdb-job-sf1/queries"

# --- Table metadata (rows, sort, block, columns) ---
TABLES = {
    "title":           {"rows":2528312, "role":"dimension(PK)+driver", "sort":"id",        "block":100000},
    "name":            {"rows":4167491, "role":"dimension(PK)",         "sort":"id",        "block":100000},
    "char_name":       {"rows":3140339, "role":"dimension(PK)",         "sort":"id",        "block":100000},
    "company_name":    {"rows":234997,  "role":"dimension(PK)",         "sort":"id",        "block":50000},
    "keyword":         {"rows":134170,  "role":"dimension(PK)",         "sort":"id",        "block":50000},
    "info_type":       {"rows":113,     "role":"dimension(PK)",         "sort":"id",        "block":113},
    "link_type":       {"rows":18,      "role":"dimension(PK)",         "sort":"id",        "block":18},
    "role_type":       {"rows":12,      "role":"dimension(PK)",         "sort":"id",        "block":12},
    "kind_type":       {"rows":7,       "role":"dimension(PK)",         "sort":"id",        "block":7},
    "company_type":    {"rows":4,       "role":"dimension(PK)",         "sort":"id",        "block":4},
    "comp_cast_type":  {"rows":4,       "role":"dimension(PK)",         "sort":"id",        "block":4},
    "aka_name":        {"rows":901343,  "role":"fact",                  "sort":"person_id", "block":50000},
    "aka_title":       {"rows":361472,  "role":"fact",                  "sort":"movie_id",  "block":50000},
    "person_info":     {"rows":2963664, "role":"fact",                  "sort":"person_id", "block":100000},
    "complete_cast":   {"rows":135086,  "role":"fact",                  "sort":"movie_id",  "block":50000},
    "movie_link":      {"rows":29997,   "role":"fact",                  "sort":"movie_id",  "block":30000},
    "movie_companies": {"rows":2609129, "role":"fact",                  "sort":"movie_id",  "block":100000},
    "movie_keyword":   {"rows":4523930, "role":"fact",                  "sort":"movie_id",  "block":100000},
    "movie_info_idx":  {"rows":1380035, "role":"fact",                  "sort":"movie_id",  "block":100000},
    "movie_info":      {"rows":14835720,"role":"fact",                  "sort":"movie_id",  "block":200000},
    "cast_info":       {"rows":36244344,"role":"fact",                  "sort":"movie_id",  "block":500000},
}

# col_kind: "int" | "intN" (nullable, -1 sentinel) | "varlen" | "char1"
COLS = {
    "title":           {"id":"int", "title":"varlen", "kind_id":"int", "production_year":"intN", "episode_nr":"intN"},
    "name":            {"id":"int", "name":"varlen", "gender":"char1", "name_pcode_cf":"varlen"},
    "char_name":       {"id":"int", "name":"varlen"},
    "company_name":    {"id":"int", "name":"varlen", "country_code":"varlen"},
    "keyword":         {"id":"int", "keyword":"varlen"},
    "info_type":       {"id":"int", "info":"varlen"},
    "link_type":       {"id":"int", "link":"varlen"},
    "role_type":       {"id":"int", "role":"varlen"},
    "kind_type":       {"id":"int", "kind":"varlen"},
    "company_type":    {"id":"int", "kind":"varlen"},
    "comp_cast_type":  {"id":"int", "kind":"varlen"},
    "aka_name":        {"person_id":"int", "name":"varlen"},
    "aka_title":       {"movie_id":"int", "title":"varlen"},
    "person_info":     {"person_id":"int", "info_type_id":"int", "info":"varlen", "note":"varlen"},
    "complete_cast":   {"movie_id":"intN", "subject_id":"int", "status_id":"int"},
    "movie_link":      {"movie_id":"int", "linked_movie_id":"int", "link_type_id":"int"},
    "movie_companies": {"movie_id":"int", "company_id":"int", "company_type_id":"int", "note":"varlen"},
    "movie_keyword":   {"movie_id":"int", "keyword_id":"int"},
    "movie_info_idx":  {"movie_id":"int", "info_type_id":"int", "info":"varlen"},
    "movie_info":      {"movie_id":"int", "info_type_id":"int", "info":"varlen", "note":"varlen"},
    "cast_info":       {"person_id":"int", "movie_id":"int", "person_role_id":"intN", "note":"varlen", "role_id":"int"},
}

# Indexes built by build_indexes.cpp
# PK pos indexes (one per dimension PK table)
PK_POS = ["title","name","char_name","company_name","keyword","info_type","link_type",
          "role_type","kind_type","company_type","comp_cast_type"]
# Primary CSR: (table, sort_col) — fact tables are physically sorted by sort_col
PRIMARY_CSR = [
    ("aka_name","person_id"),
    ("aka_title","movie_id"),
    ("person_info","person_id"),
    ("complete_cast","movie_id"),
    ("movie_link","movie_id"),
    ("movie_companies","movie_id"),
    ("movie_keyword","movie_id"),
    ("movie_info_idx","movie_id"),
    ("movie_info","movie_id"),
    ("cast_info","movie_id"),
]
# Aux CSR: (table, col)
AUX_CSR = [
    ("movie_link","linked_movie_id"),
    ("movie_link","link_type_id"),
    ("movie_companies","company_id"),
    ("movie_keyword","keyword_id"),
    ("movie_info_idx","info_type_id"),
    ("movie_info","info_type_id"),
    ("cast_info","person_id"),
    ("cast_info","role_id"),
]

# max-id (parent dimension max) for each FK column — used in offsets size annotation
MAX_ID = {
    "movie_id": 2528312,
    "person_id": 4167491,
    "linked_movie_id": 2528312,
    "link_type_id": 18,
    "company_id": 234997,
    "keyword_id": 134170,
    "info_type_id": 113,
    "role_id": 12,
    "company_type_id": 4,
    "kind_id": 7,
    "subject_id": 4,
    "status_id": 4,
    "person_role_id": 3140339,
}

# alias -> table mapping rules (from JOB convention used across all 113 queries)
ALIAS_MAP_RULES = {
    "ct": "company_type",
    "it": "info_type", "it1": "info_type", "it2": "info_type", "it3": "info_type",
    "kt": "kind_type", "kt1": "kind_type", "kt2": "kind_type",
    "lt": "link_type",
    "rt": "role_type",
    "k": "keyword",
    "t": "title", "t1": "title", "t2": "title",
    "n": "name", "n1": "name",
    "chn": "char_name",
    "cn": "company_name", "cn1": "company_name", "cn2": "company_name",
    "an": "aka_name", "an1": "aka_name", "a1": "aka_name",
    "at1": "aka_title",
    "mc": "movie_companies", "mc1": "movie_companies", "mc2": "movie_companies",
    "mi": "movie_info",
    "mi_idx": "movie_info_idx", "mi_idx1": "movie_info_idx", "mi_idx2": "movie_info_idx",
    "miidx": "movie_info_idx",
    "mk": "movie_keyword",
    "ml": "movie_link",
    "cc": "complete_cast",
    "cct1": "comp_cast_type", "cct2": "comp_cast_type",
    "ci": "cast_info",
    "pi": "person_info",
}

def parse_sql(sql):
    """Extract aliases, predicates, and joins from a query SQL string."""
    # Find FROM ... WHERE
    m = re.search(r'\bFROM\b(.*?)\bWHERE\b', sql, re.IGNORECASE|re.DOTALL)
    from_part = m.group(1) if m else ""
    # alias mapping
    alias_to_table = {}
    for am in re.finditer(r'(\w+)\s+AS\s+(\w+)', from_part, re.IGNORECASE):
        tbl, al = am.group(1).lower(), am.group(2).lower()
        if tbl in TABLES:
            alias_to_table[al] = tbl
    where_part = sql.split("WHERE",1)[1] if "WHERE" in sql.upper() else ""
    # Find select columns (for projections)
    sel_m = re.search(r'SELECT(.*?)FROM', sql, re.IGNORECASE|re.DOTALL)
    sel_text = sel_m.group(1) if sel_m else ""
    proj_cols = set()
    for pm in re.finditer(r'\b(\w+)\.(\w+)\b', sel_text):
        proj_cols.add((pm.group(1).lower(), pm.group(2).lower()))
    # Find all alias.col refs in WHERE
    where_cols = set()
    for pm in re.finditer(r'\b(\w+)\.(\w+)\b', where_part):
        where_cols.add((pm.group(1).lower(), pm.group(2).lower()))
    # Identify joins: alias.col = alias.col
    joins = []
    for jm in re.finditer(r'(\w+)\.(\w+)\s*=\s*(\w+)\.(\w+)', where_part):
        a,b,c,d = [g.lower() for g in jm.groups()]
        if a in alias_to_table and c in alias_to_table:
            joins.append(((a,b),(c,d)))
    # Identify filters: predicates on alias.col that are not joins
    # we re-scan WHERE for fragments containing alias.col but exclude join equalities
    filters = []  # list of (alias, col, predicate_text)
    # Split WHERE by AND keeping parens grouping shallowly
    where_clean = where_part.rstrip(';').strip()
    # Use a simple AND splitter (queries don't use nested AND/OR much beyond what we need)
    # We'll scan top-level AND tokens.
    def split_top_and(s):
        out = []; depth = 0; cur = ""; pending_between = 0
        i = 0
        while i < len(s):
            ch = s[i]
            if ch == '(':
                depth += 1; cur += ch
            elif ch == ')':
                depth -= 1; cur += ch
            elif depth == 0 and s[i:i+8].upper() == 'BETWEEN ':
                pending_between = 1; cur += s[i:i+8]; i += 8; continue
            elif depth == 0 and s[i:i+5].upper() == ' AND ' and pending_between:
                # Consume the AND that belongs to the BETWEEN range
                pending_between = 0; cur += s[i:i+5]; i += 5; continue
            elif depth == 0 and s[i:i+5].upper() == ' AND ':
                out.append(cur.strip()); cur = ""; i += 5; continue
            elif depth == 0 and s[i:i+4].upper() == 'AND ' and not cur.strip():
                # leading AND
                cur = ""; i += 4; continue
            else:
                cur += ch
            i += 1
        if cur.strip(): out.append(cur.strip())
        return out
    clauses = split_top_and(where_clean)
    for cl in clauses:
        cl_stripped = cl.rstrip(';').strip()
        # Check if this clause is purely a single equality alias.col = alias.col (join)
        if re.fullmatch(r'\s*\w+\.\w+\s*=\s*\w+\.\w+\s*', cl_stripped):
            continue
        # collect alias.col refs in this clause
        refs = re.findall(r'\b(\w+)\.(\w+)\b', cl)
        # uniq
        seen = []
        for a,c in refs:
            if (a,c) not in seen: seen.append((a,c))
        # attach predicate to first alias.col mentioned (single-table predicate most common)
        if seen:
            a0, c0 = seen[0]
            if a0 in alias_to_table:
                filters.append((a0, c0, cl))
    return alias_to_table, joins, filters, proj_cols, where_cols

def col_file_block(table, col):
    kind = COLS[table][col]
    rows = TABLES[table]["rows"]
    if kind == "int":
        return f"`{table}/{col}.bin` — int32[{rows}]"
    if kind == "intN":
        return f"`{table}/{col}.bin` — int32[{rows}] (NULL = -1)"
    if kind == "char1":
        return f"`{table}/{col}.bin` — uint8[{rows}] (NULL = 0; values 'm'=0x6D, 'f'=0x66)"
    if kind == "varlen":
        return (f"`{table}/{col}.offsets.bin` — uint64[{rows+1}]\n"
                f"  - `{table}/{col}.data.bin` — raw bytes; row i = data[off[i]..off[i+1]), empty range = NULL")
    return f"`{table}/{col}.bin`"

def col_cpp_access(table, col, alias):
    """Snippet showing how to read column value at row r."""
    kind = COLS[table][col]
    if kind in ("int","intN"):
        return f"`{col}_bin[r]`"
    if kind == "char1":
        return f"`{col}_bin[r]` (uint8; 0=NULL)"
    if kind == "varlen":
        return (f"`std::string_view({col}_dat + {col}_off[r], {col}_off[r+1] - {col}_off[r])`")
    return f"{col}_bin[r]"

def predicate_to_cpp(table, col, pred_sql, alias):
    """Translate a SQL predicate fragment into a hint of the C++ test."""
    p = pred_sql.strip()
    kind = COLS[table][col]
    short = p
    # Strip leading "alias.col" if present at beginning
    # Just return p as-is (we render the SQL plus a brief explanation)
    if kind == "varlen":
        if "LIKE" in p.upper():
            return f"varlen substring/prefix match — scan `{col}.data.bin` using `memmem`/`memcmp` on slice `[off[r], off[r+1])`"
        if "IS NOT NULL" in p.upper():
            return f"non-empty slice: `{col}_off[r+1] > {col}_off[r]`"
        if "IS NULL" in p.upper():
            return f"empty slice: `{col}_off[r+1] == {col}_off[r]`"
        if "BETWEEN" in p.upper():
            return f"varlen lexicographic range — compare slice against literal bounds"
        if "IN (" in p.upper() or "IN(" in p.upper().replace(" ",""):
            return f"membership in literal set — `std::unordered_set<std::string>` of literals, lookup via slice"
        if re.search(r'!=|<>', p):
            return f"equality test (negated) — `memcmp` slice against literal"
        if "=" in p:
            return f"equality — resolve literal id once (see dimension PK scan); test FK column directly if comparing to dim id"
    if kind in ("int","intN"):
        if "BETWEEN" in p.upper():
            return f"int32 range — `lo <= {col}_bin[r] && {col}_bin[r] <= hi`"
        if "IS NOT NULL" in p.upper():
            return f"`{col}_bin[r] != -1`"
        if ">=" in p:
            return f"`{col}_bin[r] >= literal`"
        if ">" in p:
            return f"`{col}_bin[r] > literal`"
        if "<=" in p:
            return f"`{col}_bin[r] <= literal`"
        if "<" in p:
            return f"`{col}_bin[r] < literal`"
        if "!=" in p or "<>" in p:
            return f"`{col}_bin[r] != literal`"
        if "=" in p:
            return f"`{col}_bin[r] == literal_id` (literal_id resolved by scanning the parent dimension's text column)"
    if kind == "char1":
        if "=" in p:
            return f"`{col}_bin[r] == (uint8_t)literal_char` (e.g., 'f' = 0x66)"
    return "see SQL"

def render_pk_pos_index(table):
    rows = TABLES[table]["rows"]
    # max_id == rows for these (id is dense 1..rows in JOB)
    return (f"### `{table}__id__pos` (pk_pos_dense)\n"
            f"- File: `indexes/{table}__id__pos.bin`\n"
            f"- Layout: `int32_t[max_id + 2]` (built by `build_pk_pos` in `build_indexes.cpp`)\n"
            f"- Semantics: `pos[id]` = row position of that id in `{table}/id.bin`, or `-1` if absent\n"
            f"- Sentinel: `-1` for missing ids\n"
            f"- Build code (verbatim):\n"
            f"  ```cpp\n"
            f"  std::vector<int32_t> pos((size_t)max_id + 2, -1);\n"
            f"  for (uint64_t r = 0; r < N; ++r) {{\n"
            f"      int32_t id = ids[r];\n"
            f"      if (id >= 0 && id <= max_id) pos[(size_t)id] = (int32_t)r;\n"
            f"  }}\n"
            f"  ```\n"
            f"- Probe: `int32_t row = pos[id]; if (row < 0) /* not present */;`")

def render_primary_csr(table, sort_col):
    max_id = MAX_ID[sort_col]
    return (f"### `{table}__{sort_col}` (primary CSR — table physically sorted by {sort_col})\n"
            f"- File: `indexes/{table}__{sort_col}__offsets.bin`\n"
            f"- Layout: `uint64_t[max_{sort_col} + 2]` (size = {max_id+2})\n"
            f"- Semantics: `{table}` rows are stored contiguously sorted by `{sort_col}`.\n"
            f"  Rows with `{sort_col} = v` occupy contiguous positions `[off[v], off[v+1])`\n"
            f"  in every `{table}/<col>.bin` and `{table}/<col>.offsets.bin` file.\n"
            f"- Sentinel: empty range when `off[v] == off[v+1]` (no rows for that key).\n"
            f"  Negative/NULL `{sort_col}` values are bucketed at `v=0`.\n"
            f"- Build code (verbatim from `counting_sort` in `build_indexes.cpp`):\n"
            f"  ```cpp\n"
            f"  offsets.assign((size_t)max_k + 2, 0);\n"
            f"  for (uint64_t r = 0; r < N; ++r) {{\n"
            f"      int32_t k = key[r] < 0 ? 0 : key[r];\n"
            f"      offsets[(size_t)k + 1]++;\n"
            f"  }}\n"
            f"  for (size_t i = 1; i < offsets.size(); ++i) offsets[i] += offsets[i-1];\n"
            f"  ```\n"
            f"- Probe: `uint64_t lo = off[v], hi = off[v+1]; for (uint64_t r = lo; r < hi; ++r) {{ /* {table} row r */ }}`")

def render_aux_csr(table, col):
    max_k = MAX_ID[col]
    N = TABLES[table]["rows"]
    return (f"### `{table}__{col}` (aux CSR)\n"
            f"- Files:\n"
            f"  - `indexes/{table}__{col}__offsets.bin` — `uint64_t[max_{col} + 2]` (size = {max_k+2})\n"
            f"  - `indexes/{table}__{col}__rowids.bin`  — `int32_t[{N}]`\n"
            f"- Semantics: for key `v`, the matching `{table}` row positions are\n"
            f"  `rowids[off[v] .. off[v+1])`. Each rowid indexes into the column files of `{table}`.\n"
            f"- Sentinel: empty range when `off[v] == off[v+1]`.\n"
            f"- Build code (verbatim from `build_aux` in `build_indexes.cpp`):\n"
            f"  ```cpp\n"
            f"  std::vector<uint64_t> off((size_t)max_k + 2, 0);\n"
            f"  for (uint64_t r = 0; r < N; ++r) {{\n"
            f"      int32_t k = keys[r] < 0 ? 0 : keys[r];\n"
            f"      off[(size_t)k + 1]++;\n"
            f"  }}\n"
            f"  for (size_t i = 1; i < off.size(); ++i) off[i] += off[i-1];\n"
            f"  std::vector<int32_t> rowids(N);\n"
            f"  std::vector<uint64_t> cur = off;\n"
            f"  for (uint64_t r = 0; r < N; ++r) {{\n"
            f"      int32_t k = keys[r] < 0 ? 0 : keys[r];\n"
            f"      rowids[cur[(size_t)k]++] = (int32_t)r;\n"
            f"  }}\n"
            f"  ```\n"
            f"- Probe: `uint64_t lo = off[v], hi = off[v+1]; for (uint64_t k = lo; k < hi; ++k) {{ int32_t r = rowids[k]; /* {table} row r */ }}`")

def render_dim_scan_pattern(dim_table, text_col, literal_example=""):
    return (f"To resolve the literal in `{dim_table}.{text_col}` → id, scan the dimension's text "
            f"column once and read the parallel `id.bin` value at the matching row "
            f"(NEVER hardcode the id). The dimension is small enough to load fully.\n"
            f"```cpp\n"
            f"// Load {dim_table}/{text_col} (varlen) and {dim_table}/id.bin (int32)\n"
            f"uint64_t N = read_row_count(\"{dim_table}\");\n"
            f"const uint64_t* off = mmap_u64(\"{dim_table}/{text_col}.offsets.bin\");\n"
            f"const char*    dat  = mmap_bytes(\"{dim_table}/{text_col}.data.bin\");\n"
            f"const int32_t* ids  = mmap_i32(\"{dim_table}/id.bin\");\n"
            f"int32_t target_id = -1;\n"
            f"for (uint64_t r = 0; r < N; ++r) {{\n"
            f"    std::string_view s(dat + off[r], off[r+1] - off[r]);\n"
            f"    if (s == LITERAL) {{ target_id = ids[r]; break; }}\n"
            f"}}\n"
            f"```")

def generate_guide(qid, sql):
    alias_to_table, joins, filters, proj_cols, where_cols = parse_sql(sql)
    # All (alias,col) actually referenced anywhere
    used_cols = set()
    for a,c in proj_cols: used_cols.add((a,c))
    for a,c in where_cols: used_cols.add((a,c))
    # tables actually used (those whose aliases appear in any ref)
    used_tables = set()
    for a,c in used_cols:
        if a in alias_to_table:
            used_tables.add(alias_to_table[a])

    out = []
    out.append(f"# {qid} Guide\n")
    out.append("## SQL")
    out.append("```sql")
    out.append(sql.strip())
    out.append("```\n")

    # --- Column Reference -----------------------------------------------------
    out.append("## Column Reference")
    out.append("")
    out.append("All column files live under `<storage>/<table>/`. Fixed int32 columns are stored as raw "
               "little-endian `int32[N]`; nullable variants (`intN` below) use `-1` as the NULL sentinel. "
               "Varlen columns use a paired `<col>.offsets.bin` (`uint64[N+1]`) + `<col>.data.bin` "
               "(raw bytes); row `i`'s value is `data[off[i]..off[i+1])`, and an empty range means NULL. "
               "`char1` columns are raw `uint8[N]` with `0` denoting NULL.\n")

    # Group cols per (alias, col)
    by_table = {}
    for a,c in used_cols:
        if a not in alias_to_table: continue
        tbl = alias_to_table[a]
        if c not in COLS.get(tbl, {}): continue
        by_table.setdefault(tbl, []).append((a,c))

    # Collect predicates per (alias,col)
    preds_for = {}
    for a,c,p in filters:
        preds_for.setdefault((a,c), []).append(p)

    # Emit columns in stable table order
    for tbl in sorted(by_table.keys()):
        seen_cols = set()
        for a,c in sorted(by_table[tbl], key=lambda x:(x[1],x[0])):
            if (tbl,c) in seen_cols: continue
            seen_cols.add((tbl,c))
            kind = COLS[tbl][c]
            ctype = {"int":"int32_t","intN":"int32_t (nullable, -1)","varlen":"varlen text","char1":"uint8_t"}[kind]
            out.append(f"### `{tbl}.{c}` ({ctype})")
            if kind == "varlen":
                out.append(f"- Files: `{tbl}/{c}.offsets.bin` (uint64[{TABLES[tbl]['rows']+1}]) + `{tbl}/{c}.data.bin`")
            else:
                out.append(f"- File: `{tbl}/{c}.bin` ({TABLES[tbl]['rows']} rows of {ctype.split()[0]})")
            # Semantics
            role = ""
            if c == "id" and tbl in PK_POS:
                role = (f"Dimension primary key. Use `indexes/{tbl}__id__pos.bin` (int32[max_id+2]) "
                        f"to map id→row position. Slot `pos[id] == -1` means the id is absent.")
            elif c == TABLES[tbl]["sort"] and tbl not in PK_POS:
                role = (f"Primary FK on which `{tbl}` is physically sorted. Use the primary CSR "
                        f"`indexes/{tbl}__{c}__offsets.bin` (uint64[max+2]) to get the contiguous range "
                        f"of rows for a given parent id.")
            elif (tbl, c) in [(t2,co) for (t2,co) in AUX_CSR]:
                role = (f"Secondary FK with aux CSR `indexes/{tbl}__{c}__offsets.bin` (uint64[max+2]) + "
                        f"`indexes/{tbl}__{c}__rowids.bin` (int32[{TABLES[tbl]['rows']}]). Use it when "
                        f"this column is the more selective join key.")
            elif c in MAX_ID and tbl not in PK_POS:
                role = (f"FK to a dimension (no dedicated index on this column in `{tbl}`); test directly "
                        f"as `{c}_bin[r] == target_id`.")
            if role:
                out.append(f"- Role: {role}")
            # Predicates: collect once from `filters` directly (dedup by text).
            seen_preds = set()
            for (aa, cc, pp) in filters:
                if alias_to_table.get(aa) == tbl and cc == c and pp not in seen_preds:
                    seen_preds.add(pp)
                    hint = predicate_to_cpp(tbl, c, pp, "")
                    out.append(f"- Predicate (this query): `{pp.rstrip(';').strip()}` → {hint}")
            # Projection mention
            for a,cc in proj_cols:
                if alias_to_table.get(a)==tbl and cc==c:
                    out.append(f"- Projected: `MIN({a}.{c})` → read value only for surviving rows; "
                               f"maintain a running min (lexicographic for varlen, arithmetic for int32 "
                               f"skipping -1).")
                    break
            out.append("")

    # Dedup column entries (might be repeated due to alias loop). Filter quickly:
    # already done via seen_cols
    # --- Table Stats ----------------------------------------------------------
    out.append("## Table Stats")
    out.append("")
    out.append("| Table | Rows | Role | Sort order | Block size |")
    out.append("|---|---|---|---|---|")
    for tbl in sorted(used_tables):
        t = TABLES[tbl]
        out.append(f"| {tbl} | {t['rows']:,} | {t['role']} | {t['sort']} | {t['block']} |")
    out.append("")

    # --- Query Analysis -------------------------------------------------------
    out.append("## Query Analysis")
    out.append("")
    # join graph
    out.append("### Join graph")
    for (a,b),(c,d) in joins:
        ta = alias_to_table.get(a,a); tc = alias_to_table.get(c,c)
        out.append(f"- `{ta}.{b}` = `{tc}.{d}`")
    out.append("")
    # filters
    out.append("### Filters (alias.col → predicate)")
    if filters:
        for a,c,p in filters:
            ta = alias_to_table.get(a,a)
            out.append(f"- `{ta}.{c}`: `{p.strip()}`")
    else:
        out.append("- (none other than join equalities)")
    out.append("")
    # aggregation
    out.append("### Aggregation & projection")
    proj_list = [f"`MIN({a}.{c})`" for a,c in sorted(proj_cols) if a in alias_to_table]
    if proj_list:
        out.append(f"- {', '.join(proj_list)}")
    out.append("- Output is a single row of MIN aggregates (no GROUP BY). Maintain running mins; "
               "early-exit is NOT safe (a later row could be lex-smaller).")
    out.append("")
    # driver suggestion
    out.append("### Suggested execution outline")
    out.append("1. Resolve each dimension literal to its id by scanning that dimension's text column "
               "(use the parallel `id.bin` to read the id of the matching row — do NOT hardcode any id).")
    if any(alias_to_table.get(a) == "title" for a,_ in used_cols):
        out.append("2. Drive on `title` (PK 1..2,528,312). For each candidate `t.id = v`, probe every "
                   "movie-fact primary CSR (`<fact>__movie_id__offsets.bin`) for the range "
                   "`[off[v], off[v+1])`. Apply per-fact filters inside that range; only then read "
                   "varlen projections.")
    else:
        out.append("2. Drive on the smallest fact relation after applying its most selective filter "
                   "(use the relevant aux CSR if a non-primary FK is highly selective).")
    out.append("3. For dimension-attribute lookups after a fact probe, use the dimension's "
               "`indexes/<dim>__id__pos.bin` to convert id → row, then read varlen attributes.")
    out.append("")

    # --- Indexes --------------------------------------------------------------
    out.append("## Indexes Used")
    out.append("")
    seen_idx = set()
    # Primary CSR for any fact in used_tables
    for tbl, sc in PRIMARY_CSR:
        if tbl in used_tables and (tbl,sc) not in seen_idx:
            out.append(render_primary_csr(tbl, sc)); out.append(""); seen_idx.add((tbl,sc))
    # Aux CSR — include only if the column is referenced in WHERE/joins for this query
    for tbl, col in AUX_CSR:
        if tbl in used_tables:
            # Check if any (alias,col) in used_cols matches
            relevant = any(alias_to_table.get(a)==tbl and c==col for a,c in used_cols)
            if relevant and (tbl,col) not in seen_idx:
                out.append(render_aux_csr(tbl, col)); out.append(""); seen_idx.add((tbl,col))
    # PK pos for dim tables in used_tables
    for tbl in PK_POS:
        if tbl in used_tables:
            out.append(render_pk_pos_index(tbl)); out.append("")

    # --- Dimension literal-resolution pattern ---------------------------------
    out.append("## Dimension Literal Resolution")
    out.append("")
    out.append("Every equality on a dimension text column (e.g., `it.info = 'rating'`, `ct.kind = 'production companies'`) "
               "must be resolved at query time by scanning that dimension's varlen column and reading the parallel "
               "`id.bin` at the matching row. NEVER hardcode a dimension id constant — the value depends on the data load.")
    out.append("")
    out.append("```cpp")
    out.append("// Generic dimension lookup template")
    out.append("uint64_t Nd = *(uint64_t*)mmap_bytes(\"<dim>/__row_count.bin\");")
    out.append("const uint64_t* doff = (const uint64_t*)mmap_bytes(\"<dim>/<text_col>.offsets.bin\");")
    out.append("const char*     ddat =                  mmap_bytes(\"<dim>/<text_col>.data.bin\");")
    out.append("const int32_t*  dids = (const int32_t*) mmap_bytes(\"<dim>/id.bin\");")
    out.append("int32_t target_id = -1;")
    out.append("for (uint64_t r = 0; r < Nd; ++r) {")
    out.append("    std::string_view s(ddat + doff[r], doff[r+1] - doff[r]);")
    out.append("    if (s == LITERAL) { target_id = dids[r]; break; }")
    out.append("}")
    out.append("```")
    out.append("")
    out.append("Then use `indexes/<dim>__id__pos.bin` to map any later id-from-fact back to a row position for "
               "reading other dimension attributes.")
    out.append("")
    out.append("## Sentinels & Null Handling")
    out.append("- int32 nullable (`intN`): `-1`")
    out.append("- char1 nullable: `0`")
    out.append("- varlen NULL: empty range (`off[i] == off[i+1]`)")
    out.append("- pk_pos missing id: `-1`")
    out.append("- CSR empty bucket: `off[v] == off[v+1]`")
    return "\n".join(out) + "\n"


def main():
    queries = {}
    for qid in sorted(os.listdir(OUTDIR)):
        qdir = os.path.join(OUTDIR, qid)
        if not os.path.isdir(qdir): continue
        tpl = os.path.join(qdir, "template.sql")
        if not os.path.exists(tpl): continue
        with open(tpl) as f:
            sql = f.read()
        queries[qid] = sql
    print(f"Generating {len(queries)} guides...", file=sys.stderr)
    for qid, sql in queries.items():
        guide = generate_guide(qid, sql)
        out = os.path.join(OUTDIR, qid, "guide.md")
        with open(out, "w") as f:
            f.write(guide)
    print("done.", file=sys.stderr)

if __name__ == "__main__":
    main()
