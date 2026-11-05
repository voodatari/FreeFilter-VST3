# FreeFilter (VST3 / AU, Windows x64 + macOS Apple Silicon)

Reimplementación en JUCE 8 del ecualizador de filtro libre **ME FreeFilter** (Steinberg Mastering Edition,
2001). Formatos: **VST3**, **AU** y **Standalone**. Compila en **Windows x64** y **macOS arm64** (M1–M4)
con GitHub Actions.

El filtro se dedujo analizando el plugin original y está contrastado contra él (ver
[docs/ANALISIS.md](docs/ANALISIS.md), con lo verificado y lo no verificado).

## Qué hace

* **30 bandas** de ±15 dB, interpoladas en dB y realizadas como FIR lineal.
  * **Lin** (bandas iguales): bandas lineales hasta Nyquist, FIR de 257 taps, latencia 128 muestras.
  * **Log** (tercios de octava): 25 Hz … 20 kHz, FIR de 8193 taps por convolución, latencia 4096 muestras.
    La latencia se comunica al host.
* **Learn / Match / Morph:** elige **Source** o **Dest.**, activa **Learn** y reproduce el material; al terminar,
  aprende el otro lado. Con ambos aprendidos, **Match** aplica `Morph · (Dest/Source)` en dB (Morph de 0 a 200 %)
  más los sliders, que en Match son un juego independiente.
* **Analizador de espectro** de entrada (verde), salida (rojo) y curva del filtro (amarillo).
* Modos de dibujo: libre, nivel y recta. Doble clic en un slider lo pone a 0 dB.
  **Reset** pone todo a 0, borra lo aprendido y desactiva Learn/Match. **Freeze** congela el analizador.
* Clic en el título → créditos.

## Compilar

Requisitos: CMake ≥ 3.22 y un compilador C++17 (MSVC o Xcode). JUCE se descarga solo.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

Los binarios quedan en `build/FreeFilter_artefacts/Release/`.
Tests del DSP (sin JUCE): `g++ -std=c++17 tests/dsp_test.cpp -o dsp_test && ./dsp_test`.

## GitHub Actions

`.github/workflows/build.yml` ejecuta los tests y compila Windows x64 y macOS arm64 en cada *push*.
Los `.zip` salen como artefactos. Una etiqueta `v*` crea además un *release* en borrador.

## Instalar en macOS

Los binarios llevan firma *ad-hoc* (sin cuenta de desarrollador), así que Gatekeeper los pone en cuarentena
al descargarlos:

```bash
xattr -cr "FreeFilter.vst3"
cp -R "FreeFilter.vst3" ~/Library/Audio/Plug-Ins/VST3/
```

(AU: `~/Library/Audio/Plug-Ins/Components/`).

## Aviso legal

El plugin original es software comercial de terceros (© Spectral Design GmbH / Steinberg). Este repositorio
contiene **código propio**: ni la DLL original, ni código decompilado, ni sus mapas de bits se incluyen.
Las ideas y algoritmos (interpolación, ventana Hann, FIR, espectros medios) no son material con copyright, pero
el nombre y la imagen del producto original sí pueden estar protegidos: **no publiques ni vendas esto con el
nombre del original** sin permiso. Por eso el repo es privado.
