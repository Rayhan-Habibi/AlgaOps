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
 *  - Non-finite values:   ALGAOPS_ERR_PARSE.
 *  - File-open failures:  ALGAOPS_ERR_IO.
 *  - Lines > 511 chars:   ALGAOPS_ERR_PARSE.
 */

#include "csv_reader.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include <ctype.h>
#include <stdio.h>

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

/* =========================================================================
 * Public API
 * ========================================================================= */

AlgaopsStatus csv_reader_open(CsvReader *reader, const char *path)
{
    if (!reader || !path) return ALGAOPS_ERR_NULL_PTR;

    memset(reader, 0, sizeof(*reader));
    strncpy(reader->path, path, sizeof(reader->path) - 1);
    reader->path[sizeof(reader->path) - 1] = '\0';

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

AlgaopsStatus csv_reader_next(CsvReader *reader, AlgaRecord *record)
{
    if (!reader || !record) return ALGAOPS_ERR_NULL_PTR;
    if (!reader->fp)        return ALGAOPS_ERR_IO;

    char line[CSV_MAX_LINE_LEN];

    for (;;) {
        if (!fgets(line, sizeof(line), reader->fp)) {
            /* End of file or read error. */
            return ALGAOPS_ERR_IO;
        }
        reader->line_number++;

        /* Detect lines that were too long (no newline within buffer). */
        size_t len = strlen(line);
        if (len == sizeof(line) - 1 &&
            line[len - 1] != '\n' && !feof(reader->fp)) {
            fprintf(stderr,
                    "[csv_reader] %s:%d: line too long (>%d chars), skipping\n",
                    reader->path, reader->line_number,
                    (int)(sizeof(line) - 2));
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
            continue;
        }

        /* We have a data line — parse it. */
        break;
    }

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

/* Helper macro: emit parse error and return. */
#define PARSE_ERR(msg)                                                   \
    do {                                                                  \
        fprintf(stderr, "[csv_reader] %s:%d: " msg "\n",                 \
                reader->path, reader->line_number);                       \
        return ALGAOPS_ERR_PARSE;                                         \
    } while (0)

    if (!tok) PARSE_ERR("missing timestamp_ms field");
    char *endptr;
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
    errno = 0;
    unsigned long seq = strtoul(tok, &endptr, 10);
    if (errno || *endptr != '\0') PARSE_ERR("invalid sequence_id value");
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

#undef PARSE_ERR
    return ALGAOPS_OK;
}

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
