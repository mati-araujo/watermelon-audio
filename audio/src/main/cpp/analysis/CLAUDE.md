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

Los numeros del snapshot (cuantos valores lleva) driftean: se cuentan del codigo, no de un doc.
