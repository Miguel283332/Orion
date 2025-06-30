#include <iostream>
#include <filesystem>
#include "region.hpp"

int main() {
    namespace fs = std::filesystem;

    // Crear directorio structure si no existe
    if (!fs::exists("structure")) {
        fs::create_directory("structure");
    }

    int chunkNum;
    std::cout << "Numero de chunk: ";
    if (!(std::cin >> chunkNum)) {
        std::cerr << "Entrada no valida" << std::endl;
        return 1;
    }
    std::string path = "structure/chunk" + std::to_string(chunkNum) + ".rgn";

    RegionCerebral region;
    region.nombre = "chunk" + std::to_string(chunkNum);

    int nNeur;
    std::cout << "Numero de neuronas: ";
    if (!(std::cin >> nNeur)) {
        std::cerr << "Entrada no valida" << std::endl;
        return 1;
    }

    for (int i = 0; i < nNeur; ++i) {
        Neurona n;
        std::cout << "-- Neurona " << (i+1) << " --" << std::endl;
        std::cout << "ID: ";
        std::cin >> n.id;
        std::cout << "Posicion x y z: ";
        std::cin >> n.x >> n.y >> n.z;
        std::string ntStr;
        std::cout << "Neurotransmisor: ";
        std::cin >> ntStr;
        n.nt = parseNT(ntStr);

        int numRec;
        std::cout << "Cantidad de receptores: ";
        std::cin >> numRec;
        for (int j = 0; j < numRec; ++j) {
            std::string recStr;
            std::cout << "  Receptor " << (j+1) << ": ";
            std::cin >> recStr;
            n.receptores.push_back(parseReceptor(recStr));
        }

        int numCon;
        std::cout << "Cantidad de conexiones: ";
        std::cin >> numCon;
        for (int j = 0; j < numCon; ++j) {
            Conexion c;
            std::cout << "  Destino ID " << (j+1) << ": ";
            std::cin >> c.destinoID;
            std::string ntc;
            std::cout << "  NT conexion " << (j+1) << ": ";
            std::cin >> ntc;
            c.tipoNT = parseNT(ntc);
            n.conexiones.push_back(c);
        }
        region.neuronas.push_back(n);
    }

    if (region.guardarEnArchivo(path)) {
        std::cout << "Chunk guardado en " << path << std::endl;
    } else {
        std::cerr << "No se pudo guardar el archivo" << std::endl;
        return 1;
    }

    return 0;
}
