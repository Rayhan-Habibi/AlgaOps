/*
 * csv_reader.c — CSV reader implementation for AlgaOps Assurance.
 *
 * Robustness requirements met:
 *  - Empty lines:         silently skipped.
 *  - Comment lines ('#'): silently skipped.
 *  - Header row:          auto-detected (first non-empty line starting with a
 *                         non-digit alphabetic char) and skipped.
 *  - Missing fields:      ALGAOPS_ERR_PARSE with file + line in stderr message.
 *  - Malformed numbers:   ALGAOPS_ERR_PARSE with file + line in stderr message.
 *  - Negative values in unsigned fields: ALGAOPS_ERR_PARSE.
 *  - Non-finite values:   ALGAOPS_ERR_PARSE (v1 reader).
 *  - File-open failures:  ALGAOPS_ERR_IO.
 *  - Lines > CSV_MAX_LINE_LEN-2 chars: ALGAOPS_ERR_PARSE.
 *
 * v1 files (no header, or exactly the four v1 columns in order) are parsed
 * positionally with the original strtok-based routine.  Files whose header
 * names additional columns are parsed by column name (empty cells allowed for
 * optional fields).
 */

#include "csv_reader.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include <ctype.h>
#include <stdio.h>

/* Column ids for named parsing. */
enum {
    C_TS = 0, C_SEQ, C_VIB, C_TEMP, C_BOOT, C_RECV, C_TSTART, C_WMS, C_SEXP, C_SVAL,
    C_RX, C_RY, C_RZ, C_PX, C_PY, C_PZ, C_CUR, C_SELF, C_SYNC, C_FAULTS, C_COUNT
};

static const char *COLUMN_NAMES[C_COUNT] = {
    "timestamp_ms", "sequence_id", "vibration_rms_g", "temperature_c", "boot_id",
    "received_ms", "t_start_ms", "window_ms", "samples_expected", "samples_valid",
    "vib_rms_x_g", "vib_rms_y_g", "vib_rms_z_g", "vib_p2p_x_g", "vib_p2p_y_g",
    "vib_p2p_z_g", "current_a", "self_test", "time_synced", "node_fault_flags"
};

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/** Trim trailing \r and \n characters in-place. */
static void strip_crlf(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

/** Return true if the line looks like a CSV header (starts with a letter). */
static bool is_header_line(const char *line)
{
    while (*line == ' ' || *line == '\t') ++line;
    return isalpha((unsigned char)*line) != 0;
}

/** Return true if the line is empty or whitespace-only. */
static bool is_empty_line(const char *line)
{
    while (*line) {
        if (!isspace((unsigned char)*line)) return false;
        ++line;
    }
    return true;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
    return s;
}

/** Split in place on ',' keeping empty fields. Returns field count. */
static int split_fields(char *line, char **fields, int max)
{
    int n = 0;
    char *p = line;
    for (;;) {
        if (n == max) return n + 1;          /* too many columns */
        fields[n++] = p;
        char *c = strchr(p, ',');
        if (!c) break;
        *c = '\0';
        p = c + 1;
    }
    for (int i = 0; i < n; i++) fields[i] = trim(fields[i]);
    return n;
}

static void map_header(CsvReader *r, const char *line)
{
    char tmp[CSV_MAX_LINE_LEN];
    strncpy(tmp, line, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *fields[CSV_MAX_COLUMNS];
    int n = split_fields(tmp, fields, CSV_MAX_COLUMNS);
    if (n > CSV_MAX_COLUMNS) n = CSV_MAX_COLUMNS;
    bool seen[C_COUNT] = { false };
    for (int i = 0; i < n; i++) {
        r->col[i] = -1;
        for (int c = 0; c < C_COUNT; c++) {
            if (strcmp(fields[i], COLUMN_NAMES[c]) == 0) {
                r->col[i] = c;
                seen[c] = true;
                break;
            }
        }
    }
    r->n_cols = n;
    bool all_v1 = seen[C_TS] && seen[C_SEQ] && seen[C_VIB] && seen[C_TEMP];
    bool legacy_layout = n == 4 && r->col[0] == C_TS && r->col[1] == C_SEQ &&
                         r->col[2] == C_VIB && r->col[3] == C_TEMP;
    r->named = all_v1 && !legacy_layout;
}

/* Read the next data line into `line`; handles comments/header/long lines. */
static AlgaopsStatus next_data_line(CsvReader *reader, char *line, size_t cap)
{
    for (;;) {
        if (!fgets(line, (int)cap, reader->fp)) {
            /* End of file or read error. */
            return ALGAOPS_ERR_IO;
        }
        reader->line_number++;

        /* Detect lines that were too long (no newline within buffer). */
        size_t len = strlen(line);
        if (len == cap - 1 &&
            line[len - 1] != '\n' && !feof(reader->fp)) {
            fprintf(stderr,
                    "[csv_reader] %s:%d: line too long (>%d chars), skipping\n",
                    reader->path, reader->line_number,
                    (int)(cap - 2));
            /* Drain the rest of the long line. */
            int ch;
            while ((ch = fgetc(reader->fp)) != '\n' && ch != EOF) { (void)ch; }
            return ALGAOPS_ERR_PARSE;
        }

        strip_crlf(line);

        if (is_empty_line(line))              continue;
        if (line[0] == '#')                   continue;
        if (!reader->header_skipped && is_header_line(line)) {
            reader->header_skipped = true;
            map_header(reader, line);
            continue;
        }
        return ALGAOPS_OK;
    }
}

static bool parse_u64(const char *s, uint64_t *out)
{
    if (!*s || *s == '-' || *s == '+' || isspace((unsigned char)*s)) return false;
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || *end != '\0') return false;
    *out = (uint64_t)v;
    return true;
}

static bool parse_u32(const char *s, uint32_t *out)
{
    uint64_t v;
    if (!parse_u64(s, &v) || v > 0xFFFFFFFFull) return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_double(const char *s, double *out)
{
    if (!*s) return false;
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (errno == ERANGE && isfinite(v) && v != 0.0) return false;
    if (end == s || *end != '\0') return false;
    *out = v;
    return true;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

AlgaopsStatus csv_reader_open(CsvReader *reader, const char *path)
{
    if (!reader || !path) return ALGAOPS_ERR_NULL_PTR;

    memset(reader, 0, sizeof(*reader));
    strncpy(reader->path, path, sizeof(reader->path) - 1);
    reader->path[sizeof(reader->path) - 1] = '\0';
    for (int i = 0; i < CSV_MAX_COLUMNS; i++) reader->col[i] = -1;

    reader->fp = fopen(path, "r");
    if (!reader->fp) {
        fprintf(stderr, "[csv_reader] Cannot open '%s': %s\n",
                path, strerror(errno));
        return ALGAOPS_ERR_IO;
    }

    reader->line_number    = 0;
    reader->header_skipped = false;
    return ALGAOPS_OK;
}

/* Helper macro: emit parse error and return. */
#define PARSE_ERR(msg)                                                   \
    do {                                                                  \
        fprintf(stderr, "[csv_reader] %s:%d: " msg "\n",                 \
                reader->path, reader->line_number);                       \
        return ALGAOPS_ERR_PARSE;                                         \
    } while (0)

/* Original positional v1 parser (strtok based). */
static AlgaopsStatus parse_positional(CsvReader *reader, const char *line, AlgaRecord *record)
{
    /*
     * Parse:  timestamp_ms , sequence_id , vibration_rms_g , temperature_c
     */
    char *ctx = NULL;
    char  tmp[CSV_MAX_LINE_LEN];
    strncpy(tmp, line, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

#ifdef _WIN32
    /* strtok_s is the Windows-safe version. */
    char *tok = strtok_s(tmp, ",", &ctx);
#else
    char *tok = strtok_r(tmp, ",", &ctx);
#endif

    if (!tok) PARSE_ERR("missing timestamp_ms field");
    char *endptr;
    if (*tok == '-') PARSE_ERR("negative timestamp_ms value");
    errno = 0;
    unsigned long long ts = strtoull(tok, &endptr, 10);
    if (errno || *endptr != '\0') PARSE_ERR("invalid timestamp_ms value");
    record->timestamp_ms = (uint64_t)ts;

#ifdef _WIN32
    tok = strtok_s(NULL, ",", &ctx);
#else
    tok = strtok_r(NULL, ",", &ctx);
#endif
    if (!tok) PARSE_ERR("missing sequence_id field");
    if (*tok == '-') PARSE_ERR("negative sequence_id value");
    errno = 0;
    unsigned long long seq = strtoull(tok, &endptr, 10);
    if (errno || *endptr != '\0' || seq > 0xFFFFFFFFull) PARSE_ERR("invalid sequence_id value");
    record->sequence_id = (uint32_t)seq;

#ifdef _WIN32
    tok = strtok_s(NULL, ",", &ctx);
#else
    tok = strtok_r(NULL, ",", &ctx);
#endif
    if (!tok) PARSE_ERR("missing vibration_rms_g field");
    errno = 0;
    double vib = strtod(tok, &endptr);
    if (errno || *endptr != '\0') PARSE_ERR("invalid vibration_rms_g value");
    if (!isfinite(vib))           PARSE_ERR("non-finite vibration_rms_g");
    record->vibration_rms_g = vib;

#ifdef _WIN32
    tok = strtok_s(NULL, ",", &ctx);
#else
    tok = strtok_r(NULL, ",", &ctx);
#endif
    if (!tok) PARSE_ERR("missing temperature_c field");
    errno = 0;
    double temp = strtod(tok, &endptr);
    /* Allow trailing whitespace after the last field. */
    while (*endptr == ' ' || *endptr == '\t') ++endptr;
    if (errno || *endptr != '\0') PARSE_ERR("invalid temperature_c value");
    if (!isfinite(temp))          PARSE_ERR("non-finite temperature_c");
    record->temperature_c = temp;

    return ALGAOPS_OK;
}

/* Named-column parser (v2). */
static AlgaopsStatus parse_named(CsvReader *reader, char *line, AlgaWindow *w)
{
    memset(w, 0, sizeof(*w));
    char *fields[CSV_MAX_COLUMNS];
    int n = split_fields(line, fields, CSV_MAX_COLUMNS);
    if (n > CSV_MAX_COLUMNS) PARSE_ERR("too many columns");
    if (n != reader->n_cols) PARSE_ERR("column count does not match header");

    const char *cell[C_COUNT] = { NULL };
    for (int i = 0; i < n; i++) {
        int c = reader->col[i];
        if (c >= 0) cell[c] = fields[i];
    }
#define PRESENT(c) (cell[c] && cell[c][0] != '\0')

    if (!PRESENT(C_TS))   PARSE_ERR("missing timestamp_ms field");
    if (!PRESENT(C_SEQ))  PARSE_ERR("missing sequence_id field");
    if (!PRESENT(C_VIB))  PARSE_ERR("missing vibration_rms_g field");
    if (!PRESENT(C_TEMP)) PARSE_ERR("missing temperature_c field");
    if (!parse_u64(cell[C_TS], &w->timestamp_ms))     PARSE_ERR("invalid timestamp_ms value");
    if (!parse_u32(cell[C_SEQ], &w->sequence_id))     PARSE_ERR("invalid sequence_id value");
    if (!parse_double(cell[C_VIB], &w->vibration_rms_g)) PARSE_ERR("invalid vibration_rms_g value");
    if (!parse_double(cell[C_TEMP], &w->temperature_c))  PARSE_ERR("invalid temperature_c value");

    if (PRESENT(C_BOOT) && !parse_u32(cell[C_BOOT], &w->boot_id)) PARSE_ERR("invalid boot_id");
    if (PRESENT(C_RECV)) {
        if (!parse_u64(cell[C_RECV], &w->received_ms)) PARSE_ERR("invalid received_ms");
        w->presence |= ALGA_HAS_RECEIVED;
    }
    if (PRESENT(C_TSTART) || PRESENT(C_WMS)) {
        if (!PRESENT(C_TSTART) || !PRESENT(C_WMS)) PARSE_ERR("t_start_ms and window_ms must both be set");
        if (!parse_u64(cell[C_TSTART], &w->t_start_ms)) PARSE_ERR("invalid t_start_ms");
        if (!parse_u32(cell[C_WMS], &w->window_ms))     PARSE_ERR("invalid window_ms");
        w->presence |= ALGA_HAS_TIMING;
    }
    if (PRESENT(C_SEXP) || PRESENT(C_SVAL)) {
        if (!PRESENT(C_SEXP) || !PRESENT(C_SVAL)) PARSE_ERR("samples_expected and samples_valid must both be set");
        if (!parse_u32(cell[C_SEXP], &w->samples_expected)) PARSE_ERR("invalid samples_expected");
        if (!parse_u32(cell[C_SVAL], &w->samples_valid))    PARSE_ERR("invalid samples_valid");
        w->presence |= ALGA_HAS_SAMPLE_COUNTS;
    }
    if (PRESENT(C_RX) || PRESENT(C_RY) || PRESENT(C_RZ)) {
        if (!PRESENT(C_RX) || !PRESENT(C_RY) || !PRESENT(C_RZ)) PARSE_ERR("incomplete vib_rms axes");
        if (!parse_double(cell[C_RX], &w->vib_rms_axis_g[0]) ||
            !parse_double(cell[C_RY], &w->vib_rms_axis_g[1]) ||
            !parse_double(cell[C_RZ], &w->vib_rms_axis_g[2])) PARSE_ERR("invalid vib_rms axis value");
        w->presence |= ALGA_HAS_AXES;
    }
    if (PRESENT(C_PX) || PRESENT(C_PY) || PRESENT(C_PZ)) {
        if (!PRESENT(C_PX) || !PRESENT(C_PY) || !PRESENT(C_PZ)) PARSE_ERR("incomplete vib_p2p axes");
        if (!parse_double(cell[C_PX], &w->vib_p2p_axis_g[0]) ||
            !parse_double(cell[C_PY], &w->vib_p2p_axis_g[1]) ||
            !parse_double(cell[C_PZ], &w->vib_p2p_axis_g[2])) PARSE_ERR("invalid vib_p2p axis value");
        w->presence |= ALGA_HAS_P2P;
    }
    if (PRESENT(C_CUR)) {
        if (!parse_double(cell[C_CUR], &w->current_a)) PARSE_ERR("invalid current_a");
        w->presence |= ALGA_HAS_CURRENT;
    }
    if (PRESENT(C_SELF)) {
        const char *s = cell[C_SELF];
        if (strcmp(s, "not_run") == 0 || strcmp(s, "0") == 0)   w->self_test = ALGA_SELF_TEST_NOT_RUN;
        else if (strcmp(s, "pass") == 0 || strcmp(s, "1") == 0) w->self_test = ALGA_SELF_TEST_PASS;
        else if (strcmp(s, "fail") == 0 || strcmp(s, "2") == 0) w->self_test = ALGA_SELF_TEST_FAIL;
        else PARSE_ERR("invalid self_test value");
        w->presence |= ALGA_HAS_SELF_TEST;
    }
    if (PRESENT(C_SYNC)) {
        if (strcmp(cell[C_SYNC], "1") == 0 || strcmp(cell[C_SYNC], "true") == 0) w->time_synced = 1;
        else if (strcmp(cell[C_SYNC], "0") == 0 || strcmp(cell[C_SYNC], "false") == 0) w->time_synced = 0;
        else PARSE_ERR("invalid time_synced value");
        w->presence |= ALGA_HAS_TIME_SYNC;
    }
    if (PRESENT(C_FAULTS)) {
        if (!parse_u32(cell[C_FAULTS], &w->node_fault_flags)) PARSE_ERR("invalid node_fault_flags");
        w->presence |= ALGA_HAS_NODE_FAULTS;
    }
#undef PRESENT
    return ALGAOPS_OK;
}

AlgaopsStatus csv_reader_next(CsvReader *reader, AlgaRecord *record)
{
    if (!reader || !record) return ALGAOPS_ERR_NULL_PTR;
    if (!reader->fp)        return ALGAOPS_ERR_IO;

    char line[CSV_MAX_LINE_LEN];
    AlgaopsStatus s = next_data_line(reader, line, sizeof(line));
    if (s != ALGAOPS_OK) return s;

    if (!reader->named) return parse_positional(reader, line, record);

    AlgaWindow w;
    s = parse_named(reader, line, &w);
    if (s != ALGAOPS_OK) return s;
    if (!isfinite(w.vibration_rms_g)) PARSE_ERR("non-finite vibration_rms_g");
    if (!isfinite(w.temperature_c))   PARSE_ERR("non-finite temperature_c");
    record->timestamp_ms    = w.timestamp_ms;
    record->sequence_id     = w.sequence_id;
    record->vibration_rms_g = w.vibration_rms_g;
    record->temperature_c   = w.temperature_c;
    return ALGAOPS_OK;
}

AlgaopsStatus csv_reader_next_window(CsvReader *reader, AlgaWindow *window)
{
    if (!reader || !window) return ALGAOPS_ERR_NULL_PTR;
    if (!reader->fp)        return ALGAOPS_ERR_IO;

    char line[CSV_MAX_LINE_LEN];
    AlgaopsStatus s = next_data_line(reader, line, sizeof(line));
    if (s != ALGAOPS_OK) return s;

    if (reader->named) return parse_named(reader, line, window);

    AlgaRecord rec;
    s = parse_positional(reader, line, &rec);
    if (s != ALGAOPS_OK) return s;
    memset(window, 0, sizeof(*window));
    window->timestamp_ms    = rec.timestamp_ms;
    window->sequence_id     = rec.sequence_id;
    window->vibration_rms_g = rec.vibration_rms_g;
    window->temperature_c   = rec.temperature_c;
    return ALGAOPS_OK;
}

#undef PARSE_ERR

void csv_reader_close(CsvReader *reader)
{
    if (!reader) return;
    if (reader->fp) {
        fclose(reader->fp);
        reader->fp = NULL;
    }
}

int csv_reader_line(const CsvReader *reader)
{
    if (!reader) return 0;
    return reader->line_number;
}
