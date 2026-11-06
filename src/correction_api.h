#pragma once
#include <stdint.h>
#include <rime_api.h>
// Scratch C ABI v1. All multibyte buffer fields are little endian. Call only
// from the engine's locked thread. Generation must identify a worker lifetime.
#define SHIXIN_TYPO_CORRECTION_VERSION 1u
#define SHIXIN_TYPO_CORRECTION_TOKEN_SIZE 40u
#define SHIXIN_TYPO_CORRECTION_HEADER_SIZE 16u
#define SHIXIN_TYPO_CORRECTION_RECORD_SIZE 328u
#define SHIXIN_TYPO_CORRECTION_SNAPSHOT_SIZE 2640u
#ifdef __cplusplus
extern "C" {
#endif
RIME_DLL uint32_t shixin_typo_corrections_version(void);
RIME_DLL int32_t shixin_typo_corrections_v1_configure(uintptr_t session,
                                              uint64_t generation);
RIME_DLL int32_t shixin_typo_corrections_v1_read(uintptr_t session, uint8_t *output,
                                         uint32_t capacity);
RIME_DLL int32_t shixin_typo_corrections_v1_select(uintptr_t session,
                                           const uint8_t *token, uint32_t size);
RIME_DLL int32_t shixin_typo_corrections_v1_invalidate(uintptr_t session);
#ifdef __cplusplus
}
#endif
