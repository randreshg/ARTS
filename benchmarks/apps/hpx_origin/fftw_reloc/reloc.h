#ifndef FFTW_RELOC_H
#define FFTW_RELOC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fftw_reloc_context fftw_reloc_context;

/* The two numerical arrays a plan pair is built on are borrowed objects: the
 * context records their identity and extent but never their bytes, so a later
 * invocation lends the array it acquired under the same identity and a row is
 * addressed as that identity plus a byte offset. */
enum { FFTW_RELOC_R2C_ARRAY = 1, FFTW_RELOC_C2C_ARRAY = 2 };

typedef struct {
  const void *address;
  uint64_t size;
  const char *name;
} fftw_reloc_symbol;

typedef struct {
  const void *address;
  uint64_t size;
  const uint64_t *slots;
  uint64_t slot_count;
} fftw_reloc_initializer;

typedef struct {
  uint64_t identity;
  uint64_t size;
  uint64_t alignment;
  void *base;
} fftw_reloc_object;

fftw_reloc_context *fftw_reloc_context_create(void);
void fftw_reloc_context_delete(fftw_reloc_context *context);
void fftw_reloc_bind(fftw_reloc_context *context, uint64_t identity,
                     void *base, uint64_t size);
/* Bind a whole numerical array the context does not own and will not publish.
 * `borrowed_key` is the identity the array carries outside this context. */
void fftw_reloc_bind_borrowed(fftw_reloc_context *context, uint64_t identity,
                              void *base, uint64_t size, uint64_t borrowed_key);
size_t fftw_reloc_object_count(const fftw_reloc_context *context);
fftw_reloc_object fftw_reloc_object_at(const fftw_reloc_context *context,
                                       size_t index);
void fftw_reloc_rebase(fftw_reloc_context *context, uint64_t identity,
                       void *base);
uint64_t fftw_reloc_reference(fftw_reloc_context *context, const void *pointer);
void *fftw_reloc_resolve(fftw_reloc_context *context, uint64_t reference);
int fftw_reloc_validate(const fftw_reloc_context *context);
void fftw_reloc_trace(const fftw_reloc_context *context);
void fftw_reloc_dump(const fftw_reloc_context *context, const char *path);
size_t fftw_reloc_snapshot_size(const fftw_reloc_context *context);
void fftw_reloc_snapshot_write(const fftw_reloc_context *context, void *image);
fftw_reloc_context *fftw_reloc_snapshot_open(void *image, size_t size);

/* Placement of a published image inside storage the caller owns.  Reserve
 * tells the caller how much storage an image of that size needs, `at` where
 * the image begins inside it, and `protect` makes the image's own pages
 * read-only for the span of the transforms.  The ordinary build reserves
 * exactly the image, places it at the base and protects nothing. */
size_t fftw_reloc_image_reserve(size_t size);
void *fftw_reloc_image_at(void *storage);
int fftw_reloc_image_protect(void *image, size_t size, int readonly);

/* One row transform: `array` is the currently acquired array bound to the
 * plan's borrowed identity, `offset` the row's byte offset inside it.  An
 * offset that starts on a complex element keeps every row's alignment equal
 * to the array's, which is what makes plan selection address-independent. */
void fftw_reloc_execute_r2c(const void *image, size_t size, uint64_t plan,
                            void *array, uint64_t offset);
void fftw_reloc_execute_c2c(const void *image, size_t size, uint64_t plan,
                            void *array, uint64_t offset);
void fftw_reloc_destroy_pair(void *image, size_t size, uint64_t r2c,
                             uint64_t c2c);
uint64_t fftw_reloc_plan_r2c(fftw_reloc_context *context, int length,
                             double *row, unsigned flags);
uint64_t fftw_reloc_plan_c2c(fftw_reloc_context *context, int length,
                             double *row, int sign, unsigned flags);

void *fftw_reloc_load(void *context, void *pointer);
void *fftw_reloc_store(void *context, void *slot, void *pointer);
int64_t fftw_reloc_difference(void *context, void *left, void *right);
uint64_t fftw_reloc_alignment(void *context, void *pointer, uint64_t mask);
void *fftw_reloc_alloc(void *context, uint64_t size);
void fftw_reloc_free(void *context, void *pointer);
void *fftw_reloc_global(void *context, uint64_t index, uint64_t size,
                         const void *initial);
void fftw_reloc_copy(void *context, void *destination, const void *source,
                      uint64_t size, int move);
void fftw_reloc_set(void *context, void *destination, int value, uint64_t size);
void fftw_reloc_qsort(void *context, void *base, uint64_t count, uint64_t size,
                       void *compare);
extern const fftw_reloc_symbol fftw_reloc_symbols[];
extern const uint64_t fftw_reloc_symbol_count;
extern const fftw_reloc_initializer fftw_reloc_initializers[];
extern const uint64_t fftw_reloc_initializer_count;
extern const uint64_t fftw_reloc_fingerprint;

#ifdef __cplusplus
}
#endif
#endif
