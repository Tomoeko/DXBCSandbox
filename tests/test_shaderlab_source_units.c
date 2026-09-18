#include "common/shaderlab_source.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static ShaderLabSourceNameStatus extract(const char* source, char** name) {
    return shaderlab_source_extract_name(
        (const uint8_t*)source, strlen(source), name);
}

int main(void) {
    char* name = (char*)1;
    CHECK(extract("// Shader \"Fake\"\nShader \"Real/Long Name\" { }",
                  &name) == SHADERLAB_SOURCE_NAME_OK);
    CHECK(strcmp(name, "Real/Long Name") == 0);
    shaderlab_source_name_free(name);

    CHECK(extract("/* Shader \"Fake\" {} */\n"
                  "ShaderThing \"No\" {}\n"
                  "Shader /* gap */ \"Actual\" {\n"
                  "  SubShader { Pass { Name \"} Shader \\\"No\\\"\" } }\n"
                  "}",
                  &name) == SHADERLAB_SOURCE_NAME_OK);
    CHECK(strcmp(name, "Actual") == 0);
    shaderlab_source_name_free(name);

    CHECK(extract("Shader \"One\" {} Shader \"Two\" {}", &name) ==
          SHADERLAB_SOURCE_NAME_MULTIPLE);
    CHECK(name == NULL);
    CHECK(extract("Shader \"Escaped\\\"Name\" {}", &name) ==
          SHADERLAB_SOURCE_NAME_MALFORMED);
    CHECK(name == NULL);
    CHECK(extract("Shader \"NoBody\"", &name) ==
          SHADERLAB_SOURCE_NAME_MALFORMED);
    CHECK(extract("Shader \"Open\" {", &name) ==
          SHADERLAB_SOURCE_NAME_MALFORMED);
    CHECK(extract("/* unterminated", &name) ==
          SHADERLAB_SOURCE_NAME_MALFORMED);
    CHECK(extract("ShaderThing \"No\" {}", &name) ==
          SHADERLAB_SOURCE_NAME_NOT_FOUND);
    CHECK(shaderlab_source_extract_name(NULL, 0U, &name) ==
          SHADERLAB_SOURCE_NAME_NOT_FOUND);
    CHECK(shaderlab_source_extract_name(NULL, 1U, &name) ==
          SHADERLAB_SOURCE_NAME_MALFORMED);
    CHECK(shaderlab_source_extract_name((const uint8_t*)"Shader \"X\" {}",
                                        13U, NULL) ==
          SHADERLAB_SOURCE_NAME_MALFORMED);

    puts("shaderlab source tests passed");
    return 0;
}
