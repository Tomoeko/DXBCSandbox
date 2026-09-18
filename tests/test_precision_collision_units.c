#include "common/common.h"
#include "common/sha256.h"
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_parser.h"
#include "test_fixture.h"

#include <stdio.h>
#include <string.h>

#ifndef DXBC_PRECISION_HALF_D3D_FIXTURE
#define DXBC_PRECISION_HALF_D3D_FIXTURE \
  "tests/fixtures/precision_half_d3d_fragment.b64"
#endif
#ifndef DXBC_PRECISION_FLOAT_D3D_FIXTURE
#define DXBC_PRECISION_FLOAT_D3D_FIXTURE \
  "tests/fixtures/precision_float_d3d_fragment.b64"
#endif
#ifndef DXBC_PRECISION_HALF_GL_FIXTURE
#define DXBC_PRECISION_HALF_GL_FIXTURE \
  "tests/fixtures/precision_half_glcore_linked.b64"
#endif
#ifndef DXBC_PRECISION_FLOAT_GL_FIXTURE
#define DXBC_PRECISION_FLOAT_GL_FIXTURE \
  "tests/fixtures/precision_float_glcore_linked.b64"
#endif

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,     \
              #condition);                                                     \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static bool contains_bytes(const uint8_t* bytes, size_t size,
                           const char* text) {
  if (!bytes || !text) return false;
  const size_t text_size = strlen(text);
  if (text_size == 0 || text_size > size) return false;
  for (size_t offset = 0; offset <= size - text_size; ++offset) {
    if (memcmp(bytes + offset, text, text_size) == 0) return true;
  }
  return false;
}

static bool digest_matches(const uint8_t* bytes, size_t size,
                           const char* expected) {
  static const char digits[] = "0123456789abcdef";
  uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
  char actual[COMMON_SHA256_DIGEST_SIZE * 2u + 1u];
  common_sha256(bytes, size, digest);
  for (size_t index = 0; index < sizeof(digest); ++index) {
    actual[index * 2u] = digits[digest[index] >> 4u];
    actual[index * 2u + 1u] = digits[digest[index] & 0x0fu];
  }
  actual[sizeof(actual) - 1u] = '\0';
  return strcmp(actual, expected) == 0;
}

int main(void) {
  uint8_t* half_d3d = NULL;
  uint8_t* float_d3d = NULL;
  uint8_t* half_gl = NULL;
  uint8_t* float_gl = NULL;
  size_t half_d3d_size = 0;
  size_t float_d3d_size = 0;
  size_t half_gl_size = 0;
  size_t float_gl_size = 0;

  CHECK(test_fixture_decode_base64(DXBC_PRECISION_HALF_D3D_FIXTURE,
                                   &half_d3d, &half_d3d_size));
  CHECK(test_fixture_decode_base64(DXBC_PRECISION_FLOAT_D3D_FIXTURE,
                                   &float_d3d, &float_d3d_size));
  CHECK(test_fixture_decode_base64(DXBC_PRECISION_HALF_GL_FIXTURE,
                                   &half_gl, &half_gl_size));
  CHECK(test_fixture_decode_base64(DXBC_PRECISION_FLOAT_GL_FIXTURE,
                                   &float_gl, &float_gl_size));

  CHECK(half_d3d_size == 426u && float_d3d_size == half_d3d_size);
  CHECK(memcmp(half_d3d, float_d3d, half_d3d_size) == 0);
  CHECK(!contains_bytes(half_d3d, half_d3d_size, "RDEF"));
  CHECK(digest_matches(
      half_d3d, half_d3d_size,
      "6b5b735995e589e28f06098e85207d8a5f5f62f3027c66b9d61a01ab322d0ee1"));

  DXBCContainerView half_view;
  DXBCContainerView float_view;
  CHECK(dxbc_container_view_first(half_d3d, half_d3d_size, &half_view));
  CHECK(dxbc_container_view_first(float_d3d, float_d3d_size, &float_view));
  CHECK(half_view.data == half_d3d + 38u);
  CHECK(float_view.data == float_d3d + 38u);
  CHECK(half_view.size == float_view.size);
  CHECK(memcmp(half_view.data, float_view.data, half_view.size) == 0);
  CHECK(digest_matches(
      half_view.data, half_view.size,
      "715512d43134376d4f90a04f9250ca11b448c0a754c4e287bdbc30e72afd50b7"));
  DXBCDocument document;
  DXBCDocumentDiagnostic diagnostic;
  dxbc_document_init(&document);
  CHECK(dxbc_document_parse(&document, half_view.data, half_view.size,
                            &diagnostic));
  dxbc_document_free(&document);

  CHECK(half_gl_size == 1299u && float_gl_size == 1297u);
  CHECK(half_gl_size != float_gl_size ||
        memcmp(half_gl, float_gl, half_gl_size) != 0);
  CHECK(contains_bytes(half_gl, half_gl_size, "rgba16f"));
  CHECK(contains_bytes(half_gl, half_gl_size, "mediump"));
  CHECK(contains_bytes(float_gl, float_gl_size, "rgba32f"));
  CHECK(contains_bytes(float_gl, float_gl_size, "highp"));
  CHECK(digest_matches(
      half_gl, half_gl_size,
      "629be170d8e7d8ca303f8ef945542e64785a72c53813a1e72a38159f1fb1711f"));
  CHECK(digest_matches(
      float_gl, float_gl_size,
      "a6f68dd0f0c96e4e894618290e9d1a71ec29425d36468d7b0264d6a8c15b4018"));

  mem_free(half_d3d, half_d3d_size);
  mem_free(float_d3d, float_d3d_size);
  mem_free(half_gl, half_gl_size);
  mem_free(float_gl, float_gl_size);
  CHECK(g_allocations_count == 0u);
  CHECK(g_allocated_bytes == 0u);
  printf("Stripped-DXBC/GLCore precision collision fixture passed.\n");
  return 0;
}
