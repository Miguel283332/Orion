"""Razonamiento lógico y manipulación de texto: problemas generados con respuesta exacta."""
from __future__ import annotations

import random

from .problemas import Problema

DIAS = ["lunes", "martes", "miércoles", "jueves", "viernes", "sábado", "domingo"]
NOMBRES = ["Ana", "Luis", "Eva", "Hugo", "Sara", "Iván", "Nora", "Pablo"]
PALABRAS = ["casa", "perro", "luna", "sol", "camino", "ventana", "planeta", "estrella", "tiempo", "materia", "energia", "pensar", "libro",
            "montaña", "rio", "ciudad", "maquina", "numero", "memoria", "futuro", "ciencia", "universo", "galaxia", "robot", "red",
            "neurona", "codigo", "teorema", "espacio", "velocidad", "silencio", "puente", "bosque", "fuego", "agua", "tierra", "viento"]
PALABRAS = [p for p in PALABRAS if "ñ" not in p]


def _caso(r: random.Random, nivel: int, casos: list):
    return casos[min(len(casos) - 1, r.randint(max(0, nivel - 2), nivel))]()


def _booleana(r: random.Random, profundidad: int) -> str:
    if profundidad == 0:
        return r.choice(["True", "False"])
    forma = r.randint(0, 2)
    if forma == 0:
        return f"not {_booleana(r, profundidad - 1)}" if profundidad == 1 else f"not ({_booleana(r, profundidad - 1)})"
    return f"({_booleana(r, profundidad - 1)} {'and' if forma == 1 else 'or'} {_booleana(r, r.randint(0, profundidad - 1))})"


def logica(r: random.Random, nivel: int) -> Problema:
    def secuencia():
        a, d = r.randint(-20, 40), r.randint(2, 6 * nivel)
        if nivel <= 2:
            v = [a + d * i for i in range(6)]
        elif nivel == 3:
            q = r.choice([2, 3])
            v = [abs(a) % 7 + 1]
            for _ in range(5):
                v.append(v[-1] * q)
        else:                                   # las diferencias crecen de forma constante, o cada término suma los dos anteriores
            if r.random() < 0.5:
                c = r.randint(1, 4)
                v = [a + d * i + c * i * i for i in range(7)]
            else:
                v = [r.randint(1, 9), r.randint(1, 9)]
                for _ in range(5):
                    v.append(v[-1] + v[-2])
        return f"¿Qué número sigue en esta secuencia? {', '.join(map(str, v[:-1]))}. Responde solo con el número.", "num", str(v[-1])

    def dia():
        hoy, n = r.randrange(7), r.randint(2, 10 ** min(nivel, 3) * 3)
        atras = nivel >= 3 and r.random() < 0.5
        return (f"Si hoy es {DIAS[hoy]}, ¿qué día de la semana {'fue hace' if atras else 'será dentro de'} {n} días? Responde solo con el día.",
                "texto", DIAS[(hoy - n if atras else hoy + n) % 7])

    def booleana():
        e = _booleana(r, min(4, nivel))
        return f"¿Cuánto vale esta expresión de Python? {e}. Responde True o False.", "texto", str(eval(e))   # expresión generada aquí

    def orden():
        gente = r.sample(NOMBRES, min(len(NOMBRES), 2 + nivel))
        frases = [f"{gente[i]} mide más que {gente[i + 1]}" if r.random() < 0.5 else f"{gente[i + 1]} mide menos que {gente[i]}"
                  for i in range(len(gente) - 1)]
        r.shuffle(frases)
        pregunta, resp = r.choice([("Quién es la persona que más mide", gente[0]), ("Quién es la persona que menos mide", gente[-1])] +
                                  ([("Quién es la segunda persona que más mide", gente[1])] if nivel >= 4 else []))
        return f"{'. '.join(frases)}. ¿{pregunta} de todas ellas? Responde solo con el nombre.", "texto", resp

    def conjuntos():
        ambos, solo_a, solo_b, ninguno = (r.randint(1, 6 * nivel) for _ in range(4))
        total = ambos + solo_a + solo_b + ninguno
        return (f"En un grupo de {total} personas, {solo_a + ambos} hablan inglés, {solo_b + ambos} hablan francés y {ambos} hablan los dos "
                "idiomas. ¿Cuántas no hablan ninguno de los dos? Responde solo con el número."), "num", str(ninguno)

    def potencia():
        a, b, m = r.randint(2, 12), r.randint(5, 40), r.randint(3, 13)
        return f"¿Cuál es el resto de dividir {a}^{b} entre {m}? Responde solo con el número.", "num", str(pow(a, b, m))

    enunciado, tipo, verdad = _caso(r, nivel, [secuencia, dia, booleana, orden, conjuntos, potencia])
    return Problema("logica", nivel, enunciado, tipo, verdad)


def lenguaje(r: random.Random, nivel: int) -> Problema:
    def palabra(minimo=0):
        return r.choice([p for p in PALABRAS if len(p) >= minimo] or PALABRAS)

    def frase(n):
        return " ".join(r.sample(PALABRAS, n))

    def letras():
        p = palabra()
        return f"¿Cuántas letras tiene la palabra «{p}»? Responde solo con el número.", "num", str(len(p))

    def reves():
        p = palabra(3 + nivel) if nivel < 4 else frase(2)
        return f"Escribe «{p}» al revés, letra a letra.", "texto", p[::-1]

    def vocales():
        p = palabra(5) if nivel < 3 else frase(nivel - 1)
        return f"¿Cuántas vocales hay en «{p}»? Responde solo con el número.", "num", str(sum(c in "aeiou" for c in p))

    def ordenar():
        ps = r.sample(PALABRAS, 2 + nivel)
        return f"Ordena alfabéticamente estas palabras y sepáralas con comas: {', '.join(ps)}.", "texto", ", ".join(sorted(ps))

    def apariciones():
        f = frase(1 + nivel)
        c = r.choice([c for c in set(f) if c != " "])
        return f"¿Cuántas veces aparece la letra «{c}» en «{f}»? Responde solo con el número.", "num", str(f.count(c))

    def cesar():
        p, k = palabra(5), r.randint(1, 5 if nivel < 5 else 20)
        return (f"Cifra la palabra «{p}» con el cifrado César avanzando cada letra {k} posiciones en el alfabeto de 26 letras "
                "(tras la z se vuelve a la a).", "texto", "".join(chr((ord(c) - 97 + k) % 26 + 97) for c in p))

    enunciado, tipo, verdad = _caso(r, nivel, [letras, reves, vocales, ordenar, apariciones, cesar])
    return Problema("lenguaje", nivel, enunciado, tipo, verdad)
