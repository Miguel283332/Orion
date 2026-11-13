"""Pruebas con una red de verdad (diminuta, creada aquí, con la misma arquitectura que Qwen/Llama): comprueban que
entrenar cambia los pesos, que deshacer los recupera y que ampliar la capacidad no altera lo que la red ya hacía."""
import pytest

torch = pytest.importorskip("torch")
pytest.importorskip("peft")
from tokenizers import Tokenizer, decoders, models, pre_tokenizers, trainers
from transformers import LlamaConfig, LlamaForCausalLM, PreTrainedTokenizerFast

from orion.config import Estado, escribir_json
from orion.modelo import MODULOS_MLP, Modelo
from orion.problemas import conjunto_fijo

PLANTILLA = ("{% for m in messages %}<|im_start|>{{ m['role'] }}\n{{ m['content'] }}<|im_end|>\n{% endfor %}"
             "{% if add_generation_prompt %}<|im_start|>assistant\n{% endif %}")


@pytest.fixture(scope="module")
def base(tmp_path_factory):
    carpeta = tmp_path_factory.mktemp("base")
    tk = Tokenizer(models.BPE())
    tk.pre_tokenizer = pre_tokenizers.ByteLevel(add_prefix_space=False)
    tk.decoder = decoders.ByteLevel()
    tk.train_from_iterator([p.enunciado for p in conjunto_fijo(90, 1)],
                           trainers.BpeTrainer(vocab_size=320, special_tokens=["<pad>", "<|im_start|>", "<|im_end|>"],
                                               initial_alphabet=pre_tokenizers.ByteLevel.alphabet()))
    tok = PreTrainedTokenizerFast(tokenizer_object=tk, pad_token="<pad>", eos_token="<|im_end|>")
    tok.chat_template = PLANTILLA
    tok.save_pretrained(carpeta)
    torch.manual_seed(0)
    cfg = LlamaConfig(vocab_size=len(tok), hidden_size=64, intermediate_size=128, num_hidden_layers=4, num_attention_heads=4,
                      num_key_value_heads=2, max_position_embeddings=1024, pad_token_id=tok.pad_token_id, eos_token_id=tok.eos_token_id,
                      bos_token_id=tok.eos_token_id)
    LlamaForCausalLM(cfg).save_pretrained(carpeta)
    return carpeta


EJEMPLOS = [(f"Calcula: {a} + {b}", f"RESPUESTA: {a + b}") for a in range(2, 8) for b in range(2, 6)]


def logits(m: Modelo):
    m.red.eval()
    m.tok.padding_side = "right"
    ent = m.tok([m.texto_chat("Calcula: 3 + 4"), m.texto_chat("Deriva respecto de x: x**2")], return_tensors="pt", padding=True, add_special_tokens=False)
    with torch.no_grad():
        return m.red(**ent).logits


def test_entrenar_cambia_los_pesos_y_deshacer_los_recupera(base):
    m = Modelo(str(base), None, 4, ["q_proj", "v_proj"])
    entrenables, total = m.parametros()
    assert 0 < entrenables < total
    antes, copia = logits(m), m.copia_adaptador()
    p1 = m.entrenar(EJEMPLOS, 1, 8, 1e-2, 0)
    p2 = m.entrenar(EJEMPLOS, 6, 8, 1e-2, 0)
    assert p2 < p1                                              # aprende
    assert not torch.allclose(antes, logits(m), atol=1e-4)      # los pesos han cambiado de verdad
    m.restaurar_adaptador(copia)
    assert torch.allclose(antes, logits(m), atol=1e-6)


def test_guardar_y_cargar_el_adaptador(base, tmp_path):
    m = Modelo(str(base), None, 4, ["q_proj", "v_proj"])
    m.entrenar(EJEMPLOS, 3, 8, 1e-2, 0)
    m.guardar_adaptador(tmp_path / "a")
    m2 = Modelo(str(base), tmp_path / "a", 4, ["q_proj", "v_proj"])
    assert torch.allclose(logits(m), logits(m2), atol=1e-5)
    assert m2.parametros()[0] == m.parametros()[0]              # sigue siendo entrenable


def test_ampliar_el_adaptador_conserva_la_funcion(base):
    m = Modelo(str(base), None, 4, ["q_proj", "v_proj"])
    m.entrenar(EJEMPLOS, 3, 8, 1e-2, 0)
    antes, n = logits(m), m.parametros()[0]
    m.ampliar_adaptador(8, ["q_proj", "v_proj"])
    assert torch.allclose(antes, logits(m), atol=1e-5) and m.parametros()[0] == 2 * n
    m.ampliar_adaptador(8, ["q_proj", "v_proj"] + MODULOS_MLP)
    assert torch.allclose(antes, logits(m), atol=1e-5) and m.parametros()[0] > 2 * n
    p = m.entrenar(EJEMPLOS, 2, 8, 1e-2, 0)                    # y lo nuevo se puede entrenar
    assert p == p and not torch.allclose(antes, logits(m), atol=1e-4)


def test_anadir_una_capa_conserva_la_funcion_y_la_capa_aprende(base, tmp_path):
    m = Modelo(str(base), None, 4, ["q_proj", "v_proj", "o_proj", "down_proj"])
    m.entrenar(EJEMPLOS, 3, 8, 1e-2, 0)
    antes, total = logits(m), m.parametros()[1]
    assert m.anadir_capa(tmp_path / "base5")
    assert m.num_capas() == 5 and m.parametros()[1] > total
    assert torch.allclose(antes, logits(m), atol=1e-4)          # recién añadida, no altera nada
    m2 = Modelo(str(tmp_path / "base5"), None, 4, ["q_proj", "v_proj", "o_proj", "down_proj"])   # la base nueva está completa en disco
    assert m2.num_capas() == 5 and torch.allclose(antes, logits(m2), atol=1e-4)
    nueva = m.red.base_model.model.model.layers[3].self_attn.o_proj
    assert float(nueva.weight.abs().sum()) == 0.0
    m.entrenar(EJEMPLOS, 2, 8, 1e-2, 0)
    assert float(nueva.lora_B["default"].weight.abs().sum()) > 0   # la capa nueva recibe aprendizaje


def test_generar_es_determinista_y_da_n_intentos(base):
    m = Modelo(str(base), None, 4, ["q_proj", "v_proj"])
    e = ["Calcula: 3 + 4", "Calcula: 10 - 2", "Deriva respecto de x: x**2"]
    a, b = m.generar(e, max_tokens=8, lote=2), m.generar(e, max_tokens=8, lote=3)
    assert a == b and [len(x) for x in a] == [1, 1, 1]
    assert [len(x) for x in m.generar(e, n=3, temperatura=0.9, max_tokens=8, lote=2)] == [3, 3, 3]


def test_ciclo_completo_con_red_real(base, tmp_path):
    """Varias generaciones de principio a fin: una red sin preentrenar no acierta, así que se estanca y recorre todas las ampliaciones."""
    from orion.bucle import Orion
    escribir_json(tmp_path / "config.json", {"modelo": str(base), "n_validacion": 18, "n_examen": 18, "problemas_por_generacion": 6,
                                             "intentos_por_problema": 2, "max_tokens": 6, "ejemplos_por_entrenamiento": 16, "epocas": 1,
                                             "paciencia": 1, "rango_lora": 4, "rango_max": 8, "max_capas_nuevas": 1})
    mensajes = []
    o = Orion(tmp_path, decir=mensajes.append)
    o.vivir(generaciones=4)
    e = Estado.cargar(tmp_path)
    assert e.generacion == 4 and e.rango == 8 and e.capas_nuevas == 1 and o.modelo.num_capas() == 5
    assert set(MODULOS_MLP) <= set(e.modulos) and len(e.validacion) == 18
    assert any("me añado una capa nueva (4 → 5 capas)" in m for m in mensajes)
    o2 = Orion(tmp_path, decir=mensajes.append)                # reinicio: carga la base ampliada y su adaptador
    assert o2.modelo.num_capas() == 5 and o2.medir(o2.validacion) == e.validacion
    assert "5 (1 añadidas por mí)" in o2.resumen()
