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

    def _sabe(self, enunciado):
        return int(hashlib.sha1(enunciado.encode()).hexdigest(), 16) % 1000 < self.nivel * 1000

    def generar(self, enunciados, n=1, temperatura=0.0, max_tokens=0, lote=0, parar=None):
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
                                             "paciencia": 1, "rango_max": 32, "max_capas_nuevas": 1})

    def crear(nivel=0.2, paso=0.25):
        sim = ModeloSimulado(nivel, paso)
        monkeypatch.setattr(bucle.Orion, "_cargar_modelo", lambda self: sim)
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
