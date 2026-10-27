#ifdef _WIN32
#include "platform/audio_win32.hpp"
#include <cstring>

bool AudioOut::open(int inputRate) {
    close();
    // Frequences standard : jouees par Windows sans conversion
    for (int rate : {48000, 44100}) {
        WAVEFORMATEX fmt = {};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = DWORD(rate);
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = 4;
        fmt.nAvgBytesPerSec = DWORD(rate) * 4;
        if (waveOutOpen(&wo, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR) {
            outRate = rate;
            break;
        }
        wo = nullptr;
    }
    if (!wo) return false;
    inRate = inputRate;
    for (int i = 0; i < kBuffers; i++) {
        data[i].assign(kFrames * 2, 0);
        std::memset(&hdr[i], 0, sizeof hdr[i]);
        hdr[i].lpData = reinterpret_cast<LPSTR>(data[i].data());
        hdr[i].dwBufferLength = DWORD(kFrames * 4);
    }
    waveOutPause(wo);                                  // on ne joue qu'une fois quelques tampons prets
    playing = false;
    pending = 0;
    cur = 0;
    fill = 0;
    framesWritten = 0;
    dropped = underruns = 0;
    step = double(inRate) / double(outRate);
    frac = 0.0;
    prevL = prevR = 0;
    return true;
}

void AudioOut::close() {
    if (!wo) return;
    waveOutReset(wo);                                  // rend tous les tampons
    for (int i = 0; i < kBuffers; i++)
        if (hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, &hdr[i], sizeof hdr[i]);
    waveOutClose(wo);
    wo = nullptr;
}

int AudioOut::queued() const {
    int n = 0;
    for (int i = 0; i < kBuffers; i++) if (busy(i)) n++;
    return n;
}

int64_t AudioOut::queuedFrames() {
    MMTIME t = {};
    t.wType = TIME_SAMPLES;
    if (waveOutGetPosition(wo, &t, sizeof t) != MMSYSERR_NOERROR) return int64_t(queued()) * int64_t(kFrames);
    uint32_t played;
    if (t.wType == TIME_SAMPLES) played = t.u.sample;
    else if (t.wType == TIME_BYTES) played = t.u.cb / 4;
    else return int64_t(queued()) * int64_t(kFrames);
    // (le compteur de Windows tient sur 32 bits : on ne garde que l'ecart, toujours petit)
    return int64_t(int32_t(uint32_t(framesWritten) - played));
}

int AudioOut::latencyMs() {
    if (!wo || !outRate) return 0;
    const int64_t q = queuedFrames();
    return q < 0 ? 0 : int(q * 1000 / outRate);
}

// Envoie le tampon plein "cur" a Windows (ou le jette si la file est pleine)
void AudioOut::submit() {
    fill = 0;
    if (busy(cur)) { dropped++; return; }              // file pleine : ce tampon-ci est perdu, on reecrit dedans
    if (playing && queued() == 0) {                    // tout a ete joue : coupure, on se remet en avance
        waveOutPause(wo);
        playing = false;
        pending = 0;
        underruns++;
    }
    WAVEHDR& h = hdr[cur];
    if (h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, &h, sizeof h);
    h.dwFlags = 0;
    if (waveOutPrepareHeader(wo, &h, sizeof h) != MMSYSERR_NOERROR) return;
    if (waveOutWrite(wo, &h, sizeof h) != MMSYSERR_NOERROR) return;
    framesWritten += kFrames;
    cur = (cur + 1) % kBuffers;
    if (!playing && ++pending >= kPrefill) {           // assez d'avance : on joue
        waveOutRestart(wo);
        playing = true;
    }
}

void AudioOut::emit(int16_t l, int16_t r) {
    if (!busy(cur)) {                                  // (si le tampon est encore en lecture, il sera jete)
        data[cur][fill * 2] = l;
        data[cur][fill * 2 + 1] = r;
    }
    if (++fill == kFrames) submit();
}

void AudioOut::push(const int16_t* lr, size_t frames) {
    if (!wo || !frames) return;
    // Rythme de conversion : file trop pleine -> on produit un peu moins d'echantillons, trop vide -> un peu plus.
    // L'ecart est mesure en tampons par rapport au remplissage vise ; correction de 0,5 % au plus.
    if (playing) {
        double err = (double(queuedFrames()) - double(kTarget * kFrames)) / double(kTarget * kFrames);
        if (err > 1.0) err = 1.0;
        if (err < -1.0) err = -1.0;
        step = double(inRate) / double(outRate) * (1.0 + 0.005 * err);
    }
    // Interpolation lineaire : chaque echantillon d'entree produit environ outRate / inRate echantillons de sortie
    for (size_t i = 0; i < frames; i++) {
        const int32_t l = lr[2 * i], r = lr[2 * i + 1];
        while (frac < 1.0) {
            emit(int16_t(prevL + int32_t(double(l - prevL) * frac)), int16_t(prevR + int32_t(double(r - prevR) * frac)));
            frac += step;
        }
        frac -= 1.0;
        prevL = l;
        prevR = r;
    }
}
#endif
