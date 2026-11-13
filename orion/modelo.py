"""El modelo: pesos base de Hugging Face + un adaptador LoRA que Orion entrena, amplía y, si hace falta, capas nuevas.

Las dos ampliaciones conservan la función: justo después de ampliar, el modelo responde igual que antes. Lo nuevo
empieza sin efecto y solo cuenta si el entrenamiento posterior lo aprovecha (y la validación lo confirma).
"""
from __future__ import annotations

import copy
import shutil
from pathlib import Path

import torch
from peft import LoraConfig, PeftModel, get_peft_model
from transformers import AutoModelForCausalLM, AutoTokenizer

from .juez import MARCA

SISTEMA = ("Eres Orion. Resuelve el problema. Razona en pocas líneas y termina SIEMPRE con una última línea de la forma "
           f"'{MARCA} <resultado>' con solo el resultado, en notación de Python (* para multiplicar, ** para potencias, "
           "fracciones como 3/4).")
MODULOS_MLP = ["gate_proj", "up_proj", "down_proj"]


def elegir_modelo() -> str:
    """El mayor Qwen2.5 que se puede entrenar con holgura en la GPU que haya."""
    if not torch.cuda.is_available():
        return "Qwen/Qwen2.5-0.5B-Instruct"
    gb = torch.cuda.get_device_properties(0).total_memory / 2**30
    return "Qwen/Qwen2.5-3B-Instruct" if gb >= 20 else "Qwen/Qwen2.5-1.5B-Instruct" if gb >= 6 else "Qwen/Qwen2.5-0.5B-Instruct"


def _tipo() -> torch.dtype:
    if not torch.cuda.is_available():
        return torch.float32
    try:
        bf16 = torch.cuda.is_bf16_supported(including_emulation=False)
    except TypeError:
        bf16 = torch.cuda.is_bf16_supported()
    return torch.bfloat16 if bf16 else torch.float16


class Modelo:
    def __init__(self, base: str, adaptador: Path | None, rango: int, modulos: list[str]):
        self.dispositivo = "cuda" if torch.cuda.is_available() else "cpu"
        self.tipo = _tipo()
        self.nombre_base = base
        self.tok = AutoTokenizer.from_pretrained(base)
        if self.tok.pad_token_id is None:
            self.tok.pad_token = self.tok.eos_token
        nucleo = AutoModelForCausalLM.from_pretrained(base, torch_dtype=self.tipo).to(self.dispositivo)
        self.rango, self.modulos = rango, list(modulos)
        if adaptador and (adaptador / "adapter_config.json").exists():
            self.red = PeftModel.from_pretrained(nucleo, str(adaptador), is_trainable=True)
        else:
            self.red = get_peft_model(nucleo, self._config_lora(rango, modulos))
        self._entrenables_a_float32()

    # ------------------------------------------------------------ estructura
    @staticmethod
    def _config_lora(rango: int, modulos: list[str]) -> LoraConfig:
        # alpha proporcional al rango: la escala (alpha/r) no cambia al ampliar, así que ampliar conserva la función
        return LoraConfig(r=rango, lora_alpha=2 * rango, target_modules=list(modulos), lora_dropout=0.0, task_type="CAUSAL_LM")

    def _entrenables_a_float32(self) -> None:
        for p in self.red.parameters():
            if p.requires_grad:
                p.data = p.data.float()

    def parametros(self) -> tuple[int, int]:
        total = sum(p.numel() for p in self.red.parameters())
        return sum(p.numel() for p in self.red.parameters() if p.requires_grad), total

    def num_capas(self) -> int:
        return int(self.red.config.num_hidden_layers)

    def copia_adaptador(self) -> dict:
        return {n: p.detach().cpu().clone() for n, p in self.red.named_parameters() if p.requires_grad}

    def restaurar_adaptador(self, copia: dict) -> None:
        with torch.no_grad():
            for n, p in self.red.named_parameters():
                if n in copia:
                    p.copy_(copia[n].to(p.device, p.dtype))

    def guardar_adaptador(self, carpeta: Path) -> None:
        if carpeta.exists():
            shutil.rmtree(carpeta)
        self.red.save_pretrained(str(carpeta))

    def ampliar_adaptador(self, rango: int, modulos: list[str]) -> None:
        """Más rango y/o más módulos con LoRA. Los pesos aprendidos se copian; lo nuevo empieza sin efecto (B = 0)."""
        viejo = {n: p.detach().clone() for n, p in self.red.named_parameters() if "lora_" in n}
        nucleo = self.red.unload()                      # quita el adaptador sin tocar los pesos base
        self.red = get_peft_model(nucleo, self._config_lora(rango, modulos))
        with torch.no_grad():
            for n, p in self.red.named_parameters():
                if n in viejo:
                    v = viejo[n].to(p.device, p.dtype)
                    if "lora_B" in n:
                        p.zero_()
                        p[:, : v.shape[1]] = v
                    else:
                        p[: v.shape[0]] = v
        self.rango, self.modulos = rango, list(modulos)
        self._entrenables_a_float32()

    def anadir_capa(self, carpeta_base: Path) -> bool:
        """Funde el adaptador en los pesos, inserta un bloque nuevo que al principio no altera nada (sus proyecciones de
        salida son cero), guarda el resultado como modelo base nuevo y empieza un adaptador vacío sobre él."""
        nucleo = self.red.merge_and_unload()
        capas = getattr(getattr(nucleo, "model", None), "layers", None)
        if capas is None or not hasattr(capas[0], "self_attn") or not hasattr(capas[0].self_attn, "o_proj") \
                or not hasattr(capas[0], "mlp") or not hasattr(capas[0].mlp, "down_proj"):
            self.red = get_peft_model(nucleo, self._config_lora(self.rango, self.modulos))
            self._entrenables_a_float32()
            return False                                # arquitectura no reconocida: no se toca
        pos = max(1, (len(capas) * 3) // 4)
        nueva = copy.deepcopy(capas[pos - 1])
        with torch.no_grad():
            for salida in (nueva.self_attn.o_proj, nueva.mlp.down_proj):
                salida.weight.zero_()
                if salida.bias is not None:
                    salida.bias.zero_()
        capas.insert(pos, nueva)
        nucleo.config.num_hidden_layers = len(capas)
        if carpeta_base.exists():
            shutil.rmtree(carpeta_base)
        nucleo.save_pretrained(str(carpeta_base))
        self.tok.save_pretrained(str(carpeta_base))
        del nucleo, self.red
        if self.dispositivo == "cuda":
            torch.cuda.empty_cache()
        # se vuelve a cargar del disco para que cada capa tenga bien su índice interno
        nucleo = AutoModelForCausalLM.from_pretrained(str(carpeta_base), torch_dtype=self.tipo).to(self.dispositivo)
        self.nombre_base = str(carpeta_base)
        self.red = get_peft_model(nucleo, self._config_lora(self.rango, self.modulos))
        self._entrenables_a_float32()
        return True

    # ------------------------------------------------------------ uso
    def texto_chat(self, enunciado: str, respuesta: str | None = None) -> str:
        mensajes = [{"role": "system", "content": SISTEMA}, {"role": "user", "content": enunciado}]
        t = self.tok.apply_chat_template(mensajes, tokenize=False, add_generation_prompt=True)
        return t if respuesta is None else t + respuesta + (self.tok.eos_token or "")

    def _autocast(self):
        return torch.autocast("cuda", dtype=self.tipo, enabled=self.dispositivo == "cuda")

    @torch.no_grad()
    def generar(self, enunciados: list[str], n: int = 1, temperatura: float = 0.0, max_tokens: int = 192, lote: int = 16,
                parar=None) -> list[list[str]]:
        """n respuestas por enunciado. Con temperatura 0 es determinista (y n debe ser 1)."""
        self.red.eval()
        self.tok.padding_side = "left"
        salida: list[list[str]] = []
        for i in range(0, len(enunciados), lote):
            if parar and parar():
                break
            textos = [self.texto_chat(e) for e in enunciados[i:i + lote]]
            ent = self.tok(textos, return_tensors="pt", padding=True, add_special_tokens=False).to(self.dispositivo)
            muestreo = dict(do_sample=True, temperature=temperatura, top_p=0.95, num_return_sequences=n) if temperatura > 0 else dict(do_sample=False)
            with self._autocast():
                gen = self.red.generate(**ent, max_new_tokens=max_tokens, pad_token_id=self.tok.pad_token_id, **muestreo)
            nuevos = self.tok.batch_decode(gen[:, ent["input_ids"].shape[1]:], skip_special_tokens=True)
            k = n if temperatura > 0 else 1
            salida += [[t.strip() for t in nuevos[j * k:(j + 1) * k]] for j in range(len(textos))]
        return salida

    def entrenar(self, ejemplos: list[tuple[str, str]], epocas: int, lote: int, lr: float, semilla: int, parar=None) -> float:
        """Ajusta el adaptador con pares (enunciado, respuesta). La pérdida solo cuenta en la respuesta. Devuelve la pérdida final media."""
        import random
        self.red.train()
        self.tok.padding_side = "right"
        datos = []
        for enunciado, respuesta in ejemplos:
            pre = self.tok(self.texto_chat(enunciado), add_special_tokens=False)["input_ids"]
            todo = self.tok(self.texto_chat(enunciado, respuesta), add_special_tokens=False)["input_ids"][:512]
            datos.append((todo, min(len(pre), len(todo))))
        params = [p for p in self.red.parameters() if p.requires_grad]
        opt = torch.optim.AdamW(params, lr=lr, weight_decay=0.0)
        escala = torch.amp.GradScaler("cuda", enabled=self.tipo == torch.float16)
        r = random.Random(semilla)
        pasos_totales = max(1, epocas * ((len(datos) + lote - 1) // lote))
        paso, perdidas = 0, []
        for _ in range(epocas):
            r.shuffle(datos)
            for i in range(0, len(datos), lote):
                if parar and parar():
                    return float("nan")
                grupo = datos[i:i + lote]
                largo = max(len(t) for t, _ in grupo)
                ids = torch.full((len(grupo), largo), self.tok.pad_token_id, dtype=torch.long)
                etiquetas = torch.full((len(grupo), largo), -100, dtype=torch.long)
                mascara = torch.zeros((len(grupo), largo), dtype=torch.long)
                for j, (t, ini) in enumerate(grupo):
                    ids[j, :len(t)] = torch.tensor(t)
                    mascara[j, :len(t)] = 1
                    etiquetas[j, ini:len(t)] = torch.tensor(t[ini:])
                for g in opt.param_groups:                      # calentamiento breve y bajada lineal
                    g["lr"] = lr * min(1.0, (paso + 1) / max(1, pasos_totales // 10)) * max(0.1, 1 - paso / pasos_totales)
                with self._autocast():
                    perdida = self.red(input_ids=ids.to(self.dispositivo), attention_mask=mascara.to(self.dispositivo),
                                       labels=etiquetas.to(self.dispositivo)).loss
                if not torch.isfinite(perdida):
                    return float("nan")
                opt.zero_grad(set_to_none=True)
                escala.scale(perdida).backward()
                escala.unscale_(opt)
                torch.nn.utils.clip_grad_norm_(params, 1.0)
                escala.step(opt)
                escala.update()
                perdidas.append(float(perdida.detach()))
                paso += 1
        cola = perdidas[-max(1, len(perdidas) // 4):]
        return sum(cola) / len(cola)
