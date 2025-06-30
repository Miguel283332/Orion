#include "region.hpp"
#include <fstream>
#include <sstream>

bool RegionCerebral::cargarDesdeArchivo(const std::string& archivo) {
    std::ifstream in(archivo);
    if (!in.is_open()) return false;

    neuronas.clear();
    std::string linea;
    Neurona actual;
    while (std::getline(in, linea)) {
        if (linea.empty() || linea[0] == '#') continue;

        if (linea == "[Neurona]") {
            if (actual.id != 0) neuronas.push_back(actual);
            actual = Neurona();
        } else if (linea.find("id=") == 0) {
            actual.id = std::stoi(linea.substr(3));
        } else if (linea.find("pos=") == 0) {
            sscanf(linea.c_str(), "pos=%f,%f,%f", &actual.x, &actual.y, &actual.z);
        } else if (linea.find("nt=") == 0) {
            actual.nt = parseNT(linea.substr(3));
        } else if (linea.find("receptores=") == 0) {
            actual.receptores.clear();
            std::stringstream ss(linea.substr(11));
            std::string item;
            while (std::getline(ss, item, ',')) {
                actual.receptores.push_back(parseReceptor(item));
            }
        } else if (linea.find("conexiones=") == 0) {
            actual.conexiones.clear();
            std::stringstream ss(linea.substr(11));
            std::string item;
            while (std::getline(ss, item, ',')) {
                int id;
                char tipo[20];
                sscanf(item.c_str(), "%d:%s", &id, tipo);
                actual.conexiones.push_back({id, parseNT(tipo)});
            }
        }
    }
    if (actual.id != 0) neuronas.push_back(actual);
    return true;
}

bool RegionCerebral::guardarEnArchivo(const std::string& archivo) const {
    std::ofstream out(archivo);
    if (!out.is_open()) return false;

    for (const auto& n : neuronas) {
        out << "[Neurona]\n";
        out << "id=" << n.id << "\n";
        out << "pos=" << n.x << "," << n.y << "," << n.z << "\n";
        out << "nt=" << toString(n.nt) << "\n";

        out << "receptores=";
        for (size_t i = 0; i < n.receptores.size(); ++i) {
            out << toString(n.receptores[i]);
            if (i != n.receptores.size() - 1) out << ",";
        }
        out << "\n";

        out << "conexiones=";
        for (size_t i = 0; i < n.conexiones.size(); ++i) {
            out << n.conexiones[i].destinoID << ":" << toString(n.conexiones[i].tipoNT);
            if (i != n.conexiones.size() - 1) out << ",";
        }
        out << "\n\n";
    }

    return true;
}

