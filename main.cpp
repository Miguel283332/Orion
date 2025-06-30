#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <string>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <cstdlib>
#include "region.hpp"
RegionCerebral rc;
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
            for (const auto& n : rc.neuronas) {
                std::cout << " - Neurona " << n.id << ": " << toString(n.nt) << "\n";
            }
        } else if (comando == "save") {
            rc.guardarEnArchivo("corteza_visual.rgn");
            std::cout << "[SISTEMA] Región guardada.\n";
        } else {
            std::cout << "[ERROR] Comando no reconocido.\n";
        }
    }
}
void hilo_microfono() {
    while (sistema_activo) {
        std::lock_guard<std::mutex> lock(mutex_consola);
        std::cout << "[MICRÓFONO] Capturando audio...\n";

        // Captura 2 segundos de audio y lo guarda como "audio.wav"
        int resultado = system("arecord -f cd -t wav -d 2 -q audio.wav");

        if (resultado != 0) {
            std::cerr << "[MICRÓFONO] Error al capturar audio\n";
        } else {
            std::cout << "[MICRÓFONO] Audio guardado en audio.wav\n";
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
        std::lock_guard<std::mutex> lock(mutex_consola);
        cv::VideoCapture cam(0); // 0 = cámara por defecto

        if (!cam.isOpened()) {
            std::cerr << "No se pudo abrir la cámara\n";
        }

        cv::Mat frame;
        cam >> frame; // Captura un solo frame

        if (frame.empty()) {
            std::cerr << "Error al capturar imagen\n";
        }

        // Guarda la imagen capturada
        cv::imwrite("captura.jpg", frame);

        std::cout << "Imagen guardada como captura.jpg\n";
        std::cout << "[CÁMARA] (Simulada) Capturando imagen del entorno...\n";
    }
}

int main() {
    rc.nombre = "corteza_visual";
    if (!rc.cargarDesdeArchivo("corteza_visual.rgn")) {
        std::cerr << "[⚠️] No se pudo cargar el archivo. Creando red de ejemplo.\n";

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

        rc.neuronas = {n1, n2};
        rc.guardarEnArchivo("corteza_visual.rgn");
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

