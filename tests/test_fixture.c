#include "test_fixture.h"

#include "common/common.h"
#include "common/file_io.h"

static int base64_value(unsigned char character) {
  if (character >= 'A' && character <= 'Z') return character - 'A';
  if (character >= 'a' && character <= 'z') return character - 'a' + 26;
  if (character >= '0' && character <= '9') return character - '0' + 52;
  if (character == '+') return 62;
  if (character == '/') return 63;
  return -1;
}

bool test_fixture_decode_base64(const char* path, uint8_t** out_bytes,
                                size_t* out_size) {
  if (!path || !out_bytes || !out_size) return false;
  *out_bytes = NULL;
  *out_size = 0;
  CommonFileBytes encoded;
  if (common_file_read_regular(path, 16u * 1024u * 1024u, &encoded) !=
      COMMON_FILE_OK) {
    return false;
  }
  if (encoded.size > (SIZE_MAX / 3u) * 4u) {
    common_file_bytes_dispose(&encoded);
    return false;
  }
  const size_t allocation = (encoded.size / 4u + 1u) * 3u;
  uint8_t* decoded = mem_alloc(allocation);
  if (!decoded) {
    common_file_bytes_dispose(&encoded);
    return false;
  }
  unsigned quartet[4];
  unsigned quartet_count = 0;
  size_t decoded_size = 0;
  bool reached_padding = false;
  bool ok = true;
  for (size_t index = 0; index < encoded.size; ++index) {
    const unsigned char character = encoded.data[index];
    if (character == ' ' || character == '\t' || character == '\r' ||
        character == '\n') {
      continue;
    }
    if (reached_padding) {
      ok = false;
      break;
    }
    if (character == '=') {
      quartet[quartet_count++] = 64u;
    } else {
      const int value = base64_value(character);
      if (value < 0) {
        ok = false;
        break;
      }
      quartet[quartet_count++] = (unsigned)value;
    }
    if (quartet_count != 4u) continue;
    if (quartet[0] >= 64u || quartet[1] >= 64u ||
        (quartet[2] == 64u && quartet[3] != 64u)) {
      ok = false;
      break;
    }
    decoded[decoded_size++] =
        (uint8_t)((quartet[0] << 2u) | (quartet[1] >> 4u));
    if (quartet[2] != 64u) {
      decoded[decoded_size++] =
          (uint8_t)((quartet[1] << 4u) | (quartet[2] >> 2u));
      if (quartet[3] != 64u) {
        decoded[decoded_size++] =
            (uint8_t)((quartet[2] << 6u) | quartet[3]);
      }
    }
    reached_padding = quartet[2] == 64u || quartet[3] == 64u;
    quartet_count = 0;
  }
  if (quartet_count != 0u || decoded_size == 0) ok = false;
  common_file_bytes_dispose(&encoded);
  if (!ok) {
    mem_free(decoded, allocation);
    return false;
  }
  void* exact = mem_realloc(decoded, allocation, decoded_size);
  if (!exact) {
    mem_free(decoded, allocation);
    return false;
  }
  *out_bytes = exact;
  *out_size = decoded_size;
  return true;
}
