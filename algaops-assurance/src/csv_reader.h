/**
 * @file csv_reader.h
 * @brief CSV reader for AlgaOps sensor measurement records.
 *
 * Reads files conforming to the schema:
 *   timestamp_ms,sequence_id,vibration_rms_g,temperature_c
 *
 * Limitations:
 *  - Lines longer than CSV_MAX_LINE_LEN bytes are rejected.
 *  - Field values must be representable as uint64_t, uint32_t, or double.
 *  - Non-finite values (NaN, Inf) are flagged as parse errors.
 *  - The header row (first non-empty line beginning with a letter) is skipped
 *    automatically.
 *  - Empty lines and lines starting with '#' are silently skipped.
 *  - Partial records (fewer than 4 fields) are reported as errors.
 */

#ifndef CSV_READER_H
#define CSV_READER_H

#include "algaops_types.h"
#include <stdio.h>

/** Maximum supported CSV line length in bytes (including newline). */
#define CSV_MAX_LINE_LEN 512

/**
 * @brief Opaque CSV reader context.
 *
 * Embed on the stack or heap; initialise with csv_reader_open().
 */
typedef struct {
    FILE    *fp;
    char     path[256];
    int      line_number;    /**< Current line number (1-indexed). */
    bool     header_skipped;
} CsvReader;

/**
 * @brief Open a CSV file for reading.
 *
 * @param reader  Non-NULL pointer to caller-owned CsvReader storage.
 * @param path    Path to the CSV file.
 * @return ALGAOPS_OK on success.
 *         ALGAOPS_ERR_NULL_PTR if reader or path is NULL.
 *         ALGAOPS_ERR_IO if the file cannot be opened.
 */
AlgaopsStatus csv_reader_open(CsvReader *reader, const char *path);

/**
 * @brief Read the next record from an open CSV file.
 *
 * Skips empty lines, comment lines ('#'), and the header row.
 *
 * @param reader  Non-NULL pointer to an open CsvReader.
 * @param record  Non-NULL pointer to an AlgaRecord that receives the result.
 * @return ALGAOPS_OK      if a record was successfully parsed.
 *         ALGAOPS_ERR_IO  if end-of-file was reached (no more records).
 *         ALGAOPS_ERR_PARSE if the line could not be parsed.
 *                          The reader advances past the bad line; call again
 *                          to attempt the next record.
 */
AlgaopsStatus csv_reader_next(CsvReader *reader, AlgaRecord *record);

/**
 * @brief Close the CSV reader and release the file handle.
 *
 * Safe to call on an already-closed or never-opened reader (no-op).
 *
 * @param reader  Non-NULL pointer to a CsvReader.
 */
void csv_reader_close(CsvReader *reader);

/**
 * @brief Return the current line number (1-indexed) of the reader.
 */
int csv_reader_line(const CsvReader *reader);

#endif /* CSV_READER_H */
