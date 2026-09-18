/* Kernel stub for stdio.h
 * Provides minimal FILE* API backed by memory buffers
 * For llama.cpp bare-metal compatibility
 */

#ifndef _COMPAT_STDIO_H
#define _COMPAT_STDIO_H

#include <embodios/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FILE structure - memory-backed */
typedef struct _FILE {
    uint8_t* data;      /* Buffer pointer */
    size_t   size;      /* Total size */
    size_t   pos;       /* Current position */
    int      eof;       /* EOF flag */
    int      error;     /* Error flag */
    int      mode;      /* Open mode */
} FILE;

/* Standard streams (not supported - stubs) */
extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

/* Seek origins */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* EOF marker */
#define EOF (-1)

/* File operations */
FILE* fopen(const char* filename, const char* mode);
int fclose(FILE* stream);
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream);
int fseek(FILE* stream, long offset, int whence);
long ftell(FILE* stream);
void rewind(FILE* stream);
int feof(FILE* stream);
int ferror(FILE* stream);
void clearerr(FILE* stream);
int fflush(FILE* stream);
int fgetc(FILE* stream);
int ungetc(int c, FILE* stream);
char* fgets(char* s, int size, FILE* stream);

/* Printf family - stubs that do nothing or minimal work */
int printf(const char* format, ...);
int fprintf(FILE* stream, const char* format, ...);
int sprintf(char* str, const char* format, ...);
int snprintf(char* str, size_t size, const char* format, ...);
int vprintf(const char* format, __builtin_va_list ap);
int vfprintf(FILE* stream, const char* format, __builtin_va_list ap);
int vsprintf(char* str, const char* format, __builtin_va_list ap);
int vsnprintf(char* str, size_t size, const char* format, __builtin_va_list ap);

/* Scanf family - stubs */
int sscanf(const char* str, const char* format, ...);

/* File positioning */
typedef long fpos_t;
int fgetpos(FILE* stream, fpos_t* pos);
int fsetpos(FILE* stream, const fpos_t* pos);

/* Remove/rename - not supported */
int remove(const char* filename);
int rename(const char* oldname, const char* newname);

/* Temporary files - not supported */
FILE* tmpfile(void);
char* tmpnam(char* s);

/* perror - stub */
void perror(const char* s);

/* String output */
int puts(const char* s);
int fputs(const char* s, FILE* stream);
int putchar(int c);
int fputc(int c, FILE* stream);
int putc(int c, FILE* stream);

/* getline - stub */
ssize_t getline(char** lineptr, size_t* n, FILE* stream);

#ifdef __cplusplus
}
#endif

#endif /* _COMPAT_STDIO_H */
