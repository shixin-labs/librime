#include "correction_api.h"
#include <rime/gear/correction_registry.h>
#include <rime/service.h>
#include <cstring>
#include <limits>
#ifdef _WIN32
#define SHIXIN_TYPO_EXPORT extern "C" RIME_DLL
#else
#define SHIXIN_TYPO_EXPORT                                                \
  extern "C" __attribute__((visibility("default")))
#endif
namespace {
void ShixinTypoWrite32(uint8_t *out, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    out[i] = uint8_t(value >> (8 * i));
}
void ShixinTypoWrite64(uint8_t *out, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i)
    out[i] = uint8_t(value >> (8 * i));
}
uint64_t ShixinTypoRead64(const uint8_t *in) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i)
    value |= uint64_t(in[i]) << (8 * i);
  return value;
}
} // namespace
SHIXIN_TYPO_EXPORT uint32_t shixin_typo_corrections_version() {
  return 1;
}
SHIXIN_TYPO_EXPORT int32_t
shixin_typo_corrections_v1_configure(uintptr_t id, uint64_t generation) {
  auto session = rime::Service::instance().GetSession(id);
  return session && session->context()->ConfigureShixinTypoCorrections(generation)
             ? 1
             : 0;
}
SHIXIN_TYPO_EXPORT int32_t shixin_typo_corrections_v1_read(
    uintptr_t id, uint8_t *output, uint32_t capacity) {
  static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                "float encoding");
  constexpr auto bytes = SHIXIN_TYPO_CORRECTION_SNAPSHOT_SIZE;
  if (!output || capacity < bytes)
    return -1;
  std::memset(output, 0, bytes);
  auto session = rime::Service::instance().GetSession(id);
  if (!session)
    return -2;
  auto *context = session->context();
  const auto count = context->ShixinTypoCorrectionSize();
  if (count > 8)
    return -3;
  ShixinTypoWrite32(output, 1);
  ShixinTypoWrite32(output + 4, bytes);
  ShixinTypoWrite32(output + 8, count);
  for (size_t i = 0; i < count; ++i) {
    shixin_typo::CorrectionView view;
    if (!context->ReadShixinTypoCorrection(i, &view) || view.text.size() > 256) {
      std::memset(output, 0, bytes);
      return -3;
    }
    auto *record = output + 16 + 328 * i;
    ShixinTypoWrite64(record, view.token.scope.generation);
    ShixinTypoWrite64(record + 8, view.token.scope.session);
    ShixinTypoWrite64(record + 16, view.token.scope.revision);
    ShixinTypoWrite64(record + 24, view.token.publication);
    record[32] = view.token.slot;
    ShixinTypoWrite32(record + 40, view.begin);
    ShixinTypoWrite32(record + 44, view.end);
    ShixinTypoWrite32(record + 48, static_cast<uint32_t>(view.edit.kind));
    ShixinTypoWrite32(record + 52, view.edit.begin);
    ShixinTypoWrite32(record + 56, view.edit.end);
    uint32_t cost;
    std::memcpy(&cost, &view.edit.cost, 4);
    ShixinTypoWrite32(record + 60, cost);
    ShixinTypoWrite32(record + 64, view.text.size());
    std::memcpy(record + 72, view.text.data(), view.text.size());
  }
  return 0;
}
SHIXIN_TYPO_EXPORT int32_t shixin_typo_corrections_v1_select(
    uintptr_t id, const uint8_t *bytes, uint32_t size) {
  if (!bytes || size != 40 || bytes[32] >= 8)
    return 0;
  for (unsigned i = 33; i < 40; ++i)
    if (bytes[i])
      return 0;
  auto session = rime::Service::instance().GetSession(id);
  if (!session)
    return 0;
  shixin_typo::CorrectionToken token{
      {ShixinTypoRead64(bytes), ShixinTypoRead64(bytes + 8), ShixinTypoRead64(bytes + 16)},
      ShixinTypoRead64(bytes + 24),
      bytes[32]};
  return session->context()->SelectShixinTypoCorrection(token) ? 1 : 0;
}
SHIXIN_TYPO_EXPORT int32_t
shixin_typo_corrections_v1_invalidate(uintptr_t id) {
  auto session = rime::Service::instance().GetSession(id);
  if (!session)
    return 0;
  session->context()->InvalidateShixinTypoCorrections();
  return 1;
}
#include "typo_resources.inc"
#undef SHIXIN_TYPO_EXPORT
