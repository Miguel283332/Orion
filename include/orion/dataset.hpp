// Corpus de bytes con memoria incremental.
//
// El corpus es un flujo de bytes que SOLO crece por el final (append-only), así la partición
// train/val/test de cada posición es estable aunque lleguen datos nuevos durante la ejecución:
//   trozos de 256 bytes; trozo%20 in {0,1} -> validación (10%), ==10 -> test (5%), resto -> train.
// La auto-mejora decide con `val`; `test` solo se informa (no se usa para elegir) para detectar
// si el sistema se está sobreajustando a la validación.
#pragma once
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace orion {

enum class Split { Train, Val, Test };

class Dataset {
public:
    static constexpr size_t kChunk = 256;

    // Añade al final del flujo (con separador). Devuelve bytes añadidos.
    size_t addText(const std::string& text);
    // Carga todos los .txt/.md/.cpp/.hpp/... de un directorio (ordenados) o un archivo.
    // Recuerda qué leyó para que `refresh()` solo añada lo nuevo de archivos que crecen.
    size_t addPath(const std::string& path, bool recursive = false);
    // Relee los archivos registrados y añade los bytes nuevos. Devuelve true si el corpus creció.
    bool refresh();

    size_t size() const { return bytes_.size(); }
    size_t count(Split s) const { return pos(s).size(); }
    const std::vector<uint8_t>& bytes() const { return bytes_; }

    void sample(Split s, int B, int ctx, std::mt19937_64& rng, std::vector<int>& ids, std::vector<int>& tgt) const;
    // Conjunto de evaluación determinista (como mucho maxN posiciones repartidas por todo el split).
    void evalSet(Split s, int ctx, size_t maxN, std::vector<int>& ids, std::vector<int>& tgt) const;
    // Contexto para generar: últimos `ctx` bytes de `text` (relleno con 0 por la izquierda).
    static std::vector<int> contextOf(const std::string& text, int ctx);

private:
    const std::vector<uint32_t>& pos(Split s) const;
    void appendBytes(const uint8_t* p, size_t n);
    void fillCtx(size_t t, int ctx, int* out) const;

    std::vector<uint8_t> bytes_;
    std::vector<uint32_t> train_, val_, test_;
    std::map<std::string, uint64_t> consumed_;  // archivo -> bytes ya leídos
};

}  // namespace orion
