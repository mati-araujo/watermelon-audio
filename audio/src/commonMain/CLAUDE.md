# Kotlin commonMain — reglas locales

- Cero imports de `android.*` o `java.*`.
- Logging por el callback `AudioLogger` (NO `android.util.Log`).
- `getAudioBridge()`, NO `AudioNativeBridge.getInstance()`.
- `NativeLibraryLoader`, `AudioBridgeProvider` y `currentDeviceCapabilities` van por expect/actual.
- Un tipo nuevo de efecto o parametro: ver `audio/src/main/cpp/effects/CLAUDE.md`.
