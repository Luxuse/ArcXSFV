#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t state;
    uint64_t seed;
} arca_ctx;

// Change this line:
void arca_init(arca_ctx* ctx, uint64_t seed);
void arca_update(arca_ctx* ctx, const uint8_t* input, size_t len);
uint64_t arca_finalize(arca_ctx* ctx);

#ifdef __cplusplus
}
#endif