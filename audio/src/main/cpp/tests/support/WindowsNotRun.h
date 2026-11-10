#pragma once

/**
 * WMA_WINDOWS_NOT_RUN("razon") — un test POSIX por naturaleza (FIFO, fd, mmap de sub-region) que en
 * Windows no corre. REQ-061 AC-061.1.
 *
 * El TEST se registra en las dos plataformas (aparece en `--gtest_list_tests` de las dos) y en
 * Windows sale SKIPPED con el marcador `[windows-not-run]`. El cuerpo POSIX va dentro de
 * `#if !defined(_WIN32)` DESPUES de la macro. Cada uso tiene su linea en `tests/windows-not-run.txt`
 * (`Suite.Test | razon`); `scripts/check-windows-not-run.py` exige que el conjunto de usos sea el listado.
 */

#include <gtest/gtest.h>

#if defined(_WIN32)
#define WMA_WINDOWS_NOT_RUN(razon) GTEST_SKIP() << "[windows-not-run] " << razon
#else
#define WMA_WINDOWS_NOT_RUN(razon) static_cast<void>(0)
#endif
