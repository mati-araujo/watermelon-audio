#!/usr/bin/env python3
"""
corpus-reference-pitch.py — REQ-032 S1. EL ORACULO del corpus: la altura verdadera de cada WAV.

🔴 INDEPENDIENTE DEL MOTOR, A PROPOSITO. Un `hz_verdadero` que saliera del propio afinador no seria
un oraculo (R-API-48): el barrido probaria que el motor es igual a si mismo. Y NO es el nominal: el
G2 del bajo acustico viene 21 cents desafinado en el propio SoundFont, y el f0 medido que el
consumidor mando fallo justo en la E4 (calidad 0,017). Esto mide el FUNDAMENTAL sobre el archivo,
con un metodo distinto del que usa el motor (NSDF + strobe de fase): pico espectral por Goertzel con
ventana de Hann e interpolacion parabolica, sobre varios tramos, y reporta cuanto discrepan.

Solo stdlib: no le agrega una dependencia al repo por un script que corre una vez por archivo.

    python3 scripts/corpus-reference-pitch.py archivo.wav [--nominal HZ]
    python3 scripts/corpus-reference-pitch.py --dir DIR         # todos los .wav, nominal por nombre
    python3 scripts/corpus-reference-pitch.py --dir DIR --partials 4   # REQ-035: H1..H4 por archivo
    python3 scripts/corpus-reference-pitch.py --self-test      # el instrumento contra una serie conocida

Modos nuevos de REQ-055 (afinador, «lectura progresiva») al final de este docstring: `--short`
(la altura instantanea por tramos cortos) y la lectura de WAV float32 con rechazo de tomas invalidas.

Salida por archivo (una linea, tab-separada):
    nombre  hz_mediana  spread_cents  n_tramos  veredicto(ok|inestable)

`spread_cents` es la diferencia max-min entre tramos: la CALIDAD del oraculo. Por encima de
`--max-spread` (1 cent por defecto) el archivo no tiene una altura estable que declarar y el
manifiesto lo lista sin hz — no se le inventa una.

EL ORACULO POR PARCIAL (REQ-035 S1, `--partials N`)
---------------------------------------------------
El strobe del motor ajusta `cents_n = C + 600·log2(1 + B·n²)` sobre los parciales 1..4 y publica C.
Sobre el corpus real esa C se aparta del H1 hasta +5,4 cents, y para saber DONDE nace el error hay
que medir cada parcial del archivo con un metodo que no sea el del motor. Con `--partials N` se
mide, en los mismos tramos, el pico de cada parcial n alrededor de `n·hz_1` y se imprime:

    cents_n   la desviacion del parcial n respecto de `n·hz_1`, en cents (n = 1 vale 0 por
              construccion: hz_1 ES la referencia)
    dB_n      su nivel respecto de H1 (un parcial a −40 dB no es un parcial: es ruido con pico)
    B_fit     el B que mejor explica cents_1..N con el MISMO modelo que usa el motor, ajustado
              aca en Python (busqueda por seccion aurea, C libre): independiente del strobe
    C_fit     la C de ese ajuste — lo que un ajuste no ponderado publicaria sobre ESTOS parciales
    res_max   el residuo maximo del ajuste, en cents: cuanto NO sigue la serie estirada el sample

Si `C_fit` reproduce el error de la fina del motor, el error nace en los parciales del sample y en
el modelo, no en el rastreo de fase. Si no, hay que mirar el rastreo. La linea sigue siendo
tab-separada; con `--partials 1` (el default) la salida es la de siempre, byte a byte.

🔴 `C` NO ES H1, Y LA DIFERENCIA ES `600·log2(1+B)`. En una cuerda con rigidez el parcial 1 TAMBIEN
esta estirado: `f_1 = f0·√(1+B)`. El motor publica C = la desviacion del fundamental IDEAL f0; este
oraculo toma como referencia el H1 que suena. Sobre una serie perfecta, entonces, `C_fit` vale
`−600·log2(1+B)`: −0,09 c con B = 1e-4 y −0,43 c con 5e-4. Lo delato el propio self-test (la
primera version esperaba 0 y salio rojo con 5e-4). Es fisica, no error, y esta por debajo de lo
que REQ-035 persigue (+5,4 c) — pero un lector que compare C con H1 tiene que restarlo.

🔴 EL INSTRUMENTO SE VERIFICA ANTES DE CREERLE (`--self-test`): sobre un WAV sintetico con B
conocido devuelve `600·log2(1+B·n²)` a 0,1 c y recupera B; y sobre uno con un parcial corrido
+10 c el residuo lo delata (> 2 c). Un oraculo que no puede fallar no es un oraculo.

EL ORACULO CORTO (REQ-055 D4, `--short`)
----------------------------------------
`measure` promedia 0,75 s: declara lo SOSTENIDO. El afinador de REQ-055 lee progresivamente, y a los
250 ms del ataque su verdad es lo que la cuerda hace AHORA, glide del ataque incluido (D4). Con
`--short` el oraculo da la altura instantanea por tramos: ventana Hann de max(`--short-win`, 10
periodos del centro) (0,1 s por defecto; en E1 son 0,243 s), salto `--short-hop` (0,05 s). Por
tramo busca el pico del DTFT (Goertzel + interpolacion parabolica, el metodo de siempre) en ±`--span-cents`
(150) del centro (`--nominal` o el de `nominal_for`). Barre grueso (~10 c) sólo en el primer tramo
con senal o si pierde el seguimiento (o hay un salto de nivel > 10 dB); despues sigue al tramo
anterior (±20 c) con parabola sucesiva sobre el log de la magnitud hasta < 0,005 c. Es lo que lo
hace posible en Python puro: 5 s tardan 0,1-0,6 s en vez de minutos.

    python3 scripts/corpus-reference-pitch.py --short --dir DIR --tsv OUT.tsv
    python3 scripts/corpus-reference-pitch.py --short archivo.wav --nominal 196

Salida por tramo (tab-separada): `archivo t_ini_s t_fin_s hz cents_vs_centro nivel_dbfs`. El nivel es
el RMS del tramo; bajo `--short-floor-dbfs` (-60) o sin pico en ±span el hz es `nan`: no hay altura
que declarar, y no se inventa una. Con `--tsv` la cabecera `#` dice el comando, la ventana y el hop, y
cada archivo va precedido de `# archivo NOMBRE sha256 HEX rate HZ frames N`: el consumidor ata el
oraculo al archivo EXACTO. Limite conocido: con 10 periodos (E1, E2) la fuga del 2do armonico en la
ventana de Hann sesga el pico hasta ~0,1 c segun la fase; con la ventana de 0,1 s es < 0,002 c.

LECTURA DE WAV Y TOMAS INVALIDAS
--------------------------------
El afinador vuelca su entrada a WAV float32 mono con un chunk propio `wmad`; el modulo `wave` no lee
float, asi que el script trae su parser RIFF: PCM16 (igual que siempre, la salida de los modos
viejos es identica byte a byte), float32 IEEE y WAVE_FORMAT_EXTENSIBLE con subformato PCM16 o float,
mono o estereo (estereo = 0,5·(L+R)). Cualquier otra cosa es SystemExit. Una toma con huecos no puede
ser oraculo de nada: se RECHAZA si el nombre termina en `.invalid.wav`, o si `wmad` dice valid=0,
framesLost>0 o discontinuities>0 (v1), o su rate no es el de fmt (o su version no es la 1 ni la 2).
Solo `--allow-invalid` la lee igual.

`wmad` v2 (REQ-055 0.6) trae el MAPA DE CORTES: una toma valida puede tener perdidas y costuras
UBICADAS (framesLost y discontinuities son totales; unlocated* tiene que ser 0 si valid=1). Cada corte
es un intervalo cerrado [ini, fin] en frames del archivo: perdida -> ini=fin=fileFrame; costura de
captura -> [fileFrame, fileFrame+fileSpan]. Un v2 incoherente (chunk != 64+24*capacidad, cutCount >
capacidad, kind fuera de {1,2}, cortes desordenados o fuera de [0, framesWritten], valid=1 con
unlocated>0, framesLost != suma de perdidas + unlocatedFrames) se rechaza. Con cortes, el oraculo
mide POR SEGMENTO (ninguna ventana mezcla frames de dos lados de un corte) y excluye el ataque que
toca un corte (ver `split_attacks`).
"""
import argparse, hashlib, math, operator, os, struct, sys, wave

# Nominales por nombre de cuerda: SOLO para centrar la busqueda (±100 cents). El resultado sale
# del archivo. Tabla publicada (A4 = 440), no derivada del motor.
NOMINAL = {"E1": 41.20344, "A1": 55.0, "D2": 73.41619, "G2": 97.99886,
           "E2": 82.40689, "A2": 110.0, "D3": 146.83238, "G3": 195.99772, "B3": 246.94165,
           "E4": 329.62756, "C4": 261.62557, "G4": 391.99544, "A4": 440.0}

# Chunk `wmad` que el afinador agrega al volcar su entrada (REQ-055): u32 version, u64 framesWritten,
# u64 framesLost, u64 discontinuities, i32 sampleRate, i32 inputSource, u32 valid. Little-endian,
# sin relleno. Se reescribe aca (R-API-48): el layout es el contrato del archivo, no codigo del motor.
WMAD_FMT = '<IQQQiiI'
WMAD_SIZE = struct.calcsize(WMAD_FMT)
assert WMAD_SIZE == 40
# v2: tras los 40 de v1, u64 unlocatedFrames, u64 unlocatedDiscontinuities, u32 cutCount, u32
# cutCapacity; despues cutCapacity slots de {u64 fileFrame, u64 lostFrames, u32 fileSpan, u32 kind}.
WMAD_V2_FMT = '<QQII'
WMAD_CUT_FMT = '<QQII'
WMAD_V2_HEAD = WMAD_SIZE + struct.calcsize(WMAD_V2_FMT)
WMAD_CUT_SIZE = struct.calcsize(WMAD_CUT_FMT)
assert WMAD_V2_HEAD == 64 and WMAD_CUT_SIZE == 24
CUT_LOSS, CUT_SEAM = 1, 2
WAVE_FORMAT_PCM, WAVE_FORMAT_FLOAT, WAVE_FORMAT_EXTENSIBLE = 1, 3, 0xFFFE

# Si es True, una toma marcada como invalida se lee igual (`--allow-invalid`). Una toma con huecos
# no puede ser oraculo de nada: el default es rechazarla.
ALLOW_INVALID = False

def parse_wav(data, name):
    """Parser RIFF propio (el modulo `wave` no lee float). `data` son los bytes del archivo.

    Devuelve (mono, sr, frames, wmad|None). PCM16 y float32 IEEE, mono o estereo (estereo: media de
    L y R); WAVE_FORMAT_EXTENSIBLE con subformato PCM o float. Todo lo demas: SystemExit."""
    if len(data) < 12 or data[0:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise SystemExit(f"{name}: no es un WAV RIFF/WAVE")
    fmt = wmad_raw = pcm = None
    data_declared = 0
    pos = 12
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack_from('<I', data, pos + 4)[0]
        body = pos + 8
        end = min(body + size, len(data))     # `data` de una captura cortada puede declarar de mas
        if cid == b'fmt ':
            fmt = data[body:end]
        elif cid == b'data' and pcm is None:
            pcm = data[body:end]
            data_declared = size
        elif cid == b'wmad':
            wmad_raw = data[body:end]
        pos = body + size + (size & 1)        # los chunks se alinean a par
    if fmt is None or len(fmt) < 16:
        raise SystemExit(f"{name}: falta el chunk fmt")
    if pcm is None:
        raise SystemExit(f"{name}: falta el chunk data")
    tag, ch, sr, _, _, bits = struct.unpack_from('<HHIIHH', fmt, 0)
    if tag == WAVE_FORMAT_EXTENSIBLE:
        if len(fmt) < 40:
            raise SystemExit(f"{name}: WAVE_FORMAT_EXTENSIBLE truncado")
        tag = struct.unpack_from('<H', fmt, 24)[0]     # los 2 primeros bytes del GUID del subformato
    if ch not in (1, 2):
        raise SystemExit(f"{name}: {ch} canales; solo mono o estereo")
    if tag == WAVE_FORMAT_PCM and bits == 16:
        code, width = 'h', 2
    elif tag == WAVE_FORMAT_FLOAT and bits == 32:
        code, width = 'f', 4
    else:
        raise SystemExit(f"{name}: formato {tag} de {bits} bits no soportado (PCM16 o float32)")
    n = len(pcm) // (width * ch)
    vals = struct.unpack('<%d%s' % (n * ch, code), pcm[:n * ch * width])
    if code == 'f':
        if not all(map(math.isfinite, vals)):
            raise SystemExit(f"{name}: muestras NaN/inf en el float32")
        if ch == 1:
            mono = list(vals)
        else:
            mono = [0.5 * (vals[i] + vals[i + 1]) for i in range(0, n * ch, ch)]
    elif ch == 1:
        mono = [v / 32768.0 for v in vals]
    else:
        mono = [(vals[i] + vals[i + 1]) / 65536.0 for i in range(0, n * ch, ch)]
    wmad = None
    if wmad_raw is not None:
        wmad, bad = parse_wmad(wmad_raw)
        if bad and not ALLOW_INVALID:
            raise SystemExit(f"{name}: toma rechazada, {bad} (--allow-invalid para leerla igual)")
        if bad:
            print(f"{name}: AVISO wmad incoherente ({bad}): se descartan sus cortes y se mide a traves de "
                  "cortes no declarados", file=sys.stderr)
    if not ALLOW_INVALID:
        why = invalid_reason(name, sr, wmad)
        # Un float32 con wmad lo escribio el volcado, que declara cuanto escribio: un `data` mas
        # corto que lo declarado, o con otros frames que wmad, es un archivo cortado o con huecos.
        # El PCM16 viejo, sin wmad, conserva la tolerancia a una captura cortada.
        if why is None and wmad is not None and code == 'f':
            if data_declared != len(pcm) or len(pcm) != n * ch * width:
                why = f"el chunk data declara {data_declared} bytes y hay {len(pcm)} (archivo cortado)"
            elif wmad["frames_written"] != n:
                why = f"wmad framesWritten={wmad['frames_written']} y el archivo tiene {n} frames"
        if why:
            raise SystemExit(f"{name}: toma rechazada, {why} (--allow-invalid para leerla igual)")
    return mono, sr, n, wmad

def parse_wmad(raw):
    """(wmad|None, problema|None). `wmad["cuts"]` = [(ini, fin, perdidos|None)] en frames del archivo
    (perdidos None = costura, largo desconocido); vacio si la toma es v1 o el chunk es incoherente."""
    if len(raw) < WMAD_SIZE:
        return None, f"chunk wmad de {len(raw)} bytes (esperaba {WMAD_SIZE})"
    k = ("version", "frames_written", "frames_lost", "discontinuities", "rate", "input_source", "valid")
    w = dict(zip(k, struct.unpack_from(WMAD_FMT, raw, 0)))
    w["cuts"] = []
    if w["version"] != 2:
        return w, None
    if len(raw) < WMAD_V2_HEAD:
        return w, f"chunk wmad v2 de {len(raw)} bytes (esperaba al menos {WMAD_V2_HEAD})"
    unl_frames, unl_disc, count, cap = struct.unpack_from(WMAD_V2_FMT, raw, WMAD_SIZE)
    w.update(unlocated_frames=unl_frames, unlocated_discontinuities=unl_disc, cut_count=count, cut_capacity=cap)
    if len(raw) != WMAD_V2_HEAD + WMAD_CUT_SIZE * cap:
        return w, f"chunk wmad v2 de {len(raw)} bytes (esperaba {WMAD_V2_HEAD + WMAD_CUT_SIZE * cap} = 64 + 24*{cap})"
    if count > cap:
        return w, f"wmad cutCount={count} mayor que cutCapacity={cap}"
    cuts, prev, lost_sum = [], 0, 0
    for i in range(count):
        frame, lost, span, kind = struct.unpack_from(WMAD_CUT_FMT, raw, WMAD_V2_HEAD + WMAD_CUT_SIZE * i)
        if kind not in (CUT_LOSS, CUT_SEAM):
            return w, f"wmad corte {i} con kind={kind} (esperaba 1 o 2)"
        if frame < prev:
            return w, f"wmad cortes desordenados: el {i} cae en {frame} y el anterior en {prev}"
        if frame + span > w["frames_written"]:
            return w, f"wmad corte {i} [{frame}, {frame + span}] fuera de [0, framesWritten={w['frames_written']}]"
        if kind == CUT_LOSS and span != 0:
            return w, f"wmad corte {i}: una perdida con fileSpan={span} (tiene que ser 0)"
        if kind == CUT_SEAM and lost != 0:
            return w, f"wmad corte {i}: una costura con lostFrames={lost} (tiene que ser 0)"
        prev, lost_sum = frame, lost_sum + lost
        cuts.append((frame, frame + span if kind == CUT_SEAM else frame, lost if kind == CUT_LOSS else None))
    if w["valid"] and (unl_frames > 0 or unl_disc > 0):
        return w, f"wmad valid=1 con unlocatedFrames={unl_frames} unlocatedDiscontinuities={unl_disc}"
    if w["frames_lost"] != lost_sum + unl_frames:
        return w, f"wmad framesLost={w['frames_lost']} y la suma de los cortes ({lost_sum}) + unlocatedFrames ({unl_frames}) es otra"
    w["cuts"] = cuts
    return w, None

def invalid_reason(name, sr, wmad):
    """Por que una toma no puede ser oraculo, o None. Una toma con huecos no mide la altura de nada."""
    if name.endswith(".invalid.wav"):
        return "el nombre dice .invalid.wav"
    if wmad is None:
        return None
    if wmad["version"] not in (1, 2):
        return f"wmad version {wmad['version']} desconocida"
    if not wmad["valid"]: return "wmad valid=0"
    if wmad["version"] == 1:       # en v2 las perdidas ubicadas son cortes, no invalidez
        if wmad["frames_lost"] > 0: return f"wmad framesLost={wmad['frames_lost']}"
        if wmad["discontinuities"] > 0: return f"wmad discontinuities={wmad['discontinuities']}"
    if wmad["rate"] != sr: return f"el rate de wmad ({wmad['rate']}) no es el de fmt ({sr})"
    return None

def read_wav(path):
    """(mono, sr, info) con el sha256 y los frames, para atar el oraculo al archivo exacto."""
    with open(path, 'rb') as f:
        data = f.read()
    mono, sr, n, wmad = parse_wav(data, os.path.basename(path))
    return mono, sr, {"sha256": hashlib.sha256(data).hexdigest(), "frames": n, "wmad": wmad}

def read_mono(path):
    mono, sr, _ = read_wav(path)
    return mono, sr

def goertzel(x, sr, hz):
    """Magnitud en `hz`. `x` ya viene con la ventana aplicada."""
    w = 2.0 * math.pi * hz / sr
    c = 2.0 * math.cos(w)
    s1 = s2 = 0.0
    for v in x:
        s = v + c * s1 - s2
        s2 = s1
        s1 = s
    return math.sqrt(max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2))

def cents(f, ref): return 1200.0 * math.log2(f / ref)
def detune(ref, c): return ref * 2.0 ** (c / 1200.0)

def peak_in(x, sr, center, span_cents, step_cents):
    """Barre `center` ± span en pasos y devuelve (hz, mag) del maximo, con interpolacion parabolica."""
    grid = []
    c = -span_cents
    while c <= span_cents + 1e-9:
        grid.append(c); c += step_cents
    mags = [goertzel(x, sr, detune(center, g)) for g in grid]
    i = max(range(len(mags)), key=lambda k: mags[k])
    if 0 < i < len(mags) - 1:
        a, b, cc = mags[i - 1], mags[i], mags[i + 1]
        den = a - 2 * b + cc
        off = 0.5 * (a - cc) / den if den != 0 else 0.0
    else:
        off = 0.0
    return detune(center, grid[i] + off * step_cents), mags[i]

def measure(path, nominal, win_s=0.75, starts=(2.0, 2.75, 3.5, 4.25)):
    mono, sr = read_mono(path)
    n = int(win_s * sr)
    hann = [0.5 * (1.0 - math.cos(2.0 * math.pi * (i + 0.5) / n)) for i in range(n)]
    # Desde 2,0 s, no desde el ataque: MEDIDO que el preset de guitarra limpia (PC 27) trae un
    # GLIDE de afinacion que dura mas de un segundo (E4: -2,28 c a 0,5 s, -0,97 c a 1,0 s, +0,03 c
    # de 2,0 s en adelante; G3: -3,9 c y despues -1,02 c estable). El afinador mide la nota
    # sostenida, no el transitorio, y el manifiesto declara lo sostenido. Cuatro tramos de 0,75 s
    # hasta el note-off (5,0 s); si un archivo no se estabiliza ni ahi, el spread lo dice.
    per_window = []
    for t0 in starts:
        a = int(t0 * sr)
        if a + n > len(mono): break
        x = [mono[a + i] * hann[i] for i in range(n)]
        coarse, _ = peak_in(x, sr, nominal, 100.0, 10.0)   # ±100 cents, de a 10
        fine, _ = peak_in(x, sr, coarse, 12.0, 0.25)       # ±12 cents, de a 0,25
        per_window.append(fine)
    if not per_window:
        return None, None, 0, []
    raw = list(per_window)
    per_window.sort()
    med = per_window[len(per_window) // 2] if len(per_window) % 2 else \
          0.5 * (per_window[len(per_window) // 2 - 1] + per_window[len(per_window) // 2])
    spread = cents(per_window[-1], per_window[0])
    return med, spread, len(per_window), raw

# --- REQ-035 S1: el oraculo POR PARCIAL ------------------------------------------------------

def stretch_cents(B, n):
    """El modelo del motor: `600·log2(1 + B·n²)`. Se reescribe aca a proposito (no se importa nada
    del motor): es la formula publicada de una cuerda con rigidez, no una decision del codigo."""
    return 600.0 * math.log2(1.0 + B * n * n)

def fit_stretched(cents_by_n, b_max=5e-3, iters=60):
    """Ajusta `cents_n = C + stretch(B, n)` con C libre y B en [0, b_max] (seccion aurea sobre B;
    para B fijo la C optima es la media de los residuos). Devuelve (B, C, residuos por n)."""
    ns = sorted(cents_by_n)
    def sse(B):
        C = sum(cents_by_n[n] - stretch_cents(B, n) for n in ns) / len(ns)
        return sum((cents_by_n[n] - C - stretch_cents(B, n)) ** 2 for n in ns), C
    phi = 0.6180339887498949
    lo, hi = 0.0, b_max
    b1, b2 = hi - phi * (hi - lo), lo + phi * (hi - lo)
    f1, f2 = sse(b1)[0], sse(b2)[0]
    for _ in range(iters):
        if f1 < f2:
            hi, b2, f2 = b2, b1, f1
            b1 = hi - phi * (hi - lo); f1 = sse(b1)[0]
        else:
            lo, b1, f1 = b1, b2, f2
            b2 = lo + phi * (hi - lo); f2 = sse(b2)[0]
    B = 0.5 * (lo + hi)
    _, C = sse(B)
    return B, C, {n: cents_by_n[n] - C - stretch_cents(B, n) for n in ns}

def measure_partials(path, nominal, partials, win_s=0.75, starts=(2.0, 2.75, 3.5, 4.25)):
    """Como `measure`, y ademas los parciales 2..`partials` alrededor de `n·hz_1` en cada tramo.

    Devuelve un dict por n con `hz` (mediana entre tramos), `cents` respecto de `n·hz_1`,
    `spread` entre tramos y `db` respecto de H1 (mediana). Para n = 1, `cents` vale 0.0.
    """
    mono, sr = read_mono(path)
    n_win = int(win_s * sr)
    hann = [0.5 * (1.0 - math.cos(2.0 * math.pi * (i + 0.5) / n_win)) for i in range(n_win)]
    per = {n: [] for n in range(1, partials + 1)}     # hz por tramo
    mag = {n: [] for n in range(1, partials + 1)}
    for t0 in starts:
        a = int(t0 * sr)
        if a + n_win > len(mono): break
        x = [mono[a + i] * hann[i] for i in range(n_win)]
        coarse, _ = peak_in(x, sr, nominal, 100.0, 10.0)
        f1, m1 = peak_in(x, sr, coarse, 12.0, 0.25)
        per[1].append(f1); mag[1].append(m1)
        for n in range(2, partials + 1):
            if n * f1 >= 0.5 * sr: break
            # El estiramiento de una cuerda real llega a ~14 c en el 4to parcial con B = 1e-3:
            # ±30 c de a 3 lo cubre con margen, y despues ±4 c de a 0,25 para interpolar.
            c, _ = peak_in(x, sr, n * f1, 30.0, 3.0)
            f, m = peak_in(x, sr, c, 4.0, 0.25)
            per[n].append(f); mag[n].append(m)
    out = {}
    if not per[1]:
        return out
    def median(v):
        v = sorted(v)
        return v[len(v) // 2] if len(v) % 2 else 0.5 * (v[len(v) // 2 - 1] + v[len(v) // 2])
    hz1 = median(per[1]); m1 = median(mag[1])
    for n in range(1, partials + 1):
        if not per[n]: continue
        hz = median(per[n])
        out[n] = {"hz": hz,
                  "cents": 0.0 if n == 1 else cents(hz, n * hz1),
                  "spread": cents(max(per[n]), min(per[n])),
                  "db": 20.0 * math.log10(max(median(mag[n]), 1e-12) / max(m1, 1e-12)),
                  "n_tramos": len(per[n])}
    return out

def format_partials(name, nominal, res, max_spread):
    if 1 not in res:
        return f"{name}\t-\t-\t0\tsin-tramos"
    hz1 = res[1]["hz"]
    verdict = "ok" if res[1]["spread"] <= max_spread else "inestable"
    cols = [name, f"{hz1:.5f}", f"{res[1]['spread']:.3f}", str(res[1]["n_tramos"]), verdict,
            f"vs-nominal={cents(hz1, nominal):+.3f}c"]
    for n in sorted(res):
        if n == 1: continue
        r = res[n]
        cols.append(f"p{n}={r['cents']:+.2f}c(±{r['spread']:.2f},{r['db']:+.0f}dB)")
    fit = {n: res[n]["cents"] for n in res}
    if len(fit) >= 3:
        B, C, resid = fit_stretched(fit)
        cols.append(f"B_fit={B:.2e}")
        cols.append(f"C_fit={C:+.2f}c")
        cols.append(f"res_max={max(abs(v) for v in resid.values()):.2f}c")
    return "\t".join(cols)

# --- REQ-055 (D4): el oraculo CORTO, la altura instantanea --------------------------------------

SHORT_PERIODS = 10          # la ventana nunca baja de 10 periodos del centro: menos no resuelve el f0
_HANN = {}

def hann_window(n):
    if n not in _HANN:
        _HANN[n] = [0.5 * (1.0 - math.cos(2.0 * math.pi * (i + 0.5) / n)) for i in range(n)]
    return _HANN[n]

def goertzel_fast(x, c):
    """Lo mismo que `goertzel` con `c = 2cos(w)` ya calculado y un solo paso por muestra: este es el
    lazo caliente del modo corto (cientos de tramos por archivo, en Python puro)."""
    s1 = s2 = 0.0
    for v in x:
        s1, s2 = v + c * s1 - s2, s1
    return math.sqrt(max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2))

def refine_peak(logmag, c0, s0, limit, max_iter=12):
    """Parabola sucesiva sobre el LOG de la magnitud (con Hann el pico es casi una gaussiana: la
    parabola en log es la que menos sesga) con paso decreciente. Si el maximo cae en un borde del
    trio, corre el centro y repite. Devuelve cents respecto del centro de busqueda, o None si se
    aleja mas de `limit` de `c0` o no converge: eso es 'perdi el pico', y el que llama decide."""
    c, s = c0, s0
    for _ in range(max_iter):
        a, b, d = logmag(c - s), logmag(c), logmag(c + s)
        if b < a or b < d:
            c += -s if a > d else s
        else:
            den = a - 2.0 * b + d
            if den >= 0.0:                      # plano: no hay curvatura de la que fiarse
                return None
            off = 0.5 * s * (a - d) / den
            c += off
            if abs(off) < 0.005:                # 0,005 c: 10x por debajo de los 0,05 c exigidos
                return c
            s = max(0.4 * s, 0.3)
        if abs(c - c0) > limit:
            return None
    return None

def locate(x, sr, center, span, prev):
    """Cents del pico de `x` (ya con Hann) respecto de `center`, o None si no hay pico en ±span.
    Con `prev` sigue desde el tramo anterior (±20 c); sin el, o si lo pierde, barre ±span de a ~10 c."""
    cache = {}
    def logmag(cc):
        if cc not in cache:
            cache[cc] = math.log(goertzel_fast(x, 2.0 * math.cos(2.0 * math.pi * detune(center, cc) / sr)) + 1e-300)
        return cache[cc]
    if prev is not None:
        r = refine_peak(logmag, prev, 1.5, 20.0)
        if r is not None and abs(r) <= span:
            return r
    k = max(2, round(2.0 * span / 10.0))
    step = 2.0 * span / k
    grid = [-span + step * i for i in range(k + 1)]
    ms = [logmag(g) for g in grid]
    i = max(range(len(ms)), key=ms.__getitem__)
    if i == 0 or i == len(ms) - 1:
        return None                             # el pico esta en el borde: fuera del rango declarado
    a, b, d = ms[i - 1], ms[i], ms[i + 1]
    c0 = grid[i] + 0.5 * step * (a - d) / (a - 2.0 * b + d)
    r = refine_peak(logmag, c0, 1.5, 15.0)
    return r if r is not None and abs(r) <= span else None

def short_track(mono, sr, center, win_s=0.1, hop_s=0.05, floor_db=-60.0, span=150.0, offset=0):
    """Altura instantanea por tramos cortos. Devuelve [(t_ini, t_fin, hz, cents_vs_centro, dbfs)].

    Ventana Hann de max(win_s, 10 periodos del centro); hop fijo. Bajo `floor_db` (RMS del tramo) o
    sin pico en ±span el hz es nan: no hay altura que declarar, y no se inventa una. `offset` es el
    frame del archivo en que empieza `mono` (un segmento): los tiempos salen del archivo."""
    n = int(round(max(win_s, SHORT_PERIODS / center) * sr))
    hop = max(1, int(round(hop_s * sr)))
    hann = hann_window(n)
    rows, prev, prev_db = [], None, None
    for start in range(0, len(mono) - n + 1, hop):
        seg = mono[start:start + n]
        rms = math.sqrt(sum(map(operator.mul, seg, seg)) / n)
        db = 20.0 * math.log10(rms) if rms > 0.0 else -math.inf
        c = None
        if db >= floor_db:
            # Un salto de nivel (ataque nuevo, fin de nota) invalida el seguimiento: se re-barre.
            if prev_db is not None and abs(db - prev_db) > 10.0:
                prev = None
            c = locate(list(map(operator.mul, seg, hann)), sr, center, span, prev)
        prev, prev_db = c, db
        hz, cc = (math.nan, math.nan) if c is None else (detune(center, c), c)
        rows.append(((start + offset) / sr, (start + offset + n) / sr, hz, cc, db))
    return rows

# --- REQ-055 (D11): los ATAQUES, detectados por el oraculo -----------------------------------------
#
# El instrumento mide POR ATAQUE (AC-055.2): t0 de cada punteo lo fija ESTE script, nunca el motor
# (R-API-48). No puede depender de que el nivel caiga bajo 0,001: en el g42 la sala esta a -48,5 dBFS
# y ninguna ventana baja de ahi. Un ataque es una SUBIDA de energia respecto del nivel previo, con la
# cuerda sonando o no. El metodo, con sus parametros fijos:
#
#   · dos energias: la cruda (x²) y la de la primera diferencia ((x[n]-x[n-1])²), que realza el
#     transitorio de banda ancha de la pua o el dedo. La cruda sola no ve un re-punteo agudo sobre una
#     cuerda grave fuerte; la diferencia sola no ve una nota grave pura bajo ruido blanco (la diferencia
#     atenua E2 en -39 dB);
#   · en cada punto t de una grilla de ONSET_BLOCK frames desde el frame 0: ANTES = la media de cada
#     energia en [t - ONSET_WINDOW, t), DESPUES = en [t, t + ONSET_WINDOW). La ventana (2048 frames,
#     ~2 periodos de E1) es larga a proposito: la energia de un bloque de 128 frames de un seno grave
#     oscila con la fase y abria un «ataque» cada refractario en los bajos de corpus-v1;
#   · los frames anteriores al archivo valen el FONDO del archivo (el percentil ONSET_BACKGROUND_PCT de
#     la energia de sus bloques de 128): una toma que arranca en el ruido de sala no abre un ataque en el
#     frame 0, y un render que arranca con la nota, si;
#   · candidato en t si el rms crudo de DESPUES >= ONSET_FLOOR_DBFS y alguna energia de DESPUES supera a
#     la de ANTES en >= ONSET_RISE_DB;
#   · t0 = el inicio del PRIMER bloque de 128 en [t, t + ONSET_WINDOW) con rms crudo >= el piso y cuya
#     energia (de la que subio) supera a la de ANTES en ONSET_RISE_DB: el candidato llega antes que el ataque (su ventana de
#     despues ya lo contiene), el bloque lo fecha;
#   · refractario: el siguiente candidato se busca desde t0 + ONSET_REFRACTORY_S.
#
# Tolerancia declarada: ONSET_TOLERANCE_FRAMES contra el frame verdadero del ataque (la grilla de 128
# mas el bloque en que la subida todavia no alcanza), probada en `self_test_attacks`. Lo que NO
# detecta, a proposito: una subida de menos de ONSET_RISE_DB en las dos energias (un re-punteo apenas
# mas fuerte que lo que ya sonaba), y dos ataques a menos del refractario (cuentan como uno).
ONSET_BLOCK = 128
ONSET_WINDOW = 2048
ONSET_RISE_DB = 6.0
ONSET_FLOOR_DBFS = -60.0
ONSET_REFRACTORY_S = 0.15
ONSET_TOLERANCE_FRAMES = 256
ONSET_BACKGROUND_PCT = 0.10

def detect_attacks(mono, sr, block=ONSET_BLOCK, window=ONSET_WINDOW, rise_db=ONSET_RISE_DB,
                   floor_dbfs=ONSET_FLOOR_DBFS, refractory_s=ONSET_REFRACTORY_S):
    """Los frames de cada ataque de `mono` (ver arriba). Lista ordenada de enteros."""
    n = len(mono)
    if n < block:
        return []
    rise = 10.0 ** (rise_db / 10.0)
    floor = 10.0 ** (floor_dbfs / 10.0)           # sobre la energia (rms²)
    refractory = int(round(refractory_s * sr))
    # Sumas acumuladas de las dos energias: cualquier media de un tramo sale en O(1).
    cr, cd = [0.0], [0.0]
    prev = 0.0
    for v in mono:
        cr.append(cr[-1] + v * v)
        d = v - prev
        cd.append(cd[-1] + d * d)
        prev = v
    blocks = n // block
    pct = lambda c: sorted((c[(k + 1) * block] - c[k * block]) / block for k in range(blocks))[
        min(blocks - 1, int(ONSET_BACKGROUND_PCT * blocks))]
    bg = (pct(cr), pct(cd))
    def mean(c, b, lo, hi):                       # media en [lo, hi); lo < 0 vale el fondo
        pre = max(0, -lo)
        lo, hi = max(lo, 0), min(hi, n)
        return ((c[hi] - c[lo]) + pre * b) / ((hi - lo) + pre) if hi > lo or pre else 0.0
    out = []
    t = 0
    while t + block <= n:
        before = (mean(cr, bg[0], t - window, t), mean(cd, bg[1], t - window, t))
        after = (mean(cr, bg[0], t, t + window), mean(cd, bg[1], t, t + window))
        rose = [i for i in (0, 1) if after[i] >= rise * before[i]]
        if after[0] < floor or not rose:
            t += block
            continue
        t0 = None
        for k in range(t, min(t + window, n - block + 1), block):
            if (cr[k + block] - cr[k]) / block < floor:
                continue                          # el bloque tiene que tener senal, no solo subir
            if any(((cr, cd)[i][k + block] - (cr, cd)[i][k]) / block >= rise * before[i] for i in rose):
                t0 = k
                break
        if t0 is None:                            # la subida esta en la cola parcial: no se fecha
            t += block
            continue
        out.append(t0)
        t = t0 + refractory
        t += (-t) % block                         # vuelve a la grilla
    return out

def segments_of(n, cuts):
    """Los segmentos [a, b) de un archivo de `n` frames que ningun corte toca. Un corte [ini, fin]
    deja [.., ini) antes y desde `fin` despues; los frames de [ini, fin) de una costura no son de nadie."""
    segs, cursor = [], 0
    for ini, fin, _ in cuts:
        ini, fin = min(ini, n), min(fin, n)
        if ini > cursor: segs.append((cursor, ini))
        cursor = max(cursor, fin)
    if n > cursor: segs.append((cursor, n))
    return segs

def detect_attacks_cut(mono, sr, cuts):
    """`detect_attacks` por segmento (el salto de un corte no es un ataque que el detector deba ver
    a traves): frames del archivo, ordenados."""
    return [a + t for a, b in segments_of(len(mono), cuts) for t in detect_attacks(mono[a:b], sr)]

def short_track_cut(mono, sr, cuts, center, *args):
    """`short_track` por segmento: ninguna ventana mezcla frames de dos lados de un corte."""
    return [r for a, b in segments_of(len(mono), cuts) for r in short_track(mono[a:b], sr, center, *args, offset=a)]

def split_attacks(attacks, n, cuts):
    """(conservados, excluidos). El ataque t0 dura hasta tEnd = el t0 del siguiente (o `n`); su guarda
    es [t0 - ONSET_WINDOW, tEnd] cerrado: se excluye si toca un corte [ini, fin]. REGLA EXACTA que
    comparte el instrumento C++: no tocar un borde en uno solo de los dos lados."""
    kept, excluded = [], []
    for i, t0 in enumerate(attacks):
        t_end = attacks[i + 1] if i + 1 < len(attacks) else n
        hit = any(ini <= t_end and fin >= t0 - ONSET_WINDOW for ini, fin, _ in cuts)
        (excluded if hit else kept).append(t0)
    return kept, excluded

def format_attacks(name, attacks, excluded=()):
    tagged = sorted([(a, "ataque") for a in attacks] + [(a, "ataque-excluido") for a in excluded])
    return [f"# {tag} {name} frame {a}" for a, tag in tagged]

def format_cuts(name, cuts):
    return [f"# corte {name} {ini} {fin} {'?' if lost is None else lost}" for ini, fin, lost in cuts]

def format_short(name, rows):
    nan = lambda v, f: "nan" if v != v else format(v, f)
    return ["\t".join((name, f"{t0:.4f}", f"{t1:.4f}", nan(hz, ".5f"), nan(c, "+.3f"), f"{db:.2f}"))
            for t0, t1, hz, c, db in rows]

def self_test_partials():
    """El oraculo por parcial contra una serie CONOCIDA, en las dos direcciones. Devuelve los fallos."""
    import tempfile
    sr, secs, f0 = 44100, 6.0, detune(82.40689, 3.0)     # E2, +3 c: el error no puede ser 0
    def write_wav(path, partial_cents):
        n = int(secs * sr)
        buf = [0.0] * n
        for k, dc in partial_cents.items():
            fk = detune(k * f0, dc)
            w = 2.0 * math.pi * fk / sr
            a = 0.3 / k
            for i in range(n): buf[i] += a * math.sin(w * i)
        with wave.open(path, 'wb') as wv:
            wv.setnchannels(2); wv.setsampwidth(2); wv.setframerate(sr)
            wv.writeframes(b''.join(struct.pack('<hh', int(v * 32767), int(v * 32767)) for v in buf))
    failures = []
    with tempfile.TemporaryDirectory() as d:
        for B in (1e-4, 5e-4):
            p = os.path.join(d, f"serie_B{B:g}.wav")
            write_wav(p, {k: stretch_cents(B, k) for k in range(1, 7)})
            res = measure_partials(p, 82.40689, 4)
            # Respecto de H1 (que esta en f0·√(1+B)), no de f0: ver el bloque `C NO ES H1`.
            for n in range(1, 5):
                want = stretch_cents(B, n) - stretch_cents(B, 1)
                got = res[n]["cents"]
                if abs(got - want) > 0.1:
                    failures.append(f"B={B:g} parcial {n}: {got:+.3f} c contra {want:+.3f} esperado")
            Bf, Cf, resid = fit_stretched({n: res[n]["cents"] for n in res})
            if abs(Bf / B - 1.0) > 0.10: failures.append(f"B={B:g}: B_fit {Bf:.3e} fuera del 10 %")
            if abs(Cf + stretch_cents(B, 1)) > 0.1:
                failures.append(f"B={B:g}: C_fit {Cf:+.3f} c, tenia que ser {-stretch_cents(B, 1):+.3f} ± 0,1")
            h1 = cents(res[1]["hz"], 82.40689)
            if abs(h1 - 3.0 - stretch_cents(B, 1)) > 0.1:
                failures.append(f"B={B:g}: H1 leyo {h1:+.3f} c, tenia que ser {3.0 + stretch_cents(B, 1):+.3f}")
        # La otra direccion: un parcial que NO sigue la serie tiene que salir como residuo.
        p = os.path.join(d, "p3_corrido.wav")
        c = {k: stretch_cents(1e-4, k) for k in range(1, 7)}; c[3] += 10.0
        write_wav(p, c)
        res = measure_partials(p, 82.40689, 4)
        Bf, Cf, resid = fit_stretched({n: res[n]["cents"] for n in res})
        if max(abs(v) for v in resid.values()) <= 2.0:
            failures.append(f"p3 +10 c: res_max {max(abs(v) for v in resid.values()):.2f} c no delata al parcial corrido")
        if abs(res[3]["cents"] - (stretch_cents(1e-4, 3) - stretch_cents(1e-4, 1) + 10.0)) > 0.1:
            failures.append(f"p3 +10 c: el oraculo leyo {res[3]['cents']:+.3f} c en el parcial 3")
    return failures

def riff(fmt_body, payload, wmad=None):
    """Arma un WAV a mano (el modulo `wave` no escribe float): fmt + [wmad] + data."""
    chunks = [(b'fmt ', fmt_body)]
    if wmad is not None: chunks.append((b'wmad', wmad))
    chunks.append((b'data', payload))
    body = b'WAVE' + b''.join(cid + struct.pack('<I', len(b)) + b + (b'\0' if len(b) & 1 else b'')
                              for cid, b in chunks)
    return b'RIFF' + struct.pack('<I', len(body)) + body

def fmt_chunk(tag, ch, sr, bits, extensible=None):
    width = bits // 8
    base = struct.pack('<HHIIHH', WAVE_FORMAT_EXTENSIBLE if extensible else tag, ch, sr, sr * ch * width, ch * width, bits)
    if not extensible:
        return base
    guid = struct.pack('<H', extensible) + bytes.fromhex("000000001000800000aa00389b71")
    return base + struct.pack('<HHI', 22, bits, 0) + guid

def wmad_chunk(version=1, written=1000, lost=0, disc=0, rate=44100, source=1, valid=1):
    return struct.pack(WMAD_FMT, version, written, lost, disc, rate, source, valid)

def wmad_v2_chunk(written=1000, lost=0, disc=0, rate=44100, source=1, valid=1, unl_frames=0, unl_disc=0,
                  cuts=(), count=None, cap=256, slots=None, version=2):
    """wmad v2 a mano. `cuts` = [(fileFrame, lostFrames, fileSpan, kind)]; `slots` fuerza cuantos
    slots lleva el cuerpo (default `cap`)."""
    body = struct.pack(WMAD_FMT, version, written, lost, disc, rate, source, valid)
    body += struct.pack(WMAD_V2_FMT, unl_frames, unl_disc, len(cuts) if count is None else count, cap)
    slot = [struct.pack(WMAD_CUT_FMT, *c) for c in cuts]
    slot += [b'\0' * WMAD_CUT_SIZE] * ((cap if slots is None else slots) - len(slot))
    return body + b''.join(slot)

def f32(v): return struct.unpack('<f', struct.pack('<f', v))[0]

def self_test_formats():
    """Float32, EXTENSIBLE y el rechazo de tomas invalidas. Devuelve los fallos."""
    import tempfile
    failures = []
    global ALLOW_INVALID
    ALLOW_INVALID = False
    # |x| > 1 y valores que un entero de 16 bits no puede representar (1/3, 1e-6, 1e-9).
    mono_vals = [f32(v) for v in (0.0, 1.0, -1.0, 1.5, -2.25, 1.0 / 3.0, 1e-6, -1e-9, 123.456, 0.99999994)]
    L = mono_vals
    R = [f32(v) for v in (0.25, -1.0, 1.0, 0.5, 2.25, -1.0 / 7.0, 3e-6, 1e-9, -23.456, 0.5)]
    pack = lambda vals: struct.pack('<%df' % len(vals), *vals)
    stereo = [x for lr in zip(L, R) for x in lr]
    with tempfile.TemporaryDirectory() as d:
        def read(name, data):
            p = os.path.join(d, name)
            with open(p, 'wb') as f: f.write(data)
            return read_wav(p)
        got, sr, info = read("m.wav", riff(fmt_chunk(3, 1, 48000, 32), pack(mono_vals)))
        if got != mono_vals or sr != 48000 or info["frames"] != len(mono_vals):
            failures.append(f"float32 mono no vuelve bit a bit: {got[:4]} ...")
        got, sr, _ = read("s.wav", riff(fmt_chunk(3, 2, 44100, 32), pack(stereo)))
        if got != [0.5 * (l + r) for l, r in zip(L, R)]:
            failures.append("float32 estereo no es 0,5·(L+R) exacto")
        got, _, _ = read("xf.wav", riff(fmt_chunk(3, 1, 44100, 32, extensible=3), pack(mono_vals)))
        if got != mono_vals: failures.append("EXTENSIBLE float no vuelve bit a bit")
        ints = [0, 1, -1, 32767, -32768, 12345]
        got, _, _ = read("xp.wav", riff(fmt_chunk(1, 1, 44100, 16, extensible=1), struct.pack('<6h', *ints)))
        if got != [v / 32768.0 for v in ints]: failures.append("EXTENSIBLE PCM16 mal leido")
        got, _, _ = read("p.wav", riff(fmt_chunk(1, 2, 44100, 16), struct.pack('<12h', *[x for lr in zip(ints, ints[::-1]) for x in lr])))
        if got != [(a + b) / 65536.0 for a, b in zip(ints, ints[::-1])]:
            failures.append("PCM16 estereo ya no es (L+R)/65536")
        for what, data in (("24 bits", riff(fmt_chunk(1, 1, 44100, 24), b'\0' * 9)),
                           ("3 canales", riff(fmt_chunk(3, 3, 44100, 32), b'\0' * 12)),
                           ("float64", riff(fmt_chunk(3, 1, 44100, 64), b'\0' * 16)),
                           ("NaN", riff(fmt_chunk(3, 1, 44100, 32), pack([float('nan')])))):
            try: read("bad.wav", data); failures.append(f"{what}: tenia que rechazarse")
            except SystemExit: pass
        # wmad: el valido pasa; cada causa de rechazo, por separado.
        mk = lambda **kw: wmad_chunk(written=len(mono_vals), **kw)
        ok = riff(fmt_chunk(3, 1, 44100, 32), pack(mono_vals), mk())
        got, _, info = read("ok.wav", ok)
        if got != mono_vals or info["wmad"]["frames_written"] != len(mono_vals):
            failures.append("wmad valido: no se acepto o no se parseo")
        pay = pack(mono_vals)
        cases = (("valid=0", "v0.wav", mk(valid=0)), ("lost>0", "l.wav", mk(lost=3)),
                 ("discontinuities>0", "dc.wav", mk(disc=1)),
                 ("rate distinto", "r.wav", mk(rate=48000)),
                 ("version 2", "v2.wav", mk(version=2)),
                 ("nombre .invalid.wav", "toma.invalid.wav", mk()),
                 ("framesWritten distinto de los frames", "fw.wav", wmad_chunk(written=len(mono_vals) + 1)))
        for what, name, wm in cases:
            data = riff(fmt_chunk(3, 1, 44100, 32), pay, wm)
            ALLOW_INVALID = False
            try: read(name, data); failures.append(f"wmad {what}: tenia que rechazarse")
            except SystemExit: pass
            ALLOW_INVALID = True
            try: read(name, data)
            except SystemExit: failures.append(f"wmad {what}: --allow-invalid tenia que dejarlo pasar")
            ALLOW_INVALID = False
        # data truncado: el header declara mas bytes de los que hay (toma cortada a mitad de copia).
        cut = riff(fmt_chunk(3, 1, 44100, 32), pay, mk())
        cut = cut[:-12]
        ALLOW_INVALID = False
        try: read("cut.wav", cut); failures.append("float32 con wmad y data truncado: tenia que rechazarse")
        except SystemExit: pass
        ALLOW_INVALID = True
        try: read("cut.wav", cut)
        except SystemExit: failures.append("data truncado: --allow-invalid tenia que dejarlo pasar")
        ALLOW_INVALID = False
        # La tolerancia vieja sigue: PCM16 sin wmad con data declarado de mas se lee.
        short = riff(fmt_chunk(1, 1, 44100, 16), struct.pack('<6h', *ints))[:-4]
        try: got, _, _ = read("old.wav", short)
        except SystemExit: failures.append("PCM16 sin wmad truncado: la tolerancia vieja se perdio")
        else:
            if got != [v / 32768.0 for v in ints[:4]]: failures.append("PCM16 truncado: frames mal leidos")
    return failures

def self_test_cuts():
    """wmad v2 (cortes), segmentos y exclusion de ataques. Devuelve los fallos."""
    import tempfile
    failures = []
    global ALLOW_INVALID
    n = 1000
    pay = struct.pack('<%df' % n, *([0.0] * n))
    mk = lambda **kw: wmad_v2_chunk(**kw)
    good = [(100, 7, 0, CUT_LOSS), (400, 0, 50, CUT_SEAM), (900, 5, 0, CUT_LOSS)]
    with tempfile.TemporaryDirectory() as d:
        def read(name, wm, payload=pay):
            p = os.path.join(d, name)
            with open(p, 'wb') as f: f.write(riff(fmt_chunk(3, 1, 44100, 32), payload, wm))
            return read_wav(p)
        _, _, info = read("v2.wav", mk(lost=12, disc=3, cuts=good))
        if info["wmad"]["cuts"] != [(100, 100, 7), (400, 450, None), (900, 900, 5)]:
            failures.append(f"v2 valido: cortes mal parseados {info['wmad']['cuts']}")
        _, _, info = read("v2sin.wav", mk())
        if info["wmad"]["cuts"] != []: failures.append("v2 sin cortes: tenia que dar lista vacia")
        _, _, info = read("v1.wav", wmad_chunk(written=n))
        if info["wmad"]["cuts"] != []: failures.append("v1 valida: tenia que dar lista vacia")
        cases = (("chunk mas largo que 64+24*cap", mk(cuts=good, lost=12, slots=257)),
                 ("chunk mas corto que 64+24*cap", mk(cuts=good, lost=12, slots=255)),
                 ("cutCount > cutCapacity", mk(cuts=good, lost=12, count=257)),
                 ("cutCount mayor que la capacidad chica", mk(cuts=good, lost=12, cap=3, count=4)),
                 ("perdida con fileSpan != 0", mk(cuts=[(100, 7, 5, CUT_LOSS)], lost=7)),
                 ("costura con lostFrames != 0", mk(cuts=[(100, 7, 5, CUT_SEAM)], lost=7)),
                 ("kind 3", mk(cuts=[(100, 7, 0, 3)], lost=7)),
                 ("kind 0", mk(cuts=[(100, 7, 0, 0)], lost=7)),
                 ("cortes desordenados", mk(cuts=[(400, 5, 0, CUT_LOSS), (399, 7, 0, CUT_LOSS)], lost=12)),
                 ("perdida fuera de framesWritten", mk(cuts=[(n + 1, 7, 0, CUT_LOSS)], lost=7)),
                 ("costura que se pasa de framesWritten", mk(cuts=[(990, 0, 11, CUT_SEAM)])),
                 ("valid=1 con unlocatedFrames", mk(unl_frames=3, lost=3)),
                 ("valid=1 con unlocatedDiscontinuities", mk(unl_disc=1, disc=1)),
                 ("framesLost != suma", mk(cuts=good, lost=13)),
                 ("framesLost < suma", mk(cuts=good, lost=11)),
                 ("valid=0", mk(cuts=good, lost=12, valid=0)),
                 ("rate distinto", mk(cuts=good, lost=12, rate=48000)),
                 ("framesWritten distinto de los frames", mk(cuts=good, lost=12, written=n + 1)),
                 ("version 3", mk(cuts=good, lost=12, version=3)))
        for what, wm in cases:
            ALLOW_INVALID = False
            try: read("c.wav", wm); failures.append(f"wmad v2 {what}: tenia que rechazarse")
            except SystemExit: pass
        # un borde exacto SI pasa: costura que termina en framesWritten y perdida en el mismo frame que otra.
        ALLOW_INVALID = False
        try: read("borde.wav", mk(cuts=[(500, 4, 0, CUT_LOSS), (500, 3, 0, CUT_LOSS), (990, 0, 10, CUT_SEAM)], lost=7))
        except SystemExit as e: failures.append(f"wmad v2 en el borde exacto: se rechazo ({e})")
        # unlocated>0 con valid=0 y --allow-invalid se lee sin romper; ni el chunk de tamano malo.
        ALLOW_INVALID = True
        for wm in (mk(unl_frames=3, lost=3, valid=0), mk(cuts=good, lost=12, slots=257), mk(count=999)):
            try: read("a.wav", wm)
            except SystemExit as e: failures.append(f"wmad v2 incoherente con --allow-invalid: {e}")
        ALLOW_INVALID = False
    # segmentos
    if segments_of(100, [(40, 40, 3)]) != [(0, 40), (40, 100)]: failures.append("segmentos de una perdida")
    if segments_of(100, [(40, 60, None)]) != [(0, 40), (60, 100)]: failures.append("segmentos de una costura")
    if segments_of(100, [(40, 60, None), (50, 50, 2)]) != [(0, 40), (60, 100)]: failures.append("perdida dentro de costura")
    if segments_of(100, [(0, 0, 1), (100, 100, 1)]) != [(0, 100)]: failures.append("cortes en los extremos")
    if segments_of(100, []) != [(0, 100)]: failures.append("sin cortes")
    # un corte de perdida en medio de una nota sostenida, con salto de fase: ninguna ventana lo cruza
    sr, F = 44100, 44100
    G3 = NOMINAL["G3"]
    a_part = synth(sr, 1.0, lambda t: G3)
    b_part = [v for v in synth(sr, 1.0, lambda t: G3)][777:]     # el mismo tono, con otra fase: un salto
    sig = a_part[:F] + b_part
    cuts = [(F, F, 777)]
    rows = short_track_cut(sig, sr, cuts, G3)
    wins = [(round(r[0] * sr), round(r[1] * sr)) for r in rows]
    if not wins or not any(b <= F for a, b in wins) or not any(a >= F for a, b in wins):
        failures.append("corte sostenido: faltan tramos de un lado")
    if any(a < F < b for a, b in wins): failures.append("corte sostenido: un tramo cruza el corte")
    if any(r[3] != r[3] or abs(r[3]) > 0.1 for r in rows):
        failures.append("corte sostenido: un tramo lee otra altura que el tono")
    found = detect_attacks_cut(sig, sr, cuts)
    if found != [0, F]:      # el segmento posterior arranca con la nota: el detector lo ve como ataque, y se excluye
        failures.append(f"corte sostenido: el detector corre por segmento y esperaba [0, {F}], dio {found}")
    kept, excl = split_attacks(found, len(sig), cuts)
    if kept: failures.append(f"corte sostenido: ataque sin excluir {kept}")
    # exclusion: borde +-1. Un solo ataque, tEnd = n.
    n2 = 50000
    for fin, want in ((27952, [30000]), (27951, [])):
        k, e = split_attacks([30000], n2, [(20000, fin, None)])
        if e != want or k != ([] if want else [30000]): failures.append(f"borde de la guarda (fin {fin}): excluidos {e}")
    # y el borde de tEnd: dos ataques, una perdida en el tEnd del primero o un frame despues.
    for f, want_k in ((40000, []), (40001, [10000])):
        k, e = split_attacks([10000, 40000], n2, [(f, f, 1)])
        if k != want_k or e != sorted(set([10000, 40000]) - set(want_k)):
            failures.append(f"borde de tEnd (perdida en {f}): conservados {k}, excluidos {e}")
    if split_attacks([30000], n2, [(20000, 20000, 1)])[0] != [30000]: failures.append("corte lejano: no debia excluir")
    if split_attacks([30000], n2, [])[0] != [30000]: failures.append("sin cortes: no debia excluir")
    # formato
    if format_cuts("a.wav", [(5, 5, 3), (7, 9, None)]) != ["# corte a.wav 5 5 3", "# corte a.wav 7 9 ?"]:
        failures.append("formato de '# corte'")
    if format_attacks("a.wav", [1, 9], [4]) != ["# ataque a.wav frame 1", "# ataque-excluido a.wav frame 4", "# ataque a.wav frame 9"]:
        failures.append("formato de '# ataque-excluido'")
    return failures

def synth(sr, secs, hz_of_t, harmonics=6):
    """Tono con armonicos (amplitud 1/k) y FASE CONTINUA: la fase integra la frecuencia, no la evalua."""
    n, ph, w = int(secs * sr), 0.0, 2.0 * math.pi / sr
    amp = 0.3 / sum(1.0 / k for k in range(1, harmonics + 1))
    out = []
    for i in range(n):
        out.append(amp * sum(math.sin(k * ph) / k for k in range(1, harmonics + 1)))
        ph += w * hz_of_t(i / sr)
    return out

def self_test_short():
    """El oraculo corto contra tonos y glides CONOCIDOS. Devuelve los fallos."""
    failures = []
    G3 = NOMINAL["G3"]
    # (c) tono estable con armonicos: cada tramo con senal a <= 0,1 c de la verdad.
    for sr in (44100, 48000):
        for name, center, off in (("E1", NOMINAL["E1"], 0.0), ("E2+3c", NOMINAL["E2"], 3.0),
                                  ("E4", NOMINAL["E4"], 0.0), ("A4-7c", NOMINAL["A4"], -7.0)):
            hz = detune(center, off)
            rows = short_track(synth(sr, 1.2, lambda t: hz), sr, center)
            if not rows: failures.append(f"{name}@{sr}: sin tramos")
            for t0, t1, h, c, db in rows:
                if not abs(c - off) <= 0.1:       # `not <=` atrapa tambien el nan
                    failures.append(f"{name}@{sr} t={t0:.2f}: {c:+.3f} c contra {off:+.3f}"); break
    # (d) glide de ataque con fase continua: c(t) = 2 - 20·exp(-t/0,3) c sobre G3, 2 s. El tramo ve el
    # glide promediado con su Hann: se compara con ese promedio ponderado.
    sr = 44100
    ct = lambda t: 2.0 - 20.0 * math.exp(-t / 0.3)
    rows = short_track(synth(sr, 2.0, lambda t: detune(G3, ct(t))), sr, G3)
    worst = 0.0
    for t0, t1, h, c, db in rows:
        n = int(round((t1 - t0) * sr)); hw = hann_window(n)
        want = sum(w * ct(t0 + (i + 0.5) / sr) for i, w in enumerate(hw)) / sum(hw)
        worst = max(worst, abs(c - want)) if c == c else math.inf
    if not worst <= 0.3: failures.append(f"glide: peor tramo a {worst:.3f} c del promedio de la ventana (tope 0,3)")
    if len(rows) < 30: failures.append(f"glide: solo {len(rows)} tramos (el hop no se respeta)")
    # (e) las dos direcciones del rango: no se pega al nominal. Y el silencio no tiene altura.
    for off in (40.0, -120.0):
        hz = detune(G3, off)
        rows = short_track(synth(sr, 0.6, lambda t: hz), sr, G3)
        for t0, t1, h, c, db in rows:
            if not abs(c - off) <= 0.1: failures.append(f"{off:+.0f} c: leyo {c:+.3f}"); break
    rows = short_track([0.0] * sr, sr, G3)
    if not rows or any(r[2] == r[2] for r in rows): failures.append("silencio: algun tramo declara hz")
    # El hop se respeta: los inicios avanzan de a hop_s.
    ts = [r[0] for r in short_track(synth(sr, 0.6, lambda t: G3), sr, G3, hop_s=0.1)]
    if any(abs(b - a - 0.1) > 1e-6 for a, b in zip(ts, ts[1:])) or len(ts) < 2:
        failures.append(f"hop 0,1 s: inicios {ts}")
    return failures

def noise(n, dbfs, seed=55):
    """Ruido blanco uniforme determinista con rms `dbfs` (LCG: sin depender del `random` de la version)."""
    amp = 10.0 ** (dbfs / 20.0) * math.sqrt(3.0)
    x, out = seed, []
    for _ in range(n):
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        out.append(amp * (2.0 * x / 0x7FFFFFFF - 1.0))
    return out

def pluck(sr, secs, hz, tau, amp=0.3, harmonics=6):
    """Una nota punteada: armonicos 1/k con decaimiento exponencial `tau` s desde el frame 0."""
    n = int(secs * sr)
    norm = amp / sum(1.0 / k for k in range(1, harmonics + 1))
    w = 2.0 * math.pi * hz / sr
    return [norm * math.exp(-i / (tau * sr)) * sum(math.sin(k * w * i) / k for k in range(1, harmonics + 1))
            for i in range(n)]

def mix_at(base, add, at):
    out = list(base)
    for i, v in enumerate(add):
        if at + i < len(out): out[at + i] += v
    return out

def self_test_attacks():
    """El detector de ataques contra onsets CONOCIDOS. Devuelve los fallos."""
    failures = []
    tol = ONSET_TOLERANCE_FRAMES
    def check(label, got, want):
        if len(got) != len(want) or any(abs(g - w) > tol for g, w in zip(got, want)):
            failures.append(f"{label}: detecto {got}, esperaba {want} (±{tol} frames)")
    for sr in (44100, 48000):
        # (a) silencio digital y una nota a mitad de bloque: un ataque, en su frame.
        sig = mix_at([0.0] * (2 * sr), pluck(sr, 1.5, NOMINAL["G3"], 0.8), 10_037)
        check(f"desde silencio@{sr}", detect_attacks(sig, sr), [10_037])
        # (b) ruido de sala a -48 dBFS solo, 3 s: ningun ataque (con el fondo encima del 0,001 del motor).
        check(f"ruido -48 dBFS@{sr}", detect_attacks(noise(3 * sr, -48.0), sr), [])
        # (c) sobre ese fondo: un punteo y un RE-PUNTEO con la cuerda sonando (decayo e^-1,2: -10,4 dB)
        # sin que ninguna ventana baje del fondo, y una nota grave pura (E2) que solo ve la energia cruda.
        base = noise(5 * sr, -48.0)
        a1, a2, a3 = int(0.5 * sr) + 77, int(1.7 * sr) + 5, int(3.6 * sr) + 200
        sig = mix_at(base, pluck(sr, 1.2, NOMINAL["G3"], 1.0), a1)
        sig = mix_at(sig, pluck(sr, 1.6, NOMINAL["G3"], 1.0), a2)
        sig = mix_at(sig, [0.05 * math.sin(2.0 * math.pi * NOMINAL["E2"] * i / sr) for i in range(sr)], a3)
        check(f"ruido + punteo + re-punteo + E2@{sr}", detect_attacks(sig, sr), [a1, a2, a3])
        # (d) re-punteo AGUDO sobre una cuerda grave fuerte: la energia cruda casi no sube (< 1 dB),
        # el transitorio de la diferencia si. Atrapa un detector con una sola de las dos energias.
        low = pluck(sr, 2.0, NOMINAL["E2"], 3.0, amp=0.5, harmonics=1)
        a4 = int(1.0 * sr) + 31
        sig = mix_at(low, pluck(sr, 0.8, NOMINAL["E4"], 0.5, amp=0.2, harmonics=8), a4)
        check(f"re-punteo agudo sobre grave@{sr}", detect_attacks(sig, sr), [0, a4])
    # (e) el refractario: dos ataques a 50 ms son uno; a 300 ms, dos.
    sr = 48000
    for gap_s, want in ((0.05, 1), (0.30, 2)):
        sig = mix_at([0.0] * sr, pluck(sr, 0.2, 440.0, 0.02), 4800)
        sig = mix_at(sig, pluck(sr, 0.5, 440.0, 0.3), 4800 + int(gap_s * sr))
        got = detect_attacks(sig, sr)
        if len(got) != want: failures.append(f"refractario {gap_s} s: {len(got)} ataques ({got}), esperaba {want}")
    # (f) una nota que decae sola no tiene segundo ataque, y bajo el piso no hay ninguno.
    check("decae sola", detect_attacks(pluck(sr, 3.0, 110.0, 0.7), sr), [0])
    check("bajo el piso", detect_attacks(noise(sr, -66.0), sr), [])
    return failures

def self_test():
    """El instrumento contra series CONOCIDAS, en las dos direcciones. Sale con 1 si falla."""
    failures = self_test_partials() + self_test_formats() + self_test_short() + self_test_attacks() + self_test_cuts()
    for f in failures: print("SELF-TEST FALLO:", f)
    print("self-test:", "ROJO" if failures else "verde", f"({len(failures)} fallos)")
    return 1 if failures else 0

def nominal_for(name):
    cuerda = os.path.splitext(name)[0].rsplit('_', 1)[-1]
    if cuerda not in NOMINAL:
        raise SystemExit(f"{name}: no se que cuerda es '{cuerda}'; pasa --nominal")
    return NOMINAL[cuerda]

def run_short(a, files):
    out = open(a.tsv, 'w') if a.tsv else sys.stdout
    try:
        print("# comando: " + " ".join(sys.argv), file=out)
        print(f"# ventana_min_s {a.short_win} (y >= {SHORT_PERIODS} periodos del centro, Hann) "
              f"hop_s {a.short_hop} span_cents ±{a.span_cents} piso_dbfs {a.short_floor_dbfs}", file=out)
        print(f"# ataques (REQ-055 D11): grilla {ONSET_BLOCK} frames desde el frame 0, antes/despues = media en "
              f"{ONSET_WINDOW} frames, subida >= {ONSET_RISE_DB} dB en la energia cruda o en la de la primera "
              f"diferencia, fondo previo = percentil {ONSET_BACKGROUND_PCT} de los bloques, piso "
              f"{ONSET_FLOOR_DBFS} dBFS, refractario {ONSET_REFRACTORY_S} s, tolerancia "
              f"{ONSET_TOLERANCE_FRAMES} frames. Linea: '# ataque ARCHIVO frame N'", file=out)
        print("# archivo\tt_ini_s\tt_fin_s\thz\tcents_vs_centro\tnivel_dbfs", file=out)
        cuts_doc = False
        for path in files:
            name = os.path.basename(path)
            mono, sr, info = read_wav(path)
            center = a.nominal or nominal_for(name)
            print(f"# archivo {name} sha256 {info['sha256']} rate {sr} frames {info['frames']}", file=out)
            cuts = info["wmad"]["cuts"] if info["wmad"] else []
            if cuts and not cuts_doc:
                cuts_doc = True       # solo si hay cortes: sin ellos la cabecera es la de siempre, byte a byte
                print("# cortes (REQ-055 0.6): intervalo cerrado [INI, FIN] en frames del archivo; perdida: INI=FIN, "
                      "faltan PERDIDOS frames justo antes de INI; costura de captura: el salto cae en un frame de "
                      "[INI, FIN], PERDIDOS '?'. Linea: '# corte ARCHIVO INI FIN PERDIDOS'. Detector y tramos corren por "
                      f"segmento; el ataque cuya guarda [t0 - {ONSET_WINDOW}, t0 del siguiente o fin] toca un corte va "
                      "como '# ataque-excluido ARCHIVO frame N'", file=out)
            if cuts:
                print("\n".join(format_cuts(name, cuts)), file=out)
            attacks, excluded = split_attacks(detect_attacks_cut(mono, sr, cuts), len(mono), cuts)
            if attacks or excluded:
                print("\n".join(format_attacks(name, attacks, excluded)), file=out)
            rows = short_track_cut(mono, sr, cuts, center, a.short_win, a.short_hop, a.short_floor_dbfs, a.span_cents)
            print("\n".join(format_short(name, rows)), file=out)
            out.flush()
    finally:
        if a.tsv: out.close()
    return 0

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav", nargs="?")
    ap.add_argument("--dir")
    ap.add_argument("--nominal", type=float)
    ap.add_argument("--max-spread", type=float, default=1.0)
    ap.add_argument("--verbose", action="store_true", help="imprime la altura de cada tramo")
    ap.add_argument("--from", dest="t0", type=float, default=2.0, help="segundo del primer tramo")
    ap.add_argument("--partials", type=int, default=1,
                    help="REQ-035: medir tambien los parciales 2..N alrededor de n·hz_1")
    ap.add_argument("--self-test", action="store_true",
                    help="verificar el oraculo (parcial, float32, wmad, corto) contra series sinteticas conocidas")
    ap.add_argument("--short", action="store_true", help="REQ-055: altura instantanea por tramos cortos")
    ap.add_argument("--short-win", type=float, default=0.1, help="ventana minima, en s (y >= 10 periodos)")
    ap.add_argument("--short-hop", type=float, default=0.05, help="salto entre tramos, en s")
    ap.add_argument("--short-floor-dbfs", type=float, default=-60.0, help="bajo este RMS el hz es nan")
    ap.add_argument("--span-cents", type=float, default=150.0, help="busqueda ±N cents del centro")
    ap.add_argument("--tsv", help="con --short: escribe el TSV (con cabecera) a este archivo")
    ap.add_argument("--allow-invalid", action="store_true",
                    help="leer tambien tomas con huecos / .invalid.wav (nunca para un oraculo)")
    a = ap.parse_args()
    global ALLOW_INVALID
    ALLOW_INVALID = a.allow_invalid
    if a.self_test:
        sys.exit(self_test())
    files = sorted(os.path.join(a.dir, f) for f in os.listdir(a.dir) if f.endswith(".wav")) \
            if a.dir else [a.wav]
    if not files or files == [None]:
        ap.error("falta el .wav o --dir")
    if a.short:
        sys.exit(run_short(a, files))
    for path in files:
        name = os.path.basename(path)
        nominal = a.nominal or nominal_for(name)
        if a.partials > 1:
            starts = tuple(a.t0 + 0.75 * k for k in range(4))
            print(format_partials(name, nominal, measure_partials(path, nominal, a.partials, starts=starts),
                                  a.max_spread))
            sys.stdout.flush()
            continue
        hz, spread, n, raw = measure(path, nominal, starts=tuple(a.t0 + 0.75 * k for k in range(4)))
        if a.verbose and raw:
            print(f"  tramos: " + "  ".join(f"{cents(r, nominal):+.3f}c" for r in raw))
        if hz is None:
            print(f"{name}\t-\t-\t0\tsin-tramos"); continue
        verdict = "ok" if spread <= a.max_spread else "inestable"
        print(f"{name}\t{hz:.5f}\t{spread:.3f}\t{n}\t{verdict}\tvs-nominal={cents(hz, nominal):+.3f}c")
        sys.stdout.flush()

if __name__ == "__main__":
    main()
