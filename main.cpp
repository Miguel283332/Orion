#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <string>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
#include "region.hpp"

std::vector<RegionCerebral> regiones;
std::vector<Neurona*> todasNeuronas;
std::atomic<bool> sistema_activo(true);
std::mutex mutex_consola;
void hilo_red() {
    while (sistema_activo) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::lock_guard<std::mutex> lock(mutex_consola);
        std::cout << "[RED] Procesando señales neuronales...\n";
        // Aquí irá el motor de propagación futuro
    }
}
void hilo_terminal() {
    std::string comando;
    while (sistema_activo) {
        std::cout << "> ";
        std::getline(std::cin, comando);

        if (comando == "exit") {
            sistema_activo = false;
        } else if (comando == "stats") {
            std::lock_guard<std::mutex> lock(mutex_consola);
            std::cout << "[STATS]\n";
            std::cout << "Neurotransmisores activos:\n";
            for (const auto* n : todasNeuronas) {
                std::cout << " - Neurona " << n->id << ": " << toString(n->nt) << " estado=" << n->estado << "\n";
            }
        } else if (comando == "save") {
            for (const auto& r : regiones) {
                r.guardarEnArchivo("structure/" + r.nombre + ".rgn");
            }
            std::cout << "[SISTEMA] Regiones guardadas.\n";
        } else {
            std::cout << "[ERROR] Comando no reconocido.\n";
        }
    }
}
void hilo_microfono() {
    while (sistema_activo) {
        {
            std::lock_guard<std::mutex> lock(mutex_consola);
            std::cout << "[MICRÓFONO] Capturando audio...\n";
        }

        int resultado = system("arecord -f cd -t wav -d 2 -q audio.wav");

        if (resultado != 0) {
            std::lock_guard<std::mutex> lock(mutex_consola);
            std::cerr << "[MICRÓFONO] Error al capturar audio\n";
        } else {
            std::vector<char> buffer;
            std::ifstream f("audio.wav", std::ios::binary);
            buffer.assign(std::istreambuf_iterator<char>(f), {});
            size_t bits = buffer.size() * 8;
            size_t total = std::min(bits, todasNeuronas.size());
            for (size_t i = 0; i < total; ++i) {
                bool bit = (buffer[i/8] >> (i%8)) & 1;
                todasNeuronas[i]->estado = bit;
            }
            std::lock_guard<std::mutex> lock(mutex_consola);
            std::cout << "[MICRÓFONO] Audio procesado en neuronas\n";
        }

        std::this_thread::sleep_for(std::chrono::seconds(6));
    }
}
void hilo_altavoz() {
    while (sistema_activo) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        std::lock_guard<std::mutex> lock(mutex_consola);
        std::cout << "[ALTAVOZ] (Simulado) Emisión de respuesta auditiva...\n";
    }
}
void hilo_camara() {
    while (sistema_activo) {
        std::this_thread::sleep_for(std::chrono::seconds(7));
        cv::VideoCapture cam(0);
        if (!cam.isOpened()) {
            std::lock_guard<std::mutex> lock(mutex_consola);
            std::cerr << "No se pudo abrir la cámara\n";
            continue;
        }

        cv::Mat frame;
        cam >> frame;

        if (frame.empty()) {
            std::lock_guard<std::mutex> lock(mutex_consola);
            std::cerr << "Error al capturar imagen\n";
        } else {
            std::vector<uchar> buf;
            cv::imencode(".bmp", frame, buf);
            size_t bits = buf.size() * 8;
            size_t total = std::min(bits, todasNeuronas.size());
            for (size_t i = 0; i < total; ++i) {
                bool bit = (buf[i/8] >> (i%8)) & 1;
                todasNeuronas[i]->estado = bit;
            }
            std::lock_guard<std::mutex> lock(mutex_consola);
            std::cout << "[CÁMARA] Imagen procesada en neuronas\n";
        }
    }
}

int main() {
    namespace fs = std::filesystem;

    if (fs::exists("structure")) {
        for (const auto& e : fs::directory_iterator("structure")) {
            if (e.path().extension() == ".rgn") {
                RegionCerebral r;
                r.nombre = e.path().stem().string();
                if (r.cargarDesdeArchivo(e.path().string())) {
                    regiones.push_back(r);
                }
            }
        }
    }

    if (regiones.empty()) {
        RegionCerebral r;
        r.nombre = "corteza_visual";

        Neurona n1;
        n1.id = 1; n1.x = 0.1f; n1.y = 0.2f; n1.z = 0.3f;
        n1.nt = Neurotransmisor::DOPAMINA;
        n1.receptores = {Receptor::D1};
        n1.conexiones = { {2, Neurotransmisor::GABA} };

        Neurona n2;
        n2.id = 2; n2.x = 0.4f; n2.y = 0.5f; n2.z = 0.6f;
        n2.nt = Neurotransmisor::GABA;
        n2.receptores = {Receptor::GABA_A};
        n2.conexiones = { {1, Neurotransmisor::DOPAMINA} };

        r.neuronas = {n1, n2};
        regiones.push_back(r);

        if (!fs::exists("structure")) fs::create_directory("structure");
        r.guardarEnArchivo("structure/" + r.nombre + ".rgn");
    }

    for (auto& reg : regiones) {
        for (auto& n : reg.neuronas) {
            todasNeuronas.push_back(&n);
        }
    }

    // Lanza hilos
    std::thread t_terminal(hilo_terminal);
    std::thread t_red(hilo_red);
    std::thread t_microfono(hilo_microfono);
    std::thread t_camara(hilo_camara);
    std::thread t_altavoz(hilo_altavoz);

    // Esperar a que terminen (solo saldrán con "exit")
    t_terminal.join();
    t_red.join();
    t_microfono.join();
    t_camara.join();
    t_altavoz.join();

    std::cout << "[SISTEMA] Apagado.\n";
    return 0;
}

