#pragma once

/**
 * FakeCaptureStream.h — TEST DOUBLE, host test build only.
 *
 * Perillas del INodeCaptureStream falso que entrega `createNodeCaptureStream()`
 * en `test_platform_backends.cpp` (las definiciones viven ahi). APAGADO POR
 * DEFECTO: con la perilla apagada la fabrica devuelve nullptr, igual que el
 * build de host real, y `startInputStream()` informa false.
 *
 * El estado es global y atomico porque el stream lo crea el nodo y el test lo
 * observa desde afuera, a veces desde otro hilo que hace de hilo de captura.
 */

namespace wma_test {

/// Enciende o apaga la fabrica. Default: apagada (nullptr).
void setFakeCaptureStreamAvailable(bool available);
/// Apaga la fabrica, restaura las perillas a su default y pone los contadores en 0.
void resetFakeCaptureStream();

void setFakeCaptureStreamOpenFails(bool fails);
void setFakeCaptureStreamStartFails(bool fails);
void setFakeCaptureStreamSampleRate(int sampleRate);  // default 48000

int fakeCaptureStreamOpens();
int fakeCaptureStreamCloses();
bool fakeCaptureStreamIsOpen();
/// Abrir estando abierto, arrancar estando cerrado, cerrar estando cerrado.
int fakeCaptureStreamViolations();

/**
 * Como el thread RT: si hay un stream abierto y arrancado, llama
 * `processInputBlock(copia, frames, 2, 0)` sobre el nodo dueño y devuelve lo que
 * devuelve. false si no hay stream vivo, o no esta abierto y arrancado.
 * `interleavedStereo` se copia: processInputBlock escribe in place.
 */
bool fakeCaptureStreamDeliver(const float* interleavedStereo, int frames);

}  // namespace wma_test
