#!/usr/bin/env python3
"""Lean 4 SIMULADO para probar el motor sin Lean instalado (NO es un verificador real).

Imita la interfaz `lean archivo.lean` y el formato de sus mensajes. Qué demuestra lo decide un ORÁCULO (JSON en
la variable MOCK_LEAN_ORACLE): para cada teorema cuyo enunciado contenga cierto texto, qué cuerpos acepta.
Además comprueba lo que Lean comprueba por estructura: que los lemas usados estén definidos ANTES, `sorry`,
`#print axioms` y errores de sintaxis básicos (delimitadores sin cerrar).
"""
import json, os, re, sys


def norm(s):
    return re.sub(r"\s+", " ", s).strip()


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--version":
        print("Lean (version 4.0.0-mock, x86_64-unknown-linux-gnu)")
        return 0
    ruta = sys.argv[-1]
    texto = open(ruta, encoding="utf-8").read()
    oraculo = {"teoremas": []}
    if os.environ.get("MOCK_LEAN_ORACLE"):
        oraculo = json.load(open(os.environ["MOCK_LEAN_ORACLE"], encoding="utf-8"))
    if os.environ.get("MOCK_LEAN_SLEEP"):
        import time
        time.sleep(float(os.environ["MOCK_LEAN_SLEEP"]))
    lineas = texto.split("\n")
    # --- declaraciones de nivel superior
    decl = []   # (linea, nombre, enunciado, cuerpo)
    i = 0
    while i < len(lineas):
        m = re.match(r"theorem\s+(\S+)", lineas[i])
        if m:
            ini = i
            j = i
            while j < len(lineas) and ":=" not in lineas[j]:
                j += 1
            enun_txt = "\n".join(lineas[ini:j + 1])
            enun = enun_txt.split(":=")[0]
            resto = enun_txt.split(":=", 1)[1]
            cuerpo = [re.sub(r"^\s*by\b", "", resto)]
            k = j + 1
            while k < len(lineas) and (not lineas[k].strip() or lineas[k].startswith((" ", "\t"))):
                cuerpo.append(lineas[k])
                k += 1
            decl.append((ini + 1, m.group(1), norm(enun), "\n".join(cuerpo)))
            i = k
        else:
            i += 1
    salida, errores = [], 0
    probados = {}
    definidos = []
    for linea, nombre, enun, cuerpo in decl:
        c = norm(cuerpo)
        # delimitadores
        for a, b in (("(", ")"), ("⟨", "⟩"), ("[", "]")):
            if c.count(a) != c.count(b):
                salida.append(f"Problema.lean:{linea}:4: error: unexpected token; expected '{b}'")
                errores += 1
                break
        else:
            usados = [x for x in re.findall(r"\bl\d+_\w+", c) if x != nombre]
            desconocidos = [x for x in usados if x not in definidos]
            if "sorry" in c:
                salida.append(f"Problema.lean:{linea}:8: warning: declaration uses 'sorry'")
                probados[nombre] = ["sorryAx"]
            elif desconocidos:
                salida.append(f"Problema.lean:{linea}:2: error: unknown identifier '{desconocidos[0]}'")
                errores += 1
            else:
                regla = next((r for r in oraculo["teoremas"]
                              if all(t in enun for t in ([r["enunciado_contiene"]] if isinstance(r["enunciado_contiene"], str) else r["enunciado_contiene"]))), None)
                ok = False
                if regla:
                    es_first = c.startswith("first")
                    if es_first:
                        ok = bool(regla.get("auto"))
                    else:
                        ok = any(t in c for t in regla.get("acepta_contiene", []))
                        pat = regla.get("usa_lema")
                        if ok and pat:
                            ok = re.search(pat, c) is not None
                if ok:
                    probados[nombre] = regla.get("axiomas", [])
                else:
                    salida.append(f"Problema.lean:{linea}:2: error: unsolved goals\n⊢ {enun[:120]}")
                    errores += 1
        definidos.append(nombre)
    for m in re.finditer(r"#print axioms (\S+)", texto):
        n = m.group(1)
        if n not in probados:
            salida.append(f"Problema.lean:99:0: error: unknown constant '{n}'")
            errores += 1
        elif probados[n]:
            salida.append(f"'{n}' depends on axioms: [{', '.join(probados[n])}]")
        else:
            salida.append(f"'{n}' does not depend on any axioms")
    print("\n".join(salida))
    return 1 if errores else 0


if __name__ == "__main__":
    sys.exit(main())
