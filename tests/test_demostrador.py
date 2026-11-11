# Pruebas del demostrador (Python). Usan un Lean SIMULADO (tests/mock_lean.py) y un LLM falso;
# la parte de SymPy usa SymPy real. Ejecutar:  python demostrador.py selftest   (o python -m unittest)
import json, os, re, sys, tempfile, threading, unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest import mock

RAIZ = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(RAIZ))
import demostrador as d  # noqa: E402

MOCK_LEAN = str(RAIZ / "tests" / "mock_lean.py")
FIRMA = "theorem par_mayor (n : Nat) : ∃ m : Nat, m > n ∧ m % 2 = 0 := by"
OK_PAR = "```lean\n" + FIRMA + "\n  exact ⟨2 * n + 2, by omega⟩\n```"
MAL_PAR = "```lean\n" + FIRMA + "\n  omega\n```"


class FakeLLM:
    """LLM falso: `manejador(usuario, kw, n_llamada)` devuelve texto, dict, o una excepción a lanzar."""
    modelo = "falso"

    def __init__(self, manejador):
        self.m, self.llamadas = manejador, []

    def chat(self, sistema, usuario, **kw):
        self.llamadas.append({"u": usuario, **kw})
        r = self.m(usuario, kw, len(self.llamadas))
        if isinstance(r, BaseException):
            raise r
        return r if isinstance(r, str) else json.dumps(r)


def formalizar(firma=FIRMA, defs=""):
    return {"definiciones": defs, "teorema": firma}


class Base(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.dir = Path(self.td.name) / "run"
        self.addCleanup(self.td.cleanup)
        self.logs = []

    def oraculo(self, reglas):
        p = Path(self.td.name) / "oraculo.json"
        p.write_text(json.dumps({"teoremas": reglas}), encoding="utf-8")
        patcher = mock.patch.dict(os.environ, {"MOCK_LEAN_ORACLE": str(p)})
        patcher.start()
        self.addCleanup(patcher.stop)

    def cfg(self, **kw):
        base = dict(lean=MOCK_LEAN, directos=3, finales=2, replanes=2, planes=2, fidelidad=False, timeout_lean=30)
        base.update(kw)
        return d.Config(**base)

    def motor(self, llm, cfg=None, dirx=None):
        cfg = cfg or self.cfg()
        return d.Motor(cfg, llm, d.LeanVerificador(cfg), dirx or self.dir, log=self.logs.append)

    def problema(self, enun="Demuestra que para todo n existe un par mayor que n.", backend="lean"):
        return {"id": "p", "enunciado": enun, "backend": backend, "dominio": "matematicas"}

    def estado(self, dirx=None):
        return json.loads(((dirx or self.dir) / "estado.json").read_text(encoding="utf-8"))


class TestUtilidades(unittest.TestCase):
    def test_json(self):
        self.assertEqual(d.extraer_json('```json\n{"a": 1}\n```'), {"a": 1})
        self.assertEqual(d.extraer_json('Claro: {"a": "x {y} z"} listo'), {"a": "x {y} z"})
        with self.assertRaises(ValueError):
            d.extraer_json("sin json")

    def test_extraer_cuerpo_y_firma(self):
        v = d.LeanVerificador(d.Config())
        casos = {
            "```lean\ntheorem t (n : Nat) : n = n := by\n  rfl\n```": "  rfl",
            "```lean\ntheorem t : 1 = 1 := by rfl\n```": "  rfl",
            "Explicación.\n```lean\ntheorem t : 1 = 1 := by\n  simp\n```\nOtra nota\n```text\nno\n```": "  simp",
            "```lean\ntheorem t (h : p ∧ q) : q := by\n  obtain ⟨a, b⟩ := h\n  exact b\n\ntheorem otro : True := by trivial\n```": "  obtain ⟨a, b⟩ := h\n  exact b",
            "omega": "  omega",
            "```lean\ntheorem t : ∃ x : Nat, x > 0 := by\n  refine ⟨1, ?_⟩\n  · omega\n```": "  refine ⟨1, ?_⟩\n  · omega",
        }
        for entrada, esperado in casos.items():
            self.assertEqual(v.extraer_cuerpo(entrada), esperado, entrada)
        self.assertEqual(v.extraer_firma("```lean\nlemma  foo (a : Nat)\n   : a = a := by simp\n```"), "theorem foo (a : Nat) : a = a := by")
        self.assertEqual(v.extraer_firma("sin teorema"), "")
        self.assertEqual(v.nombre("theorem abc_1 (x : Nat) : x = x := by"), "abc_1")
        self.assertEqual(v.renombrar("theorem abc (x : Nat) : x = x := by", "l3_abc"), "theorem l3_abc (x : Nat) : x = x := by")

    def test_prohibido(self):
        v = d.LeanVerificador(d.Config())
        for malo in ("  sorry", "  admit", "axiom x : False", "  native_decide", "set_option maxRecDepth 1 in\n  rfl",
                     "  unsafe def f := 1", "macro \"x\" : tactic => `(tactic| rfl)", "import Mathlib", "#eval 1"):
            self.assertIsNotNone(v.prohibido(malo), malo)
        self.assertIsNone(v.prohibido("  omega -- sorry en un comentario\n  /- admit -/ simp"))
        self.assertIsNone(v.prohibido("  exact ⟨2 * n + 2, by omega⟩"))
        p = d.SympyVerificador(d.Config())
        for malo in ("verificar(1, True)", "import subprocess", "open('x')", "eval('1')", "__import__('os')"):
            self.assertIsNotNone(p.prohibido(malo), malo)
        self.assertIsNone(p.prohibido("x = symbols('x')  # open es solo un comentario\nsolve(x**2 - 1, x)"))

    def test_resumir_error_y_huella(self):
        v = d.LeanVerificador(d.Config())
        s = "/tmp/x/Problema.lean:3:2: error: unsolved goals\n⊢ n = n\n/tmp/x/Problema.lean:5:0: warning: unused variable"
        r = v.resumir_error(s)
        self.assertIn("línea 3:2: error: unsolved goals", r)
        self.assertNotIn("warning", r)
        self.assertEqual(d.huella("a  b"), d.huella("a b"))
        self.assertEqual(d.huella("ℕ → ℕ"), d.huella("Nat -> Nat"))

    def test_pistas_y_cabecera(self):
        v = d.LeanVerificador(d.Config())
        self.assertIn("set_option autoImplicit false", v.encabezado)
        err = "/t/Problema.lean:3:46: error: unknown identifier 'even'"
        r = v.resumir_error(err)
        self.assertIn("PISTA", r)
        self.assertIn("m % 2 = 0", r)
        self.assertEqual(v.titulo_error(r), "línea 3:46: error: unknown identifier 'even'")   # el título no incluye la pista
        self.assertIn("PISTA", v.resumir_error("/t/Problema.lean:3:56: error: Function expected at\n  even"))
        self.assertIn("sintaxis", v.resumir_error("/t/Problema.lean:4:2: error: unexpected token ')'"))
        self.assertNotIn("PISTA", v.resumir_error("/t/Problema.lean:3:2: error: unsolved goals\n⊢ n = n"))
        self.assertIn("exact?", d.LeanVerificador(d.Config(mathlib=True)).resumir_error(err))      # con Mathlib la pista es otra
        self.assertIn("m % 2 = 0", v.prompt_formalizar("x", ""))

    def test_mensajes_reales_de_lean_4_35(self):
        """Salida real pegada desde Colab: 'Unknown constant' con mayúscula y error(lean.unknownIdentifier)."""
        v = d.LeanVerificador(d.Config())
        real = ("/tmp/x/Problema.lean:5:27: error(lean.unknownIdentifier): Unknown constant `Nat.exists_even`\n"
                "/tmp/x/Problema.lean:5:9: error: Tactic `rcases` failed")
        r = v.resumir_error(real)
        self.assertIn("línea 5:27: error(lean.unknownIdentifier): Unknown constant `Nat.exists_even`", r)
        self.assertIn("PISTA", r)
        self.assertIn("No inventes", r)
        self.assertIn("Unknown constant `Nat.exists_even`", v.titulo_error(r))

    def test_estrategia_y_testigos_para_existenciales(self):
        v = d.LeanVerificador(d.Config())
        self.assertIn("EXISTENCIAL", v.estrategia(FIRMA))
        self.assertIn("omega", v.estrategia(FIRMA))
        self.assertEqual(v.estrategia("theorem t (n : Nat) : n + 0 = n := by"), "")
        alts = v.testigos_auto(FIRMA)
        self.assertIn("exact ⟨2 * n + 2, by omega⟩", alts)
        self.assertEqual(len(alts), len(set(alts)))
        self.assertEqual(v.testigos_auto("theorem t (n : Nat) : n = n := by"), [])
        self.assertEqual(v.testigos_auto("theorem t : ∃ m : Nat, m > 3 := by")[0], "exact ⟨0, by omega⟩")   # sin variables: constantes

    def test_trivial(self):
        v = d.LeanVerificador(d.Config())
        self.assertIsNotNone(v.enunciado_trivial("theorem t : True := by"))
        self.assertIsNotNone(v.enunciado_trivial("theorem t (h : False) : 1 = 2 := by"))
        self.assertIsNone(v.enunciado_trivial(FIRMA))

    def test_escritura_atomica(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "sub" / "a.json"
            d.escribir_atomico(p, "uno")
            d.escribir_atomico(p, "dos")
            self.assertEqual(p.read_text(), "dos")
            self.assertEqual([x.name for x in p.parent.iterdir()], ["a.json"])


class TestLeanSimulado(Base):
    def test_verificador_con_mock(self):
        self.oraculo([{"enunciado_contiene": "par_mayor", "acepta_contiene": ["2 * n + 2"], "axiomas": ["propext"]}])
        v = d.LeanVerificador(self.cfg())
        self.assertIn("Lean", v.entorno())
        ok, s = v.comprobar(f"import Lean\n\n{FIRMA}\n  exact ⟨2 * n + 2, by omega⟩", nombre_final="par_mayor")
        self.assertTrue(ok, s)
        ok, s = v.comprobar(f"import Lean\n\n{FIRMA}\n  omega")
        self.assertFalse(ok)
        self.assertIn("unsolved goals", s)
        ok, s = v.comprobar(f"import Lean\n\n{FIRMA}\n  sorry")
        self.assertFalse(ok)
        self.assertIn("sorry", s)
        ok, _ = v.comprobar(f"import Lean\n\n{FIRMA}\n  sorry", permitir_sorry=True)
        self.assertTrue(ok)
        ok, s = v.comprobar(f"import Lean\n\n{FIRMA}\n  exact ⟨2 * n + 2, by omega", nombre_final="par_mayor")
        self.assertFalse(ok)


class TestMotorLean(Base):
    def setUp(self):
        super().setUp()
        self.oraculo([{"enunciado_contiene": "par_mayor", "acepta_contiene": ["2 * n + 2"]}])

    def router(self, pruebas):
        it = iter(pruebas)

        def f(u, kw, n):
            if "Traduce el siguiente problema" in u:
                return formalizar()
            if "Demuestra este teorema en Lean 4" in u:
                return next(it)
            return {"error": "inesperado"}
        return f

    def test_resuelve_y_repara_con_el_error_anterior(self):
        llm = FakeLLM(self.router([MAL_PAR, OK_PAR]))
        m = self.motor(llm)
        self.assertEqual(m.ejecutar(self.problema()), 0)
        e = self.estado()
        self.assertEqual(e["estado"], "verificado")
        self.assertEqual(len(llm.llamadas), 3)
        self.assertIn("2 * n + 2", (self.dir / "resultado.lean").read_text(encoding="utf-8"))
        # el 2.º intento recibe el código anterior y el error de Lean (en el cuaderno original no ocurría)
        segundo = llm.llamadas[2]["u"]
        self.assertIn("TU INTENTO ANTERIOR", segundo)
        self.assertIn("omega", segundo)
        self.assertIn("unsolved goals", segundo)
        self.assertEqual(llm.llamadas[1]["format" if False else "json_mode"], False)
        self.assertTrue(llm.llamadas[0]["json_mode"])
        # las semillas de cada intento son distintas
        seeds = [c["seed"] for c in llm.llamadas]
        self.assertEqual(len(seeds), len(set(seeds)))
        self.assertTrue((self.dir / "registro.jsonl").exists())
        self.assertIn("✔", d.informe(self.dir))

    def test_construcciones_prohibidas_y_repetidas(self):
        malo = "```lean\n" + FIRMA + "\n  sorry\n```"
        nativo = "```lean\n" + FIRMA + "\n  native_decide\n```"
        llm = FakeLLM(self.router([malo, nativo, MAL_PAR, MAL_PAR, OK_PAR]))
        m = self.motor(llm, self.cfg(directos=6))
        self.assertEqual(m.ejecutar(self.problema()), 0)
        errores = " | ".join(self.estado()["raiz"]["errores"])
        self.assertIn("Construcción prohibida", errores)
        self.assertIn("repetida", errores)

    def test_enunciado_trivial_se_rechaza_y_se_reformaliza(self):
        respuestas = iter([formalizar("theorem t : True := by"), formalizar()])

        def f(u, kw, n):
            if "Traduce el siguiente problema" in u:
                return next(respuestas)
            return OK_PAR
        llm = FakeLLM(f)
        self.assertEqual(self.motor(llm).ejecutar(self.problema()), 0)
        self.assertIn("trivial", llm.llamadas[1]["u"])   # el error se le devuelve al modelo

    def test_fidelidad(self):
        resp = iter([{"equivalente": False, "motivo": "es más débil"}, {"equivalente": True, "motivo": ""}])

        def f(u, kw, n):
            if "Traduce el siguiente problema" in u:
                return formalizar()
            if "Comprueba si este enunciado" in u:
                return next(resp)
            return OK_PAR
        llm = FakeLLM(f)
        self.assertEqual(self.motor(llm, self.cfg(fidelidad=True)).ejecutar(self.problema()), 0)
        formalizaciones = [c for c in llm.llamadas if "Traduce el siguiente problema" in c["u"]]
        self.assertEqual(len(formalizaciones), 2)
        self.assertIn("Fidelidad", formalizaciones[1]["u"])

    def test_even_de_mathlib_recibe_una_pista_accionable(self):
        """Caso real: qwen2.5:7b escribió `even m` con `import Lean` y repitió el error 8 veces porque no era accionable."""
        resp = iter([formalizar("theorem t (n : Nat) : ∃ m, m > n ∧ even m := by"), formalizar()])

        def f(u, kw, n):
            if "Traduce el siguiente problema" in u:
                return next(resp)
            return OK_PAR
        llm = FakeLLM(f)
        self.assertEqual(self.motor(llm).ejecutar(self.problema()), 0)
        segunda = llm.llamadas[1]["u"]
        self.assertIn("unknown identifier 'even'", segunda)
        self.assertIn("PISTA", segunda)
        self.assertIn("m % 2 = 0", segunda)

    def test_un_estado_fallido_se_reintenta_al_volver_a_ejecutar(self):
        """Caso real: tras 8 formalizaciones fallidas, volver a ejecutar fallaba al instante sin intentar nada."""
        mala = formalizar("theorem t (n : Nat) : ∃ m, m > n ∧ even m := by")
        llm1 = FakeLLM(lambda u, kw, n: mala if "Traduce el siguiente problema" in u else OK_PAR)
        self.assertEqual(self.motor(llm1, self.cfg(formalizar=3)).ejecutar(self.problema()), 1)
        e = self.estado()
        self.assertEqual((e["estado"], e["intentos_formalizar"]), ("fallido", 3))
        self.assertFalse(e["ultimo_error"].startswith("No se pudo formalizar"))     # sin prefijo anidado
        self.assertTrue(e["error_fatal"].startswith("No se pudo formalizar"))
        # segunda ejecución con el modelo "arreglado": debe REINTENTAR (antes fallaba sin llamar al modelo)
        llm2 = FakeLLM(lambda u, kw, n: formalizar() if "Traduce el siguiente problema" in u else OK_PAR)
        self.assertEqual(self.motor(llm2, self.cfg(formalizar=3)).ejecutar(self.problema()), 0)
        self.assertGreaterEqual(len(llm2.llamadas), 2)
        self.assertTrue(any("se reintenta" in l for l in self.logs))

    def test_un_nodo_fallido_se_reabre_pero_no_repite_lo_descartado(self):
        self.oraculo([{"enunciado_contiene": "par_mayor", "acepta_contiene": ["2 * n + 2"]}])
        llm = FakeLLM(self.router([MAL_PAR, MAL_PAR, MAL_PAR]))
        self.assertEqual(self.motor(llm, self.cfg(profundidad=0)).ejecutar(self.problema()), 1)
        self.assertEqual(self.estado()["raiz"]["estado"], "fallido")
        llm2 = FakeLLM(self.router([MAL_PAR, OK_PAR]))   # la primera repite una demostración ya descartada
        self.assertEqual(self.motor(llm2, self.cfg(profundidad=0)).ejecutar(self.problema()), 0)
        self.assertIn("repetida", " ".join(self.estado()["raiz"]["errores"]))

    def test_repetir_sube_la_temperatura_y_se_ensenan_los_fallos(self):
        simp = "```lean\n" + FIRMA + "\n  simp\n```"
        decide = "```lean\n" + FIRMA + "\n  decide\n```"
        llm = FakeLLM(self.router([MAL_PAR, MAL_PAR, simp, decide, OK_PAR]))
        self.assertEqual(self.motor(llm, self.cfg(directos=6)).ejecutar(self.problema()), 0)
        # llamadas: 0 formalizar, 1 omega, 2 omega (repetida), 3 simp, 4 decide, 5 ok
        base3 = d.TEMPERATURAS[2]
        self.assertAlmostEqual(llm.llamadas[3]["temperatura"], base3 + 0.15)      # tras una repetición, más variedad
        self.assertIn("ESTRATEGIA SUGERIDA", llm.llamadas[1]["u"])
        quinto = llm.llamadas[5]["u"]
        self.assertIn("YA FALLARON", quinto)
        self.assertIn("omega", quinto)
        self.assertIn("simp", quinto)

    def test_pausa_por_presupuesto_y_reanudacion(self):
        llm = FakeLLM(self.router([MAL_PAR, MAL_PAR, OK_PAR]))
        m1 = self.motor(llm, self.cfg(max_intentos=2))
        self.assertEqual(m1.ejecutar(self.problema()), 3)
        e = self.estado()
        self.assertEqual(e["estado"], "pausado")
        self.assertTrue(e["formalizado"])
        self.assertEqual(e["raiz"]["directos"], 1)
        n_antes = len(llm.llamadas)
        m2 = self.motor(llm)       # nueva ejecución con el mismo directorio: continúa
        self.assertEqual(m2.ejecutar(self.problema()), 0)
        self.assertEqual(len(llm.llamadas) - n_antes, 2)    # NO vuelve a formalizar ni repite el intento 1
        self.assertEqual(self.estado()["llamadas"], 4)
        self.assertTrue(any("Reanudando" in l for l in self.logs))

    def test_interrupcion_ctrl_c_y_reanudacion(self):
        it = iter([MAL_PAR, OK_PAR])

        def f(u, kw, n):
            if "Traduce el siguiente problema" in u:
                return formalizar()
            if n == 3:
                return KeyboardInterrupt()
            return next(it)
        llm = FakeLLM(f)
        self.assertEqual(self.motor(llm).ejecutar(self.problema()), 130)
        self.assertEqual(self.estado()["estado"], "pausado")
        self.assertEqual(self.motor(llm).ejecutar(self.problema()), 0)

    def test_error_del_modelo_guarda_y_reanuda(self):
        caido = {"v": True}

        def f(u, kw, n):
            if "Traduce el siguiente problema" in u:
                return formalizar()
            if caido["v"]:
                caido["v"] = False
                return d.ErrorModelo("Ollama caído")
            return OK_PAR
        llm = FakeLLM(f)
        self.assertEqual(self.motor(llm).ejecutar(self.problema()), 4)
        self.assertTrue(self.estado()["formalizado"])
        self.assertEqual(self.motor(llm).ejecutar(self.problema()), 0)

    def test_sin_solucion_agota_y_falla(self):
        llm = FakeLLM(self.router([MAL_PAR, "```lean\n" + FIRMA + "\n  simp\n```", "```lean\n" + FIRMA + "\n  decide\n```"]))
        m = self.motor(llm, self.cfg(profundidad=0))
        self.assertEqual(m.ejecutar(self.problema()), 1)
        e = self.estado()
        self.assertEqual(e["estado"], "fallido")
        self.assertEqual(e["raiz"]["estado"], "fallido")
        self.assertIn("✘", d.informe(self.dir))

    def test_ya_verificado_no_llama_al_modelo(self):
        llm = FakeLLM(self.router([OK_PAR]))
        self.assertEqual(self.motor(llm).ejecutar(self.problema()), 0)
        n = len(llm.llamadas)
        self.assertEqual(self.motor(llm).ejecutar(self.problema()), 0)
        self.assertEqual(len(llm.llamadas), n)

    def test_estado_corrupto_y_problema_distinto(self):
        self.dir.mkdir(parents=True)
        (self.dir / "estado.json").write_text("{esto no es json", encoding="utf-8")
        llm = FakeLLM(self.router([OK_PAR]))
        self.assertEqual(self.motor(llm).ejecutar(self.problema()), 0)
        self.assertTrue(list(self.dir.glob("estado.*.corrupto.json")))
        # otro enunciado en el mismo directorio: se conserva copia y se empieza de nuevo
        llm2 = FakeLLM(self.router([OK_PAR]))
        self.assertEqual(self.motor(llm2).ejecutar(self.problema("Otro enunciado distinto.")), 0)
        self.assertTrue(list(self.dir.glob("estado.*.bak.json")))
        self.assertGreaterEqual(len(llm2.llamadas), 2)

    def test_axiomas_no_permitidos_bloquean_el_resultado(self):
        self.oraculo([{"enunciado_contiene": "par_mayor", "acepta_contiene": ["2 * n + 2"], "axiomas": ["miAxioma"]}])
        otra = "```lean\n" + FIRMA + "\n  exact ⟨2 * n + 2 + 0, by omega⟩\n```"   # distinta: la repetida se rechazaría
        llm = FakeLLM(self.router([OK_PAR, otra]))
        self.assertEqual(self.motor(llm).ejecutar(self.problema()), 1)
        self.assertIn("axiomas no permitidos", self.estado()["ultimo_error"])
        self.assertFalse((self.dir / "resultado.lean").exists())
        self.oraculo([{"enunciado_contiene": "par_mayor", "acepta_contiene": ["2 * n + 2"], "axiomas": ["propext", "Classical.choice", "Quot.sound"]}])
        self.assertEqual(self.motor(llm).ejecutar(self.problema()), 0)

    def test_tacticas_automaticas(self):
        self.oraculo([{"enunciado_contiene": "par_mayor", "auto": True}])
        llm = FakeLLM(self.router([]))
        self.assertEqual(self.motor(llm).ejecutar(self.problema()), 0)
        self.assertEqual(len(llm.llamadas), 1)   # solo formalizar: lo resolvió `first | omega | ...`
        self.assertIn("first", (self.dir / "resultado.lean").read_text(encoding="utf-8"))


class TestDescomposicion(Base):
    FIRMA_P = "theorem principal (n : Nat) : n + 0 + 0 = n := by"

    def lema(self, u):
        m = re.search(r"\bl\d+_aux\b", u)
        return m.group(0) if m else None

    def test_un_nivel_de_lemas(self):
        self.oraculo([{"enunciado_contiene": "principal", "acepta_contiene": ["exact"], "usa_lema": r"l\d+_aux"},
                      {"enunciado_contiene": "aux", "acepta_contiene": ["omega"]}])

        def f(u, kw, n):
            if "Traduce el siguiente problema" in u:
                return formalizar(self.FIRMA_P)
            if "Diseña una estrategia" in u:
                return {"lemas": ["theorem aux (n : Nat) : n + 0 = n := by"]}
            if "Demuestra este teorema en Lean 4" in u:
                if "theorem principal" in u.split("LEMAS YA DEMOSTRADOS")[0]:
                    nombre = self.lema(u)
                    cuerpo = f"exact {nombre} n" if nombre else "omega"
                    return "```lean\n" + self.FIRMA_P + f"\n  {cuerpo}\n```"
                return "```lean\ntheorem aux (n : Nat) : n + 0 = n := by\n  omega\n```"
            return {}
        llm = FakeLLM(f)
        self.assertEqual(self.motor(llm).ejecutar(self.problema("enunciado")), 0)
        codigo = (self.dir / "resultado.lean").read_text(encoding="utf-8")
        self.assertLess(codigo.index("_aux"), codigo.index("theorem principal"))   # el lema va ANTES
        raiz = self.estado()["raiz"]
        self.assertEqual(len(raiz["hijos"]), 1)
        self.assertEqual(raiz["hijos"][0]["estado"], "probado")

    def test_recursion_dos_niveles(self):
        self.oraculo([{"enunciado_contiene": "principal", "acepta_contiene": ["exact"], "usa_lema": r"l\d+_aux"},
                      {"enunciado_contiene": "aux", "acepta_contiene": ["exact"], "usa_lema": r"l\d+_sub"},
                      {"enunciado_contiene": "sub", "acepta_contiene": ["omega"]}])

        def f(u, kw, n):
            if "Traduce el siguiente problema" in u:
                return formalizar(self.FIRMA_P)
            if "Diseña una estrategia" in u:
                objetivo = u.split("TEOREMA PRINCIPAL:")[1].split("\n")[1]
                if "principal" in objetivo:
                    return {"lemas": ["theorem aux (n : Nat) : n + 0 = n := by"]}
                return {"lemas": ["theorem sub (n : Nat) : n = n + 0 := by"]}
            if "Demuestra este teorema en Lean 4" in u:
                objetivo = u.split("```lean\n")[1].split("\n")[0]
                ctx = u.split("LEMAS YA DEMOSTRADOS")[1]
                if "theorem principal" in objetivo:
                    nombre = self.lema(u)
                    return "```lean\n" + self.FIRMA_P + f"\n  exact {nombre} n\n```" if nombre else "```lean\n" + self.FIRMA_P + "\n  omega\n```"
                if "_aux" in objetivo:
                    sub = re.search(r"\bl\d+_sub\b", ctx)
                    firma = objetivo
                    return "```lean\n" + firma + (f"\n  exact {sub.group(0)} n\n```" if sub else "\n  omega\n```")
                return "```lean\n" + objetivo + "\n  omega\n```"
            return {}
        llm = FakeLLM(f)
        m = self.motor(llm, self.cfg(profundidad=2))
        self.assertEqual(m.ejecutar(self.problema("enunciado")), 0)
        codigo = (self.dir / "resultado.lean").read_text(encoding="utf-8")
        self.assertLess(codigo.index("_sub"), codigo.index("_aux"))
        self.assertLess(codigo.index("_aux"), codigo.index("theorem principal"))
        raiz = self.estado()["raiz"]
        self.assertEqual(raiz["hijos"][0]["hijos"][0]["prof"], 2)

    def test_reanuda_en_mitad_del_plan_sin_repetir_lo_demostrado(self):
        self.oraculo([{"enunciado_contiene": "principal", "acepta_contiene": ["exact"], "usa_lema": r"l\d+_aux"},
                      {"enunciado_contiene": "aux", "acepta_contiene": ["omega"]}])

        def f(u, kw, n):
            if "Traduce el siguiente problema" in u:
                return formalizar(self.FIRMA_P)
            if "Diseña una estrategia" in u:
                return {"lemas": ["theorem aux (n : Nat) : n + 0 = n := by"]}
            if "Demuestra este teorema en Lean 4" in u:
                if "theorem principal" in u.split("LEMAS YA DEMOSTRADOS")[0]:
                    nombre = self.lema(u)
                    return "```lean\n" + self.FIRMA_P + (f"\n  exact {nombre} n\n```" if nombre else "\n  omega\n```")
                return "```lean\ntheorem aux (n : Nat) : n + 0 = n := by\n  omega\n```"
            return {}
        llm = FakeLLM(f)
        # 1 formalizar + 3 directos + 1 plan + 1 lema = 6 llamadas -> pausa antes del intento final
        self.assertEqual(self.motor(llm, self.cfg(max_intentos=6)).ejecutar(self.problema("e")), 3)
        e = self.estado()
        self.assertEqual(e["raiz"]["hijos"][0]["estado"], "probado")
        self.assertEqual(e["raiz"]["finales"], 0)
        antes = len(llm.llamadas)
        self.assertEqual(self.motor(llm).ejecutar(self.problema("e")), 0)
        nuevas = [c["u"] for c in llm.llamadas[antes:]]
        self.assertEqual(len(nuevas), 1)                                   # solo el intento final
        self.assertNotIn("Diseña una estrategia", nuevas[0])               # no re-planifica
        self.assertIn("exact" if False else "LEMAS YA DEMOSTRADOS", nuevas[0])

    def test_un_lema_que_falla_se_descarta_y_se_conservan_los_probados(self):
        self.oraculo([{"enunciado_contiene": "principal", "acepta_contiene": ["exact"], "usa_lema": r"l\d+_buen"},
                      {"enunciado_contiene": "buen", "acepta_contiene": ["omega"]},
                      {"enunciado_contiene": "imposible", "acepta_contiene": ["NUNCA"]}])
        planes = iter([{"lemas": ["theorem buen (n : Nat) : n + 0 = n := by", "theorem imposible (n : Nat) : n = n + 1 := by"]},
                       {"lemas": ["theorem otro (n : Nat) : n = n := by"]}])

        def f(u, kw, n):
            if "Traduce el siguiente problema" in u:
                return formalizar(self.FIRMA_P)
            if "Diseña una estrategia" in u:
                return next(planes)
            if "Demuestra este teorema en Lean 4" in u:
                objetivo = u.split("```lean\n")[1].split("\n")[0]
                if "theorem principal" in objetivo:
                    m = re.search(r"\bl\d+_buen\b", u)
                    return "```lean\n" + self.FIRMA_P + (f"\n  exact {m.group(0)} n\n```" if m else "\n  omega\n```")
                return "```lean\n" + objetivo + "\n  omega\n```"
            return {}
        self.oraculo([{"enunciado_contiene": "principal", "acepta_contiene": ["exact"], "usa_lema": r"l\d+_buen"},
                      {"enunciado_contiene": "buen", "acepta_contiene": ["omega"]},
                      {"enunciado_contiene": "otro", "acepta_contiene": ["omega"]},
                      {"enunciado_contiene": "imposible", "acepta_contiene": ["NUNCA"]}])
        llm = FakeLLM(f)
        self.assertEqual(self.motor(llm, self.cfg(directos=2, profundidad=1)).ejecutar(self.problema("e")), 0)
        raiz = self.estado()["raiz"]
        nombres = [h["firma"] for h in raiz["hijos"]]
        self.assertTrue(any("buen" in x for x in nombres))
        self.assertFalse(any("imposible" in x for x in nombres))
        self.assertTrue(any("imposible" in x for x in raiz["fallidos"]))


class TestClienteHTTP(unittest.TestCase):
    def test_cliente_ollama(self):
        visto = {}

        class H(BaseHTTPRequestHandler):
            def do_POST(self):
                visto["body"] = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                b = json.dumps({"message": {"role": "assistant", "content": "hola"}}).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(b)))
                self.end_headers()
                self.wfile.write(b)

            def log_message(self, *a):
                pass
        srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.addCleanup(srv.shutdown)
        llm = d.LLM(d.Config(ollama_url=f"127.0.0.1:{srv.server_address[1]}", modelo="m:1", num_ctx=4096))
        self.assertEqual(llm.chat("sis", "usr", temperatura=0.3, num_predict=77, seed=5, json_mode=True), "hola")
        b = visto["body"]
        self.assertEqual((b["model"], b["format"], b["stream"], b["options"]["num_ctx"], b["options"]["seed"], b["options"]["num_predict"]), ("m:1", "json", False, 4096, 5, 77))
        self.assertEqual([m["role"] for m in b["messages"]], ["system", "user"])

    def test_sin_servidor_da_error_claro(self):
        llm = d.LLM(d.Config(ollama_url="http://127.0.0.1:1"))
        with mock.patch.object(d.time, "sleep"):
            with self.assertRaises(d.ErrorModelo) as cm:
                llm.chat("s", "u")
        self.assertIn("ollama serve", str(cm.exception))


class TestSympy(Base):
    """Física con SymPy REAL: velocidad de escape y radio de Schwarzschild."""
    ESPEC = {"descripcion": "Velocidad de escape v_esc(r) y comprobar que vale c en r = 2GM/c^2",
             "preambulo": "G, M, c, r = symbols('G M c r', positive=True)\nr_s = 2*G*M/c**2",
             "variables": ["v_esc"],
             "criterios": ["simplify(v_esc**2 - 2*G*M/r) == 0", "simplify(v_esc.subs(r, r_s) - c) == 0"]}
    BIEN = "```python\nm, v = symbols('m v', positive=True)\nE = Rational(1, 2)*m*v**2 - G*M*m/r\nv_esc = solve(Eq(E, 0), v)[0]\n```"
    MAL = "```python\nv_esc = c\n```"

    def cfg_sympy(self, **kw):
        base = dict(backend="sympy", fidelidad=False, directos=4, timeout_py=20)
        base.update(kw)
        return d.Config(**base)

    def motor_sympy(self, llm, **kw):
        cfg = self.cfg_sympy(**kw)
        return d.Motor(cfg, llm, d.SympyVerificador(cfg), self.dir, log=self.logs.append)

    def router(self, pruebas):
        it = iter(pruebas)

        def f(u, kw, n):
            if "Convierte este problema" in u:
                return self.ESPEC
            if "Resuelve este problema DERIVANDO" in u:
                return next(it)
            return {}
        return f

    def test_resuelve_con_reparacion(self):
        llm = FakeLLM(self.router([self.MAL, self.BIEN]))
        self.assertEqual(self.motor_sympy(llm).ejecutar(self.problema("Escape.", "sympy")), 0)
        self.assertIn("CRITERIO_FALLA", llm.llamadas[2]["u"])     # el fallo concreto se le devuelve al modelo
        res = (self.dir / "resultado.py").read_text(encoding="utf-8")
        self.assertIn("solve(Eq(E, 0), v)", res)
        self.assertIn("verificar(2,", res)                          # los criterios los pone el motor, no el modelo

    def test_biblioteca_aprende_y_reutiliza(self):
        bib = str(Path(self.td.name) / "bib.json")
        llm = FakeLLM(self.router([self.BIEN]))
        self.assertEqual(self.motor_sympy(llm, biblioteca=bib).ejecutar(self.problema("Velocidad de escape de un cuerpo.", "sympy")), 0)
        entradas = d.biblioteca_cargar(bib)
        self.assertEqual(len(entradas), 1)
        self.assertIn("solve(Eq(E, 0), v)", entradas[0]["codigo"])
        # un problema parecido recibe la demostración verificada como ejemplo
        ej = d.biblioteca_ejemplos(bib, "Velocidad de escape de otro cuerpo celeste.", "sympy")
        self.assertIn("EJEMPLOS VERIFICADOS", ej)
        self.assertIn("solve(Eq(E, 0), v)", ej)
        self.assertEqual(d.biblioteca_ejemplos(bib, "Velocidad de escape de un cuerpo.", "sympy"), "")   # no se copia a sí mismo
        self.assertEqual(d.biblioteca_ejemplos(bib, "Velocidad de escape de otro cuerpo.", "lean"), "")  # otro backend

    def test_error_de_preambulo_es_accionable(self):
        mala = dict(self.ESPEC, preambulo="x = indefinida + 1")
        llm = FakeLLM(lambda u, kw, n: (mala if n == 1 else self.ESPEC) if "Convierte este problema" in u else
                      ("```python\nm, v = symbols('m v', positive=True)\nv_esc = solve(Eq(Rational(1,2)*m*v**2 - G*M*m/r, 0), v)[0]\n```"))
        m = self.motor_sympy(llm)
        self.assertEqual(m.ejecutar(self.problema("Escape.", "sympy")), 0)
        self.assertIn("NameError", llm.llamadas[1]["u"])      # el reintento ve el error real, el preámbulo y la pista
        self.assertIn("TU PREÁMBULO", llm.llamadas[1]["u"])
        self.assertIn("PISTA", llm.llamadas[1]["u"])
        self.assertTrue(any("NameError" in l for l in self.logs if "rechazada" in l))   # el log muestra la excepción, no 'Traceback'

    def test_trampas_de_salida_y_criterios_no_booleanos(self):
        for trampa in ('v_esc = 0\nprint("TODO_VERIFICADO")\nraise SystemExit(0)', 'v_esc = 0\nimport os\nos._exit(0)',
                       'v_esc = 0\nprint("TODO_VERIFICADO")'):
            llm = FakeLLM(self.router(["```python\n" + trampa + "\n```", self.BIEN]))
            self.assertEqual(self.motor_sympy(llm).ejecutar(self.problema("Escape.", "sympy")), 0, trampa)
            self.setUp()
        espec = dict(self.ESPEC, criterios=["simplify(v_esc**2 - 2*G*M/r)"])   # Expr, no booleano: antes aceptaba v_esc = 0
        ok, _ = d.SympyVerificador(self.cfg_sympy()).comprobar(d.PRELUDIO_PY + "G, M, r = symbols('G M r', positive=True)\nv_esc = 0\n"
                                                              "verificar(1, simplify(v_esc**2 - 2*G*M/r))\nprint('TODO_VERIFICADO')", "x")
        self.assertFalse(ok)

    def test_la_trampa_no_pasa(self):
        trampa = "```python\nv_esc = sqrt(2*G*M/r)\nverificar(1, True)\n```"
        llm = FakeLLM(self.router([trampa, self.BIEN]))
        self.assertEqual(self.motor_sympy(llm).ejecutar(self.problema("Escape.", "sympy")), 0)
        self.assertIn("Construcción prohibida", " ".join(self.estado()["raiz"]["errores"]))

    def test_bucle_infinito_y_errores_de_python(self):
        llm = FakeLLM(self.router(["```python\nwhile True:\n    pass\n```", "```python\nv_esc = nodefinida + 1\n```", self.BIEN]))
        self.assertEqual(self.motor_sympy(llm, timeout_py=2).ejecutar(self.problema("Escape.", "sympy")), 0)
        errores = " | ".join(self.estado()["raiz"]["errores"])
        self.assertIn("límite", errores)
        self.assertIn("NameError", errores)

    def test_especificacion_invalida_se_reformula(self):
        malas = iter([{"descripcion": "x", "preambulo": "", "variables": ["v"], "criterios": ["sin cerrar ("]}, self.ESPEC])

        def f(u, kw, n):
            if "Convierte este problema" in u:
                return next(malas)
            return self.BIEN
        llm = FakeLLM(f)
        self.assertEqual(self.motor_sympy(llm).ejecutar(self.problema("Escape.", "sympy")), 0)
        self.assertIn("sintaxis", llm.llamadas[1]["u"])

    def test_pausa_y_reanudacion(self):
        llm = FakeLLM(self.router([self.MAL, self.BIEN]))
        self.assertEqual(self.motor_sympy(llm, max_intentos=2).ejecutar(self.problema("Escape.", "sympy")), 3)
        self.assertEqual(self.motor_sympy(llm).ejecutar(self.problema("Escape.", "sympy")), 0)


class TestProblemasYCLI(unittest.TestCase):
    def test_archivos_de_problemas_validos(self):
        for f in sorted((RAIZ / "problemas").glob("*.json")):
            p = json.loads(f.read_text(encoding="utf-8"))
            self.assertIn("enunciado", p, f.name)
            self.assertIn(p.get("backend", "lean"), ("lean", "sympy"), f.name)

    def test_notebook_de_colab_sincronizado_con_el_codigo(self):
        """El cuaderno lleva el código incrustado (no usa git clone): debe ser idéntico a demostrador.py."""
        nb = json.loads((RAIZ / "colab" / "Demostrador_Colab.ipynb").read_text(encoding="utf-8"))
        celdas = {}
        for c in nb["cells"]:
            fuente = "".join(c["source"])
            if c["cell_type"] == "code" and fuente.startswith("%%writefile "):
                nombre, _, resto = fuente.partition("\n")
                celdas[nombre[len("%%writefile "):]] = resto
        self.assertEqual(celdas["demostrador.py"].rstrip("\n"), (RAIZ / "demostrador.py").read_text(encoding="utf-8").rstrip("\n"),
                         "colab/Demostrador_Colab.ipynb está desactualizado: ejecuta  python tools/generar_notebook.py")
        __import__("ast").parse(celdas["demostrador.py"])      # el código incrustado es Python válido
        todo = "".join("".join(c["source"]) for c in nb["cells"] if c["cell_type"] == "code" and not "".join(c["source"]).startswith("%%writefile"))
        self.assertNotIn("problemas/", todo)                      # ninguna celda depende de archivos auxiliares
        self.assertIn("--problema par_mayor", todo)

    def test_problemas_incluidos_coinciden_con_los_archivos(self):
        for nombre, f in (("par_mayor", "facil_par_mayor.json"), ("escape_schwarzschild", "facil_velocidad_escape_schwarzschild.json")):
            archivo = json.loads((RAIZ / "problemas" / f).read_text(encoding="utf-8"))
            self.assertEqual(d.PROBLEMAS_INCLUIDOS[nombre], archivo, nombre)

    def test_problema_por_nombre_o_por_ruta_inexistente(self):
        """Caso real: `problemas/facil_par_mayor.json` no existía en Colab y el programa terminaba con un traceback."""
        a = d.construir_parser().parse_args(["resolver", "--problema", "par_mayor"])
        self.assertEqual(d.cargar_problema(a)["backend"], "lean")
        with tempfile.TemporaryDirectory() as td:
            cwd = os.getcwd()
            os.chdir(td)
            try:
                a = d.construir_parser().parse_args(["resolver", "--problema", "problemas/facil_par_mayor.json"])
                self.assertEqual(d.cargar_problema(a)["id"], "par_mayor")           # el archivo no existe: se usa el incluido
                a = d.construir_parser().parse_args(["resolver", "--problema", "problemas/facil_velocidad_escape_schwarzschild.json"])
                self.assertEqual(d.cargar_problema(a)["backend"], "sympy")
                a = d.construir_parser().parse_args(["resolver", "--problema", "no_existe.json"])
                with self.assertRaises(SystemExit) as cm:
                    d.cargar_problema(a)
                msg = str(cm.exception)
                self.assertIn("par_mayor", msg)
                self.assertIn("--enunciado", msg)
                (Path(td) / "mio.json").write_text(json.dumps({"enunciado": "  x  ", "backend": "sympy"}), encoding="utf-8")
                a = d.construir_parser().parse_args(["resolver", "--problema", "mio.json"])
                self.assertEqual((d.cargar_problema(a)["id"], d.cargar_problema(a)["enunciado"]), ("mio", "x"))   # un archivo real tiene prioridad
            finally:
                os.chdir(cwd)

    def test_cli_estado(self):
        with tempfile.TemporaryDirectory() as td:
            self.assertIn("No hay estado", d.informe(Path(td)))


if __name__ == "__main__":
    unittest.main(verbosity=2)
