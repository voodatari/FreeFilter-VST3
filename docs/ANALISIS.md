# Análisis del plugin original: ME FreeFilter

Plugin analizado: `ME FreeFilter WL.dll` (DLL COM/DirectX con API `Vc*` de Wavelab, x86 de 32 bits, 1 986 560 bytes).
SHA-256: `7fed8edd612129f4b63186bbe376b792019c32de184c7ee078409d47ce18f6ff`.
Producto: *Steinberg Mastering Edition* v1.5, © 1998–2001 Spectral Design GmbH.

Herramientas: REA + Ghidra 12.1.4 (análisis estático) y un host de pruebas propio de 32 bits que carga la DLL
**con una instalación de la Mastering Edition registrada legítimamente** (la DLL valida el registro al arrancar;
no se ha parcheado ni saltado nada). Aquí solo se documenta el **comportamiento observado**: ni el código
decompilado ni la DLL ni sus mapas de bits forman parte de este repositorio.

## Modo "bandas iguales" (Lin) — verificado contra el original

1. **30 bandas** con sliders en dB (±15). El punto de control de la banda `k` está en `(k + 0.5) · fs / 60`.
   El slider 31 de la interfaz original no tiene efecto.
2. Interpolación **lineal en dB** entre puntos de control sobre los 513 bins de una FFT de 1024 puntos,
   con extremos planos. `G[bin] = 10^(x[bin] / 20)` (amplitud).
3. `G` → IFFT (simétrica) → ventana **Hann de 257 puntos** → **FIR lineal de 257 taps**.
4. Latencia del original: 896 muestras (igual a todas las frecuencias de muestreo); aquí 128 (convolución directa).

## Learn / Match / Morph — verificado contra el original (modo bandas iguales)

* El original mantiene dos espectros de 513 bins: **A** (`P+0x30`) y **B** (`P+0x34`).
* **Learn** acumula la potencia `|X|²` de **bloques rectangulares de 1024 muestras con avance de 768** (el primer
  bloque empieza 256 muestras antes del primer audio; el analizador es continuo y su fase es global).
  Con el mismo ruido de entrada, mis espectros coinciden con los del original con una dispersión de 3·10⁻⁵.
* **Match** divide cada array por su número de bloques (media) y construye la curva
  `dB[bin] = Morph · 10·log10(A[bin] / B[bin]) + interpolación(sliders de Match)`
  (un bin igual a 0 se toma como 1). Se usa una **tabla de sliders independiente** (`P+0x44`) en lugar de la normal.
  Después, el mismo diseño del FIR que arriba. **Morph** va de 0 a 200 % (1.0 = 100 %).
* A y B se asocian a **Dest** y **Source** por el significado de la fórmula (amplificar donde el destino tiene más
  energía que el origen); el mapeo exacto de los botones de la interfaz original no se pudo comprobar.
* En el original, Match no se puede activar en modo Learn, ni sin haber aprendido origen y destino.

Verificación con espectros sintéticos (A con +6/+10/−8 dB, B con +0/+4, Morph 0.5/1/1.5/2, sliders de Match):
**error máximo 0.001 dB** en 6 casos. Verificación de la cadena completa (aprender dos ruidos → Match → respuesta):
**0.0012 dB**.

## Modo "tercios de octava" (Log) — verificado solo en las bandas altas

* 30 puntos de control en las frecuencias de tercio de octava (25 Hz … 20 kHz), con interpolación **lineal en Hz**.
* El original usa dos procesadores (bandas graves a menor tasa, agudas a tasa completa) con una latencia total de
  10 819 muestras a 44.1 kHz en estéreo.
* **Bandas altas (≳ 2 kHz): verificadas.** Un slider a +12 dB en 8 kHz da un pico de +11.36 dB, igual que el suavizado
  de 257 puntos a tasa completa del modo de bandas iguales.
* **Bandas graves: NO verificadas.** En el host de pruebas, el original en modo tercios no da una respuesta plana por
  debajo de ~1.3 kHz con todos los sliders a 0 dB (rizado de hasta −19 dB, determinista, independiente del tamaño
  de bloque y de la frecuencia de muestreo), mientras que el modo de bandas iguales es plano (±0.06 dB). No se ha
  podido explicar (la CPU es una Intel Core i7 estándar). Por eso las bandas graves de este plugin son un diseño ideal
  (interpolación lineal en Hz con resolución de ≈ ±11 Hz), no una copia comprobada del original.
* Implementación: FIR de 8193 taps con convolución particionada (latencia 4096 muestras), mezcla suave entre
  la respuesta de resolución fina (graves) y la de 257 puntos (agudos) entre 1 y 1.6 kHz.

## Diferencias deliberadas

* **Latencia**: 128 muestras en bandas iguales (original 896) y 4096 en tercios (original 10 819).
* **Cambios de slider** con *crossfade* de 256 muestras entre juegos de coeficientes (bandas iguales) o con el
  cambio de respuesta de la convolución de JUCE (tercios).
* **Interfaz** dibujada con vectores propios (el original usaba mapas de bits de Spectral Design).
* **Modos de dibujo**: *dibujo libre*, *nivel* y *recta* según el comportamiento esperado; los nombres originales
  (*rubber band*, *niveau*, *bone*) no se han verificado.
* **Freeze** congela el analizador (el original: "congela todo el plugin"; comportamiento exacto no verificado).
* En modo tercios, el aprendizaje usa bloques de 16384 muestras con avance 12288 (resolución fina), no los de
  1024/768 del modo de bandas iguales.
* **Reset** pone todos los sliders a 0, borra lo aprendido y desactiva Learn y Match.
