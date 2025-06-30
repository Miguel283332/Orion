#include "neurona.hpp"
#include <algorithm>

// --- Neurotransmisores ---
Neurotransmisor parseNT(const std::string& s) {
    if (s == "DOPAMINA") return Neurotransmisor::DOPAMINA;
    if (s == "SEROTONINA") return Neurotransmisor::SEROTONINA;
    if (s == "GABA") return Neurotransmisor::GABA;
    if (s == "GLUTAMATO") return Neurotransmisor::GLUTAMATO;
    if (s == "ACETILCOLINA") return Neurotransmisor::ACETILCOLINA;
    return Neurotransmisor::DESCONOCIDO;
}

std::string toString(Neurotransmisor nt) {
    switch (nt) {
        case Neurotransmisor::DOPAMINA: return "DOPAMINA";
        case Neurotransmisor::SEROTONINA: return "SEROTONINA";
        case Neurotransmisor::GABA: return "GABA";
        case Neurotransmisor::GLUTAMATO: return "GLUTAMATO";
        case Neurotransmisor::ACETILCOLINA: return "ACETILCOLINA";
        default: return "DESCONOCIDO";
    }
}

// --- Receptores ---
Receptor parseReceptor(const std::string& s) {
    if (s == "D1") return Receptor::D1;
    if (s == "D2") return Receptor::D2;
    if (s == "NMDA") return Receptor::NMDA;
    if (s == "AMPA") return Receptor::AMPA;
    if (s == "GABA_A") return Receptor::GABA_A;
    return Receptor::DESCONOCIDO;
}

std::string toString(Receptor r) {
    switch (r) {
        case Receptor::D1: return "D1";
        case Receptor::D2: return "D2";
        case Receptor::NMDA: return "NMDA";
        case Receptor::AMPA: return "AMPA";
        case Receptor::GABA_A: return "GABA_A";
        default: return "DESCONOCIDO";
    }
}

// --- Forma divergente ---
std::string Neurona::formaDivergente() const {
    if (nt == Neurotransmisor::GABA && receptores.size() == 1 && receptores[0] == Receptor::GABA_A) {
        return "INTERNEURONA_INHIBIDORA";
    }
    if (nt == Neurotransmisor::DOPAMINA && (std::find(receptores.begin(), receptores.end(), Receptor::D1) != receptores.end())) {
        return "MODULADORA_D1";
    }
    return "INDEFINIDA";
}

