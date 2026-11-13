"""Pruebas de Orion. No necesitan GPU ni internet: el modelo real se sustituye por uno diminuto creado aquí mismo."""
import hashlib
import json
import random

import pytest
import sympy as sp

from orion import curriculo as cur
from orion import juez, problemas
from orion.config import Config, Estado, escribir_json
from orion.problemas import DOMINIOS, Problema, conjunto_fijo, generar, respuesta_de_referencia


# ------------------------------------------------------------------ problemas y juez
def test_el_juez_acepta_la_verdad_de_todos_los_dominios():
    assert juez.autoprueba(450) == 0


def test_el_juez_rechaza_respuestas_malas_y_peligrosas():
    r = random.Random(3)
    for d in DOMINIOS:
        p = generar(d, 3, r)
        for mala in ["", "no lo sé", "RESPUESTA:", "RESPUESTA: 123456789", "RESPUESTA: __import__('os').system('echo hola')",
                     "RESPUESTA: open('x','w')", "RESPUESTA: x.__class__", "RESPUESTA: " + "9" * 400]:
            assert not juez.juzgar(p, mala), (d, mala)


def test_el_juez_entiende_formas_equivalentes():
    p = Problema("derivadas", 2, "Deriva respecto de x: x**3 + 2*x", "expr", "3*x**2 + 2")
    for buena in ["RESPUESTA: 3x^2 + 2", "razono...\nRESPUESTA: 2 + 3*x**2", "RESPUESTA: f'(x) = 3·x² + 2.", "RESPUESTA: $3x^2+2$"]:
        assert juez.juzgar(p, buena), buena
    assert not juez.juzgar(p, "RESPUESTA: 3*x**2")
    q = Problema("cuadraticas", 1, "…", "conjunto", "-2, 3")
    assert juez.juzgar(q, "RESPUESTA: x = 3, x = -2") and juez.juzgar(q, "RESPUESTA: 3 y -2")
    assert not juez.juzgar(q, "RESPUESTA: 3") and not juez.juzgar(q, "RESPUESTA: 3, 3")
    s = Problema("sistemas", 1, "…", "lista", "1, 2")
    assert juez.juzgar(s, "RESPUESTA: 1, 2") and not juez.juzgar(s, "RESPUESTA: 2, 1")
    n = Problema("cinematica", 1, "…", "num", "44.100000")
    assert juez.juzgar(n, "RESPUESTA: 44.1 m") and juez.juzgar(n, "RESPUESTA: 44,1") and not juez.juzgar(n, "RESPUESTA: 45")
    i = Problema("integrales", 1, "…", "primitiva", "2*x")
    assert juez.juzgar(i, "RESPUESTA: x^2 + 7") and not juez.juzgar(i, "RESPUESTA: 2*x")
    assert juez.juzgar(i, "RESPUESTA: " + respuesta_de_referencia(i))


def test_solo_cuenta_la_ultima_respuesta():
    p = Problema("aritmetica", 1, "Calcula: 2 + 2", "num", "4")
    assert juez.juzgar(p, "RESPUESTA: 5\nme corrijo\nRESPUESTA: 4")
    assert not juez.juzgar(p, "RESPUESTA: 4\nRESPUESTA: 5")


def test_validacion_y_examen_son_fijos_y_disjuntos():
    a, b = conjunto_fijo(90, 7), conjunto_fijo(90, 7)
    assert [p.enunciado for p in a] == [p.enunciado for p in b]
    e = conjunto_fijo(90, 8, excluir={p.huella for p in a})
    assert not {p.huella for p in a} & {p.huella for p in e}
    assert {p.dominio for p in a} == set(DOMINIOS) and {p.nivel for p in a} == {1, 2, 3, 4, 5}


# ------------------------------------------------------------------ currículo y aceptación
def test_el_curriculo_sube_de_nivel_y_evita_lo_reservado():
    c, r = {}, random.Random(0)
    reservados = {p.huella for p in conjunto_fijo(90, 7)}
    ps = cur.elegir(c, 60, r, reservados)
    assert len(ps) == 60 and all(p.nivel == 1 for p in ps) and not {p.huella for p in ps} & reservados
    avisos = cur.anotar(c, [(p, 1.0 if p.dominio == "aritmetica" else 0.0) for p in ps * 3])
    assert c["aritmetica"]["nivel"] == 2 and c["lineales"]["nivel"] == 1 and len(avisos) == 1
    ps = cur.elegir(c, 300, r, reservados)
    assert any(p.nivel == 2 for p in ps)
    assert sum(p.dominio == "lineales" for p in ps) > sum(p.dominio == "aritmetica" and p.nivel == 1 for p in ps)   # más donde falla


def test_solo_se_acepta_una_mejora_clara():
    base = [1] * 50 + [0] * 50
    assert not cur.mejora(base, base)
    assert not cur.mejora(base, [1] * 49 + [0] + [1] + [0] * 49)            # gana 1, pierde 1
    assert cur.mejora(base, [1] * 50 + [1] * 5 + [0] * 45)                  # gana 5
    assert not cur.mejora(base, [0] * 10 + [1] * 40 + [1] * 12 + [0] * 38)  # gana 12, pierde 10: ruido
    assert cur.comparar(base, [1] * 50 + [1] * 5 + [0] * 45) == (5, 0, 3)


# ------------------------------------------------------------------ el bucle, con un modelo simulado
class ModeloSimulado:
    """Acierta una fracción `nivel` de los problemas de aritmética; entrenar la cambia en `paso`."""
    def __init__(self, nivel=0.2, paso=0.25):
        self.nivel, self.paso, self.rango, self.modulos, self.capas, self.entrenamientos = nivel, paso, 16, ["q_proj"], 4, 0
        self.invento = "no se me ocurre nada"

    def _sabe(self, enunciado):
        return int(hashlib.sha1(enunciado.encode()).hexdigest(), 16) % 1000 < self.nivel * 1000

    def generar(self, enunciados, n=1, temperatura=0.0, max_tokens=0, lote=0, parar=None, sistema=""):
        if "Inventas" in sistema:
            return [[self.invento]] * len(enunciados)
        return [[f"RESPUESTA: {sp.sympify(e.split(': ')[1]) if self._sabe(e) else 'x'}"] * (n if temperatura > 0 else 1) for e in enunciados]

    def entrenar(self, ejemplos, *a, **k):
        assert ejemplos and all(isinstance(e, tuple) for e in ejemplos)
        self.entrenamientos += 1
        self.nivel += self.paso
        return 0.5

    def copia_adaptador(self): return {"nivel": self.nivel}
    def restaurar_adaptador(self, c): self.nivel = c["nivel"]
    def guardar_adaptador(self, carpeta): carpeta.mkdir(parents=True, exist_ok=True); (carpeta / "adapter_config.json").write_text("{}")
    def num_capas(self): return self.capas
    def parametros(self): return 1000 * self.rango, 10**6 * self.capas
    def ampliar_adaptador(self, rango, modulos): self.rango, self.modulos = rango, list(modulos)
    def anadir_capa(self, carpeta): carpeta.mkdir(parents=True); self.capas += 1; return True


@pytest.fixture
def orion_simulado(tmp_path, monkeypatch):
    from orion import bucle
    monkeypatch.setattr(problemas, "DOMINIOS", ["aritmetica"])
    monkeypatch.setattr(cur, "DOMINIOS", ["aritmetica"])
    escribir_json(tmp_path / "config.json", {"modelo": "simulado", "n_validacion": 60, "n_examen": 60, "problemas_por_generacion": 20,
                                             "paciencia": 1, "rango_max": 32, "max_capas_nuevas": 1, "inventos_por_generacion": 2})

    def crear(nivel=0.2, paso=0.25, mayor=None):
        sim = ModeloSimulado(nivel, paso)
        monkeypatch.setattr(bucle.Orion, "_crear_modelo", lambda self, base, *a: mayor if base == "grande" else sim)
        monkeypatch.setattr(bucle, "siguiente_modelo", lambda actual: "grande" if mayor and actual == "simulado" else None)
        return bucle.Orion(tmp_path, decir=lambda *_: None), sim
    return crear, tmp_path


def test_una_mejora_real_se_acepta_y_queda_guardada(orion_simulado):
    crear, datos = orion_simulado
    o, sim = crear(nivel=0.2, paso=0.25)
    o.vivir(generaciones=2)
    e = Estado.cargar(datos)
    assert e.generacion == 2 and sim.nivel == pytest.approx(0.7)
    assert sum(e.validacion) > 60 * 0.5 and sum(e.examen) / 60 > e.examen_inicial + 0.25      # el examen, que no decide, también sube
    assert (datos / e.adaptador / "adapter_config.json").exists() and len(list((datos / "adaptadores").iterdir())) == 1
    g = [json.loads(l) for l in (datos / "generaciones.jsonl").read_text().splitlines()]
    assert [x["tipo"] for x in g] == ["inicio", "generacion", "generacion"] and all(x["aceptada"] for x in g[1:])
    ejemplos = [json.loads(l) for l in (datos / "ejemplos.jsonl").read_text().splitlines()]
    assert {x["origen"] for x in ejemplos} == {"propia", "referencia"}
    assert not {Problema(**{k: x[k] for k in ("dominio", "nivel", "enunciado", "tipo", "verdad")}).huella for x in ejemplos} & o.reservados
    for x in ejemplos:                                             # todo lo que entra al entrenamiento es correcto según el juez
        assert juez.juzgar(Problema(x["dominio"], x["nivel"], x["enunciado"], x["tipo"], x["verdad"]), x["respuesta"])
    assert "generación 2" in o.resumen() and "✓ aceptada" in o.resumen()


def test_un_entrenamiento_que_empeora_se_deshace_y_al_estancarse_crece(orion_simulado):
    crear, datos = orion_simulado
    o, sim = crear(nivel=0.5, paso=-0.3)
    o.vivir(generaciones=6)
    e = Estado.cargar(datos)
    assert sim.nivel == pytest.approx(0.5) and sim.entrenamientos == 6      # siempre vuelve a los pesos anteriores
    g = [json.loads(l) for l in (datos / "generaciones.jsonl").read_text().splitlines()]
    assert not any(x.get("aceptada") for x in g)
    acciones = [x.get("accion", x["tipo"]) for x in g if x["tipo"] in ("ampliacion", "limite")]
    assert len(acciones) == 6 and "doble" in acciones[0] and "rango 16 a 32" in acciones[1] and "MLP" in acciones[2]
    assert "capa nueva (4 → 5" in acciones[3] and "cuatro veces" in acciones[4] and acciones[5] == "limite"
    assert e.rango == 32 and e.capas_nuevas == 1 and sim.capas == 5 and e.modelo_base.startswith("bases/") and (datos / e.modelo_base).exists()
    assert e.lr <= Config().lr


def test_continua_donde_lo_dejo(orion_simulado):
    crear, datos = orion_simulado
    o, _ = crear(nivel=0.2, paso=0.25)
    o.vivir(generaciones=1)
    antes = Estado.cargar(datos)
    o2, sim2 = crear(nivel=0.45, paso=0.25)          # "reinicio": mismos datos, modelo recargado
    assert o2.estado.generacion == 1 and o2.estado.validacion == antes.validacion
    o2.vivir(generaciones=1)
    assert Estado.cargar(datos).generacion == 2 and len([l for l in (datos / "generaciones.jsonl").read_text().splitlines() if '"inicio"' in l]) == 1


def test_un_corte_a_mitad_no_deja_nada_a_medias(orion_simulado):
    crear, datos = orion_simulado
    o, sim = crear(nivel=0.2, paso=0.25)
    assert o.inicio()
    antes = Estado.cargar(datos)
    entrenar = sim.entrenar
    sim.entrenar = lambda *a, **k: (o.parar(), entrenar(*a, **k))[1]      # llega Ctrl-C durante el entrenamiento
    o.generacion()
    despues = Estado.cargar(datos)
    assert despues.generacion == 0 and despues.validacion == antes.validacion and sim.nivel == pytest.approx(0.2)


def test_al_agotar_el_modelo_se_traslada_a_uno_mayor_solo_si_no_empeora(orion_simulado):
    crear, datos = orion_simulado
    o, sim = crear(nivel=0.5, paso=-0.3, mayor=ModeloSimulado(nivel=0.1, paso=0.0))      # el mayor es peor: se descarta
    o.vivir(generaciones=6)
    e = Estado.cargar(datos)
    assert e.modelo_original == "simulado" and e.migracion_descartada == "grande" and e.migraciones == 0 and o.modelo is sim
    o.vivir(generaciones=1)                                                              # ya no lo vuelve a intentar: límite
    assert json.loads((datos / "generaciones.jsonl").read_text().splitlines()[-1])["tipo"] == "limite"


def test_traslado_a_un_modelo_mayor_que_si_mejora(orion_simulado):
    crear, datos = orion_simulado
    grande = ModeloSimulado(nivel=0.6, paso=0.2)
    o, sim = crear(nivel=0.5, paso=-0.3, mayor=grande)
    o.vivir(generaciones=6)
    e = Estado.cargar(datos)
    assert o.modelo is grande and e.modelo_base == e.modelo_original == "grande" and e.migraciones == 1
    assert e.capas_nuevas == 0 and e.rango == 16 and e.multiplicador_exploracion == 1
    assert not any((datos / "bases").iterdir())                                          # la base ampliada del modelo anterior se borra
    assert sum(e.validacion) > 60 * 0.7 and grande.entrenamientos == 1                   # se llevó lo aprendido: entrenó con sus ejemplos
    assert "traslado a grande ✓ hecho" in o.resumen()


# ------------------------------------------------------------------ programación: ejecución aislada, juez e inventos
from orion import programas  # noqa: E402


def test_el_ejecutor_corre_codigo_normal_y_bloquea_el_peligroso():
    assert programas.ejecutar("def f(a, b):\n    return a + b", "f", [[1, 2], [3, 4]]) == [3, 7]
    assert programas.ejecutar("import math\ndef f(n):\n    return (math.gcd(n, 12), {n}, (n, 0.1 + 0.2))", "f", [[8]]) == [[4, [8], [8, 0.3]]]
    for malo in ["import os\ndef f():\n    return os.getcwd()", "def f():\n    return open('/etc/passwd').read()",
                 "def f():\n    return ().__class__.__bases__", "def f():\n    return __import__('os')", "import subprocess\ndef f():\n    return 1",
                 "def f():\n    return eval('1')", "from os import path\ndef f():\n    return 1", "def f(:\n    pass"]:
        assert not programas.codigo_seguro(malo) and programas.ejecutar(malo, "f", [[]]) is None, malo
    assert programas.ejecutar("def f():\n    while True:\n        pass", "f", [[]], segundos=2) is None          # bucle infinito
    assert programas.ejecutar("def f(n):\n    return 1 // n", "f", [[0]]) is None                               # excepción
    assert programas.ejecutar("def g():\n    return 1", "f", [[]]) is None                                       # falta la función


def test_el_juez_de_codigo_acepta_todas_las_referencias_y_rechaza_lo_incorrecto():
    r = random.Random(5)
    vistos = set()
    for i in range(150):
        p = problemas.GENERADORES["codigo"](r, i % 5 + 1)
        d = json.loads(p.verdad)
        if d["funcion"] in vistos:
            continue
        vistos.add(d["funcion"])
        assert juez.juzgar(p, "Aquí está:\n```python\n" + p.referencia + "\n```\nEspero que sirva."), d["funcion"]
        assert not juez.juzgar(p, f"```python\ndef {d['funcion']}(*a):\n    return None\n```"), d["funcion"]
        assert not juez.juzgar(p, "No sé hacerlo. RESPUESTA: 3")
    assert vistos == {f[1] for f in programas.FAMILIAS}
    p = problemas.GENERADORES["codigo"](r, 2)
    assert juez.juzgar(p, "```python\ndef otra():\n    pass\n```\ny la buena:\n```python\n" + p.referencia)   # bloque sin cerrar, varios bloques


INVENTO = """ENUNCIADO: Devuelve la suma de los cuadrados de los números pares de la lista.
```python
def suma_cuadrados_pares(lista):
    return sum(v * v for v in lista if v % 2 == 0)
```
ENTRADAS: [[[1, 2, 3, 4]], [[]], [[6]], [[5, 7]], [[2, 2]]]
Espero que te guste."""


def test_un_ejercicio_inventado_solo_vale_si_su_solucion_funciona():
    p = programas.invento_valido(INVENTO)
    assert p and p.dominio == "inventados" and "suma_cuadrados_pares(lista)" in p.enunciado
    assert json.loads(p.verdad)["salidas"] == [20, 0, 36, 0, 8]
    assert juez.juzgar(p, "```python\ndef suma_cuadrados_pares(lista):\n    t = 0\n    for v in lista:\n        if v % 2 == 0:\n            t += v ** 2\n    return t\n```")
    assert not juez.juzgar(p, "```python\ndef suma_cuadrados_pares(lista):\n    return sum(lista)\n```")
    for malo in [INVENTO.replace("v % 2 == 0", "v % 0 == 0"),                       # la solución falla
                 INVENTO.replace("return sum(v * v for v in lista if v % 2 == 0)", "return 7"),   # trivial: siempre lo mismo
                 INVENTO.replace("return sum", "import os\n    return sum"),         # código no permitido
                 INVENTO.replace("ENTRADAS:", "PRUEBAS:"), INVENTO.replace("[[6]], [[5, 7]], [[2, 2]]]", ""), "hola"]:
        assert programas.invento_valido(malo) is None, malo


def test_los_inventos_entran_en_su_curriculo_y_los_practica(orion_simulado):
    crear, datos = orion_simulado
    o, sim = crear(nivel=0.2, paso=0.25)
    sim.invento = INVENTO
    generar = sim.generar
    sim.generar = lambda e, *a, **k: [["no sé"] * (k.get("n", 1) if k.get("temperatura", 0) > 0 else 1) if "función de Python" in x else y
                                      for x, y in zip(e, generar([x if "función" not in x else "Calcula: 1 + 1" for x in e], *a, **k))]
    o.vivir(generaciones=2)
    assert len(o.inventados) == 1                                                     # el repetido no se guarda dos veces
    guardados = [json.loads(l) for l in (datos / "inventados.jsonl").read_text().splitlines()]
    assert len(guardados) == 1 and guardados[0]["referencia"].startswith("def suma_cuadrados_pares")
    ejemplos = [json.loads(l) for l in (datos / "ejemplos.jsonl").read_text().splitlines()]
    propio = [x for x in ejemplos if x["dominio"] == "inventados"]
    assert propio and propio[0]["origen"] == "referencia" and "```python" in propio[0]["respuesta"]   # no lo resolvió: estudia su propia solución
    from orion import bucle
    assert len(bucle.Orion(datos, decir=lambda *_: None).inventados) == 1 and "inventados por mí" in o.resumen()


def test_texto_y_razonamiento():
    t = Problema("logica", 1, "…", "texto", "miércoles")
    assert juez.juzgar(t, "RESPUESTA: Miercoles.") and juez.juzgar(t, "RESPUESTA: «miércoles»") and not juez.juzgar(t, "RESPUESTA: jueves")
    o = Problema("lenguaje", 3, "…", "texto", "agua, luna, sol")
    assert juez.juzgar(o, "RESPUESTA: agua,luna ,  sol") and not juez.juzgar(o, "RESPUESTA: luna, agua, sol")
    r = random.Random(9)
    for d in ("logica", "lenguaje"):
        for i in range(100):
            p = generar(d, i % 5 + 1, r)
            assert p.tipo in ("num", "texto") and p.verdad and juez.juzgar(p, problemas.texto_de_referencia(p))
