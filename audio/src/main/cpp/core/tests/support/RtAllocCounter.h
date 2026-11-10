#pragma once

/**
 * RtAllocCounter.h — cuenta las allocations de UN hilo, para afirmar "0 alloc en RT".
 *
 * Reemplaza el `operator new`/`delete` globales del binario de test (rtalloc_counter.cpp).
 * El conteo solo corre en el hilo que lo armo con `arm()`: los demas hilos (el de control, gtest)
 * allocan como siempre y no suman. Es el unico instrumento de allocation de la suite; un
 * callback que alloca (un `std::vector::resize`, un `new`) se ve como `count > 0`.
 *
 * No intercepta `malloc` directo: una libreria C que alocara por fuera de `operator new` no se
 * ve. Sirve para la clase de defecto que importa aca (contenedores y `new` de C++).
 */

#include <cstdint>

namespace wma_test {

/// Desde ahora, las allocations de ESTE hilo se cuentan.
void armAllocCounter();
/// Deja de contar en este hilo y devuelve cuantas hubo desde `armAllocCounter()`.
int64_t disarmAllocCounter();

}  // namespace wma_test
