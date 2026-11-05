# Análisis del plugin original: ME FreeFilter

Plugin analizado: `ME FreeFilter WL.dll` (DLL COM/DirectX con API `Vc*` de Wavelab, x86 de 32 bits, 1 986 560 bytes).
SHA-256: `7fed8edd612129f4b63186bbe376b792019c32de184c7ee078409d47ce18f6ff`.
Producto: *Steinberg Mastering Edition* v1.5, © 1998–2001 Spectral Design GmbH.

Herramientas: REA + Ghidra 12.1.4 (análisis estático) y un host de pruebas propio de 32 bits que carga la DLL
**con una instalación registrada legítimamente** (la DLL valida el registro de la Mastering Edition al arrancar;
no se ha parcheado ni saltado nada). Aquí solo se documenta el **comportamiento observado**: ni el código
decompilado ni la DLL forman parte de este repositorio.

## Qué hace el motor (modo de bandas iguales, el modo por defecto)

1. **30 bandas** con sliders en dB (±15). El punto de control de la banda `k` está en `(k + 0.5) · fs / 60`
   (367.5 Hz + 735 Hz · k a 44.1 kHz). El slider 31 de la interfaz original no tiene efecto.
2. Los valores se **interpolan linealmente en dB** entre los puntos de control sobre 513 bins de una FFT de
   1024 puntos, con los extremos planos (el primer y el último valor se extienden hasta DC y Nyquist).
   `G[bin] = 10^(x[bin] / 20)` (amplitud).
3. `G` se pasa al tiempo (IFFT de 1024 puntos, espectro simétrico → respuesta real y simétrica) y se multiplica
   por una **ventana Hann de 257 puntos**. El resultado es un **FIR lineal de 257 taps**.
4. El audio se filtra con ese FIR. El original lo hace por bloques FFT con una latencia de **896 muestras**
   (igual a todas las frecuencias de muestreo); aquí se usa convolución directa con **128 muestras** de latencia.
5. El filtro es idéntico en términos normalizados para 32, 44.1, 48, 64, 88.2 y 96 kHz.

## Verificación contra la DLL original

Con el host de pruebas se compararon, usando ruido gaussiano de 49 152 muestras y la misma configuración de
sliders en ambos:

| Caso | Diferencia entre la salida del original y la de `FreeFilterDSP.h` |
|---|---|
| Todo a 0 dB | −84.7 dB (relativa a la señal) |
| Todo a +12 dB | −84.7 dB |
| 6 configuraciones aleatorias de ±15 dB | −55 … −58 dB |

La respuesta en frecuencia de un slider aislado (+12 dB, 13 bins) coincide con el original con un error máximo de
0.0006 dB (test de regresión en `tests/dsp_test.cpp`). La diferencia residual de las configuraciones aleatorias
(unas milésimas en amplitud) viene del procesado por bloques del original, que no es exactamente invariante en el tiempo.

## Diferencias deliberadas

* **Latencia** de 128 muestras (el original, 896). La respuesta en frecuencia es la misma.
* **Cambios de slider** con *crossfade* de 256 muestras entre juegos de coeficientes, para evitar chasquidos.
* **Interfaz** dibujada con vectores propios (el original usaba mapas de bits de Spectral Design).
* **Modos de dibujo**: se han implementado *dibujo libre*, *nivel* y *recta* según el comportamiento
  esperado; los nombres originales (*rubber band*, *niveau*, *bone*) no se han verificado contra el original.

## Pendiente

* **Learn / Match / Morph** (aprendizaje de espectro de origen y destino y aplicación de 0 a 200 %).
  El motor guarda dos arrays de 513 bins con los espectros aprendidos.
* **Modo de tercios de octava** (el original conmuta entre bandas iguales y tercios de octava).
* Verificar el comportamiento exacto de *Freeze* (aquí congela el analizador).
