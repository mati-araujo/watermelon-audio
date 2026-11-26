# Afinador (REQ-001) — trampas locales

Ring lock-free + thread de analisis + snapshot atomico. La captura que alimenta el ring es un
thread RT (`../CLAUDE.md`).

## Corpus grabado (REQ-001 S10 / REQ-032)

- `bash scripts/fetch-corpus.sh` baja el corpus de los assets del release `corpus-v1` y VERIFICA
  su sha256 contra `tests/corpus-manifest.txt`. Sin corpus, los tests de robustez salen
  **SKIPPED y NUNCA passed**: una corrida que no verifico no se lee como cobertura. El directorio
  esta en `.gitignore`; los WAV no se versionan.
- `bash scripts/render-corpus.sh`: la RECETA para reconstruirlo o extenderlo. Fija el `.sf2` por
  commit + sha256 + blob y lee su version del artefacto. No hace falta para correr los tests.
- `python3 scripts/corpus-reference-pitch.py --dir DIR`: el ORACULO del `hz_verdadero` del
  manifiesto, independiente del motor (R-API-48) y del nominal. Cuatro tramos desde 2,0 s.
- `python3 scripts/spectrum-by-segment.py archivo.wav --f0 HZ`: la tabla de dB POR TRAMOS
  (Hann 250 ms) que separa hipotesis sobre un timbre real.
- La VARA del enganche (REQ-055): `tests/support/LatchInstrument.h` (nucleo puro) y
  `tests/test_latch_instrument.cpp`; el enganche se define sobre una ventana FIJA de 4096 frames
  (no sobre la tanda drenada; t0 se fecha en el sub-bloque de 256 frames del ataque; se alimenta en tandas de 256, y la
  linea de base trae ademas el RANGO sobre {96,192,240,256,480,960,1536}: el motor de hoy depende de la particion del
  flujo, ver la cabecera del TSV); mide t_primera / t_conv / error por enganche contra el oraculo
  corto `tests/corpus-v1-oraculo-corto.tsv` (`corpus-reference-pitch.py --short`, desde la raiz, ver
  su docstring) y compara con `tests/baseline/enganche-corpus-v1.tsv`. Esa linea de base se regenera
  con `WMA_LATCH_BASELINE_UPDATE=1 bash scripts/run-cpp-tests.sh -R LatchInstrumentCorpus` y su diff
  ES la revision.
- POR ATAQUE (REQ-055 D11, `measureAttacks`): t0 de cada punteo lo da el ORACULO (lineas `# ataque` del
  TSV corto, detector de `corpus-reference-pitch.py`, probado por su `--self-test`, que NO corre en el
  gate: correrlo a mano si se toca el detector), nunca el motor. Linea de base en
  `tests/baseline/ataque-corpus-v1.tsv`, mismo comando de regeneracion. La toma del g42 sale SKIPPED.

- EL MAPA DE CORTES (REQ-055 0.6): el volcado (`TunerInputDump`) escribe `wmad` v2 con hasta 256 cortes UBICADOS
  (frame del ARCHIVO; perdida con largo, costura como tramo de largo desconocido) y la toma sale sana; lo no ubicable la
  deja `.invalid.wav`. El oraculo emite `# corte NOMBRE INI FIN PERDIDOS|?`, mide por segmento y excluye el ataque cuya
  guarda `[t0 - 2048, tEnd]` toca un corte (`# ataque-excluido`); el instrumento (`cutTouchesSpan`) usa la MISMA regla.
  Si se toca una, se toca la otra y se corre el `--self-test`.

Los numeros del snapshot (cuantos valores lleva) driftean: se cuentan del codigo, no de un doc.
