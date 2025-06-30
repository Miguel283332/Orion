#include <iostream>
#include <filesystem>
#include <random>
#include <cmath>
#include "region.hpp"

int main() {
    namespace fs = std::filesystem;

    if (!fs::exists("structure")) {
        fs::create_directory("structure");
    }

    bool estructuraVacia = true;
    for (const auto& e : fs::directory_iterator("structure")) {
        if (e.path().extension() == ".rgn") {
            estructuraVacia = false;
            break;
        }
    }

    if (estructuraVacia) {
        int chunks;
        int neuronasPorChunk;
        float distancia;

        std::cout << "Numero de chunks: ";
        if (!(std::cin >> chunks)) {
            std::cerr << "Entrada no valida" << std::endl;
            return 1;
        }

        std::cout << "Neuronas por chunk: ";
        if (!(std::cin >> neuronasPorChunk)) {
            std::cerr << "Entrada no valida" << std::endl;
            return 1;
        }

        std::cout << "Distancia entre neuronas: ";
        if (!(std::cin >> distancia)) {
            std::cerr << "Entrada no valida" << std::endl;
            return 1;
        }

        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> ntDist(0, 4); // Neurotransmisores definidos
        std::uniform_int_distribution<int> recDist(0, 4);

        int idGlobal = 1;

        for (int c = 1; c <= chunks; ++c) {
            RegionCerebral region;
            region.nombre = "chunk" + std::to_string(c);

            int dim = std::ceil(std::cbrt(static_cast<double>(neuronasPorChunk)));
            int count = 0;

            for (int x = 0; x < dim && count < neuronasPorChunk; ++x) {
                for (int y = 0; y < dim && count < neuronasPorChunk; ++y) {
                    for (int z = 0; z < dim && count < neuronasPorChunk; ++z) {
                        Neurona n;
                        n.id = idGlobal++;
                        n.x = x * distancia;
                        n.y = y * distancia;
                        n.z = z * distancia;
                        n.nt = static_cast<Neurotransmisor>(ntDist(rng));
                        n.receptores.push_back(static_cast<Receptor>(recDist(rng)));

                        // conexiones aleatorias dentro del mismo chunk
                        int maxCon = std::min(5, neuronasPorChunk - 1);
                        std::uniform_int_distribution<int> conCountDist(1, maxCon);
                        std::uniform_int_distribution<int> conDist(1, neuronasPorChunk);
                        int numCon = conCountDist(rng); // numero variable de conexiones
                        for (int k = 0; k < numCon; ++k) {
                            Conexion cox;
                            cox.destinoID = idGlobal - 1 - conDist(rng) % neuronasPorChunk;
                            if (cox.destinoID < (idGlobal - neuronasPorChunk))
                                cox.destinoID = idGlobal - 1; // asegurarse de no ir atras demasiado
                            cox.tipoNT = static_cast<Neurotransmisor>(ntDist(rng));
                            n.conexiones.push_back(cox);
                        }

                        region.neuronas.push_back(n);
                        ++count;
                    }
                }
            }

            std::string path = "structure/chunk" + std::to_string(c) + ".rgn";
            region.guardarEnArchivo(path);
            std::cout << "[CREADO] " << path << std::endl;

            // Mostrar conexiones de cada neurona creadas en este chunk
            for (const auto& n : region.neuronas) {
                std::cout << "  Neurona " << n.id << " conexiones: ";
                for (size_t i = 0; i < n.conexiones.size(); ++i) {
                    std::cout << n.conexiones[i].destinoID;
                    if (i != n.conexiones.size() - 1) std::cout << ", ";
                }
                if (n.conexiones.empty()) std::cout << "(sin conexiones)";
                std::cout << std::endl;
            }
        }

        std::cout << "Estructura creada con " << chunks << " chunks." << std::endl;
    } else {
        std::cout << "Estructura ya existente. No se realizaron cambios." << std::endl;
    }

    return 0;
}
