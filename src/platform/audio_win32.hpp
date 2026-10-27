#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>

// Sortie audio Windows (waveOut, inclus dans Windows : rien a installer).
//
// La carte son est ouverte a une frequence standard (48 000 Hz, sinon 44 100 Hz) : Windows la joue telle quelle.
// Le son de la console (32 728 Hz) est converti ici, par interpolation lineaire.
//
// Le son est range dans une file de tampons confies a Windows au fur et a mesure. Pour que cette file ne se
// vide pas (coupures) ni ne deborde (pertes), le rythme de conversion est ajuste en continu, de 0,5 % au plus
// (inaudible), selon son remplissage : c'est ce qui cale l'emulateur et la carte son l'un sur l'autre.
// Au demarrage, et apres une coupure, on attend quelques tampons d'avance avant de jouer.
class AudioOut {
public:
    ~AudioOut() { close(); }
    bool open(int inputRate);                          // inputRate : frequence du son fourni (celle de la console)
    void close();
    void push(const int16_t* lr, size_t frames);       // echantillons stereo (gauche, droite, gauche...)
    bool isOpen() const { return wo != nullptr; }
    int rate() const { return outRate; }               // frequence de la carte son

    // Mesures (affichees dans le titre) : retard reel entre l'envoi a Windows et la sortie du son, en ms ;
    // tampons jetes (file pleine) et coupures (file vide) depuis l'ouverture.
    int latencyMs();
    uint32_t dropped = 0, underruns = 0;

private:
    static constexpr int kBuffers = 10;                // file de 10 tampons
    static constexpr size_t kFrames = 512;             // de 512 echantillons stereo (environ 11 ms a 48 kHz)
    static constexpr int kPrefill = 4;                 // tampons d'avance avant de (re)commencer a jouer
    static constexpr int kTarget = 5;                  // remplissage vise (en tampons), environ 53 ms

    HWAVEOUT wo = nullptr;
    WAVEHDR hdr[kBuffers] = {};
    std::vector<int16_t> data[kBuffers];
    int cur = 0;                                       // tampon en cours de remplissage
    size_t fill = 0;                                   // echantillons stereo deja dans ce tampon
    bool playing = false;
    int pending = 0;                                   // tampons envoyes depuis la derniere (re)mise en route
    int inRate = 0, outRate = 0;
    uint64_t framesWritten = 0;                        // echantillons stereo confies a Windows depuis l'ouverture

    // conversion de frequence
    double step = 1.0;                                 // avance dans le son d'entree par echantillon de sortie
    double frac = 0.0;                                 // position entre l'echantillon precedent et le suivant
    int32_t prevL = 0, prevR = 0;

    bool busy(int i) const { return (hdr[i].dwFlags & WHDR_PREPARED) && !(hdr[i].dwFlags & WHDR_DONE); }
    int queued() const;
    int64_t queuedFrames();                            // echantillons envoyes mais pas encore joues
    void emit(int16_t l, int16_t r);                   // un echantillon converti vers le tampon courant
    void submit();
};
#endif
