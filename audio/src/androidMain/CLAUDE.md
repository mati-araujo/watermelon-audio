# Kotlin androidMain — reglas locales

- Dependencias Android permitidas: DataStore, Lifecycle, Core KTX, hardware.usb.
- `android.util.Log` permitido (no necesita abstraccion aca).
- Antes de tocar una `external fun` de `AudioNativeBridge.kt`: `audio/src/main/cpp/jni/CLAUDE.md`
  (mutex por categoria, `Result<T>`, y los tres gates del cruce).
