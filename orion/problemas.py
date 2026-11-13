"""Problemas con respuesta comprobable, generados sin límite y con dificultad graduada (niveles 1 a 5).

Cada generador recibe un `random.Random` y un nivel y devuelve un `Problema`. La respuesta correcta la calcula SymPy,
no el modelo: eso es lo que permite al juez decidir sin opiniones.

Tipos de respuesta:
- "expr":      una expresión; es correcta si equivale a la verdad.
- "num":       un número; correcta si coincide con tolerancia relativa del 0,5 %.
- "lista":     varias expresiones separadas por comas, en el orden pedido.
- "conjunto":  varias expresiones separadas por comas, en cualquier orden.
- "primitiva": una expresión cuya derivada equivale a la verdad (integrales: vale cualquier constante).
"""
from __future__ import annotations

import hashlib
import random
from dataclasses import dataclass, asdict

import sympy as sp

x = sp.Symbol("x")
NIVEL_MAX = 5


@dataclass
class Problema:
    dominio: str
    nivel: int
    enunciado: str
    tipo: str
    verdad: str          # en sintaxis de SymPy; para "lista" y "conjunto", separada por comas

    @property
    def huella(self) -> str:
        return hashlib.sha1(self.enunciado.encode()).hexdigest()[:16]

    def a_dict(self) -> dict:
        return asdict(self)


def _s(e) -> str:
    return sp.sstr(e)


def _entero(r: random.Random, lim: int, cero: bool = False) -> int:
    while True:
        n = r.randint(-lim, lim)
        if n != 0 or cero:
            return n


def _polinomio(r: random.Random, grado: int, lim: int):
    p = sum(_entero(r, lim, cero=(i != grado)) * x**i for i in range(grado + 1))
    return sp.expand(p)


# ---------------------------------------------------------------- matemáticas
def aritmetica(r: random.Random, nivel: int) -> Problema:
    lim = [0, 12, 60, 300, 999, 9999][nivel]
    a, b, c = (r.randint(2, lim) for _ in range(3))
    formas = [f"{a} + {b}", f"{a} - {b}", f"{a} * {b % 12 + 2}"]
    if nivel >= 2:
        formas += [f"{a} * {b}", f"{a} + {b} * {c % 20 + 2}"]
    if nivel >= 3:
        formas += [f"({a} - {b}) * {c % 30 + 2}", f"{a} * {b} - {c}"]
    if nivel >= 4:
        formas += [f"({a} + {b}) * ({c} - {a % 50})", f"{a} * {b} + {c} * {a % 90 + 2}"]
    e = r.choice(formas)
    return Problema("aritmetica", nivel, f"Calcula: {e}", "num", _s(sp.sympify(e)))


def lineales(r: random.Random, nivel: int) -> Problema:
    lim = [0, 5, 9, 12, 20, 40][nivel]
    sol = sp.Rational(_entero(r, lim, True), 1 if nivel < 3 else r.choice([1, 1, 2, 3, 4, 5]))
    a, c = _entero(r, lim), (_entero(r, lim, True) if nivel >= 2 else 0)
    if a == c:
        c = 0
    b = _entero(r, lim * 2, True)
    d = (a - c) * sol + b                      # a*x + b = c*x + d  tiene solución sol
    if nivel >= 4:
        k = _entero(r, 6)
        izq, der = f"{k}*({_s(a * x + b)})", _s(sp.expand(k * (c * x + d)))
    else:
        izq, der = _s(a * x + b), _s(c * x + d)
    return Problema("lineales", nivel, f"Resuelve la ecuación para x: {izq} = {der}", "expr", _s(sol))


def cuadraticas(r: random.Random, nivel: int) -> Problema:
    lim = [0, 4, 7, 10, 14, 20][nivel]
    r1, r2 = _entero(r, lim, True), _entero(r, lim, True)
    while r2 == r1:
        r2 = _entero(r, lim, True)
    if nivel >= 4:
        r1 = sp.Rational(r1, r.choice([2, 3]))
    a = 1 if nivel < 3 else _entero(r, 4)
    den = sp.denom(sp.Rational(r1))
    p = sp.expand(a * den * (x - r1) * (x - r2))
    raices = sorted({sp.Rational(r1), sp.Rational(r2)})
    return Problema("cuadraticas", nivel, f"Halla todas las soluciones reales de: {_s(p)} = 0. Sepáralas con comas.",
                    "conjunto", ", ".join(_s(v) for v in raices))


def expandir(r: random.Random, nivel: int) -> Problema:
    lim = [0, 4, 6, 9, 9, 12][nivel]
    f1 = _polinomio(r, 1, lim)
    f2 = _polinomio(r, 1 if nivel < 3 else 2, lim)
    e = f"({_s(f1)})*({_s(f2)})"
    if nivel >= 4:
        f3 = _polinomio(r, 1, lim)
        e += f" + ({_s(f3)})**2" if nivel == 4 else f"*({_s(f3)})"
    return Problema("expandir", nivel, f"Desarrolla y simplifica: {e}", "expr", _s(sp.expand(sp.sympify(e))))


def _funcion(r: random.Random, nivel: int):
    if nivel == 1:
        return _entero(r, 9) * x ** r.randint(1, 4)
    if nivel == 2:
        return _polinomio(r, r.randint(2, 3), 9)
    base = [sp.sin(x), sp.cos(x), sp.exp(x), x ** r.randint(2, 4)]
    if nivel == 3:
        return _entero(r, 6) * r.choice(base) + _polinomio(r, 2, 6)
    k = _entero(r, 5)
    interior = [sp.sin(k * x), sp.cos(k * x), sp.exp(k * x)]
    if nivel == 4:
        return _entero(r, 6) * r.choice(interior) + _entero(r, 6) * x ** r.randint(1, 3)
    return _polinomio(r, 1, 5) * r.choice(interior)


def derivadas(r: random.Random, nivel: int) -> Problema:
    f = _funcion(r, nivel)
    if nivel == 5 and r.random() < 0.5:
        f = sp.sin(_polinomio(r, 2, 4)) if r.random() < 0.5 else sp.exp(_polinomio(r, 2, 3))
    return Problema("derivadas", nivel, f"Deriva respecto de x: {_s(f)}", "expr", _s(sp.diff(f, x)))


def integrales(r: random.Random, nivel: int) -> Problema:
    f = _funcion(r, nivel)
    return Problema("integrales", nivel, f"Calcula una primitiva (integral indefinida) respecto de x de: {_s(f)}. No añadas la constante.",
                    "primitiva", _s(f))


def sistemas(r: random.Random, nivel: int) -> Problema:
    lim = [0, 3, 5, 8, 12, 20][nivel]
    while True:
        a, b, c, d = (_entero(r, lim) for _ in range(4))
        if a * d - b * c != 0:
            break
    sx, sy = _entero(r, lim, True), _entero(r, lim, True)
    if nivel >= 4:
        sx = sp.Rational(sx, 2)
    e1, e2 = a * sx + b * sy, c * sx + d * sy
    y = sp.Symbol("y")
    return Problema("sistemas", nivel,
                    f"Resuelve el sistema: {_s(a * x + b * y)} = {_s(e1)} ; {_s(c * x + d * y)} = {_s(e2)}. Da x e y, en ese orden, separados por una coma.",
                    "lista", f"{_s(sx)}, {_s(sy)}")


# ---------------------------------------------------------------- física (unidades SI, g = 9.8 m/s^2)
def cinematica(r: random.Random, nivel: int) -> Problema:
    v0, a, t = r.randint(0, 30), r.randint(1, 9), r.randint(2, 12)
    h = r.randint(5, 120)
    casos = [
        (f"Un coche va a {v0 + 5} m/s durante {t} s a velocidad constante. ¿Qué distancia recorre, en metros?", (v0 + 5) * t),
        (f"Un móvil parte del reposo con aceleración constante de {a} m/s^2. ¿Qué velocidad tiene, en m/s, a los {t} s?", a * t),
        (f"Un móvil parte con velocidad {v0} m/s y aceleración constante de {a} m/s^2. ¿Qué distancia recorre, en metros, en {t} s?",
         v0 * t + sp.Rational(a, 2) * t**2),
        (f"Se deja caer un objeto desde {h} m de altura (g = 9.8 m/s^2, sin rozamiento). ¿Cuánto tarda en llegar al suelo, en segundos?",
         sp.sqrt(2 * h / sp.Float(9.8))),
        (f"Se lanza un objeto hacia arriba a {v0 + 5} m/s (g = 9.8 m/s^2, sin rozamiento). ¿Qué altura máxima alcanza, en metros?",
         (v0 + 5) ** 2 / (2 * sp.Float(9.8))),
        (f"Un coche que va a {v0 + 10} m/s frena con deceleración constante de {a} m/s^2. ¿Qué distancia recorre, en metros, hasta pararse?",
         sp.Rational((v0 + 10) ** 2, 2 * a)),
    ]
    enunciado, v = casos[min(len(casos) - 1, r.randint(max(0, nivel - 2), nivel))]
    return Problema("cinematica", nivel, enunciado + " Responde solo con el número.", "num", _s(sp.N(v, 8)))


def energia(r: random.Random, nivel: int) -> Problema:
    m, v, h = r.randint(1, 60), r.randint(2, 30), r.randint(2, 80)
    volt, res, res2 = r.randint(3, 240), r.randint(2, 90), r.randint(2, 90)
    g = sp.Float(9.8)
    casos = [
        (f"Por una resistencia de {res} ohmios pasa una corriente de {v} A. ¿Qué tensión hay entre sus extremos, en voltios?", v * res),
        (f"¿Cuál es la energía cinética, en julios, de un cuerpo de {m} kg que se mueve a {v} m/s?", sp.Rational(m * v * v, 2)),
        (f"¿Cuál es la energía potencial gravitatoria, en julios, de un cuerpo de {m} kg a {h} m de altura (g = 9.8 m/s^2)?", m * g * h),
        (f"Una resistencia de {res} ohmios se conecta a {volt} V. ¿Qué potencia disipa, en vatios?", sp.Rational(volt * volt, res)),
        (f"Un cuerpo de {m} kg cae desde {h} m partiendo del reposo (g = 9.8 m/s^2, sin rozamiento). ¿Con qué velocidad llega al suelo, en m/s?",
         sp.sqrt(2 * g * h)),
        (f"Dos resistencias de {res} y {res2} ohmios en paralelo se conectan a {volt} V. ¿Qué corriente total circula, en amperios?",
         sp.Rational(volt, res) + sp.Rational(volt, res2)),
    ]
    enunciado, val = casos[min(len(casos) - 1, r.randint(max(0, nivel - 2), nivel))]
    return Problema("energia", nivel, enunciado + " Responde solo con el número.", "num", _s(sp.N(val, 8)))


def respuesta_de_referencia(p: Problema) -> str:
    """La respuesta correcta tal como debería escribirla el modelo."""
    if p.tipo == "primitiva":
        return _s(sp.integrate(sp.sympify(p.verdad), x))
    return p.verdad


GENERADORES = {
    "aritmetica": aritmetica, "lineales": lineales, "cuadraticas": cuadraticas, "expandir": expandir, "derivadas": derivadas,
    "integrales": integrales, "sistemas": sistemas, "cinematica": cinematica, "energia": energia,
}
DOMINIOS = list(GENERADORES)


def generar(dominio: str, nivel: int, r: random.Random) -> Problema:
    return GENERADORES[dominio](r, max(1, min(NIVEL_MAX, nivel)))


def conjunto_fijo(n: int, semilla: int, dominios: list[str] | None = None, excluir: set[str] | None = None) -> list[Problema]:
    """n problemas repartidos por igual entre todos los dominios y niveles, siempre los mismos para la misma semilla."""
    r = random.Random(semilla)
    doms = dominios or DOMINIOS
    vistos, salida, i, intentos = set(excluir or ()), [], 0, 0
    while len(salida) < n and intentos < n * 50:
        intentos += 1
        p = generar(doms[i % len(doms)], (i // len(doms)) % NIVEL_MAX + 1, r)
        if p.huella in vistos:
            continue
        vistos.add(p.huella)
        salida.append(p)
        i += 1
    return salida
