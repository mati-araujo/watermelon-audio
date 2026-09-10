/**
 * tml_impl.cpp — REQ-039 S1. La UNICA TU que instancia TinyMidiLoader.
 *
 * `tml.h` es header-only con el patron de stb: la implementacion se crea donde se
 * define `TML_IMPLEMENTATION`, y tiene que ser en un solo lugar o el linker ve
 * simbolos duplicados. Es el mismo motivo por el que `tsf_impl.cpp` existe.
 *
 * Vive en `core/tests/` y no en `thirdparty/` porque el arnes de conformidad es
 * de TESTS: `tml` no entra al artefacto que se publica.
 */
#define TML_IMPLEMENTATION
#include "tml.h"
