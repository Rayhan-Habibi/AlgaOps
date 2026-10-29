/**
 * @file algaops_export.h
 * @brief Symbol-visibility macro for the shared library (libalgaops).
 *
 * The gateway loads the core through ctypes, so only functions marked
 * ALGAOPS_API are exported from the shared build.  Static builds (CLI, tests,
 * firmware) compile the macro to nothing.
 */

#ifndef ALGAOPS_EXPORT_H
#define ALGAOPS_EXPORT_H

#if defined(ALGAOPS_BUILD_SHARED)
#  if defined(_WIN32)
#    define ALGAOPS_API __declspec(dllexport)
#  elif defined(__GNUC__) || defined(__clang__)
#    define ALGAOPS_API __attribute__((visibility("default")))
#  else
#    define ALGAOPS_API
#  endif
#else
#  define ALGAOPS_API
#endif

#endif /* ALGAOPS_EXPORT_H */
