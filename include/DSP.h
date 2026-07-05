#pragma once

#include <vector>
#include <complex>
#include <cmath>
#include <numeric>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// IQ Verisi tipi
using IQSample = std::complex<float>;

class DSP {
public:
    // Cooley-Tukey Radix-2 1D FFT (İleri Yönlü)
    // DİKKAT: data.size() mutlaka 2'nin kuvveti olmalıdır! (Örn: 1024, 2048)
    static void fft(std::vector<IQSample>& data) {
        const size_t N = data.size();
        if (N <= 1) return;

        // Çift ve tek indeksleri ayırma
        std::vector<IQSample> even(N / 2);
        std::vector<IQSample> odd(N / 2);
        for (size_t i = 0; i < N / 2; ++i) {
            even[i] = data[2 * i];
            odd[i] = data[2 * i + 1];
        }

        // Rekürsif çağrı
        fft(even);
        fft(odd);

        // Birleştirme (Kelebek İşlemi - Butterfly)
        for (size_t k = 0; k < N / 2; ++k) {
            // e^(-2 * pi * i * k / N)
            float angle = static_cast<float>(-2.0 * M_PI * k / N);
            IQSample t = std::polar(1.0f, angle) * odd[k];
            data[k] = even[k] + t;
            data[k + N / 2] = even[k] - t;
        }
    }

    // Basit FIR (Finite Impulse Response) Filtresi
    // Gelen IQ sinyalini verilen katsayılarla (taps) konvolüsyona sokar
    static std::vector<IQSample> applyFIR(const std::vector<IQSample>& input, const std::vector<float>& taps) {
        std::vector<IQSample> output(input.size());
        size_t numTaps = taps.size();
        
        for (size_t i = 0; i < input.size(); ++i) {
            IQSample sum(0.0f, 0.0f);
            for (size_t j = 0; j < numTaps; ++j) {
                if (i >= j) {
                    sum += input[i - j] * taps[j];
                }
            }
            output[i] = sum;
        }
        return output;
    }

    // FFT sonucundan Güç Spektrumu (Power Spectrum) hesaplar (dB cinsinden)
    static std::vector<float> calculatePowerSpectrum_dB(const std::vector<IQSample>& fftData) {
        std::vector<float> power_dB(fftData.size());
        for (size_t i = 0; i < fftData.size(); ++i) {
            // Büyüklüğün karesi üzerinden dB hesaplaması: 10 * log10(re^2 + im^2)
            float mag = std::norm(fftData[i]); // std::norm returns re^2 + im^2
            if (mag == 0) mag = 1e-12f; // Log(0) hatasını engellemek için
            power_dB[i] = 10.0f * std::log10(mag);
        }
        return power_dB;
    }

    // Sinyal tespiti için Gürültü Eşiğini (Noise Floor) hareketli ortalama ile hesaplar
    static float calculateNoiseFloor(const std::vector<float>& powerSpectrum, float percentile = 0.5f) {
        if (powerSpectrum.empty()) return 0.0f;
        
        std::vector<float> sortedSpectrum = powerSpectrum;
        std::sort(sortedSpectrum.begin(), sortedSpectrum.end());
        
        size_t index = static_cast<size_t>(sortedSpectrum.size() * percentile);
        return sortedSpectrum[index];
    }

    // ADS-B (AM) Sinyalleri için Genlik Hesaplayıcı
    static void calculateMagnitude(const std::vector<IQSample>& chunk, std::vector<float>& magnitudeOut) {
        for (size_t i = 0; i < chunk.size(); ++i) {
            float i_val = chunk[i].real();
            float q_val = chunk[i].imag();
            magnitudeOut[i] = std::sqrt(i_val * i_val + q_val * q_val);
        }
    }

    // AIS (GMSK/FM) Sinyalleri için Frekans Diskriminatörü (Phase Difference)
    static void fmDemodulate(const std::vector<unsigned char>& rawData, std::vector<float>& outFM) {
        outFM.resize(rawData.size() / 2);
        float prev_i = 0.0f;
        float prev_q = 0.0f;
        
        for (size_t k = 0; k < rawData.size(); k += 2) {
            float i_val = (static_cast<float>(rawData[k]) - 127.5f) / 128.0f;
            float q_val = (static_cast<float>(rawData[k+1]) - 127.5f) / 128.0f;
            
            // s[n] * conj(s[n-1]) = (I_n + j*Q_n) * (I_prev - j*Q_prev)
            // Reel Kısım: I_n*I_prev + Q_n*Q_prev
            // İmajiner Kısım: Q_n*I_prev - I_n*Q_prev
            float real = i_val * prev_i + q_val * prev_q;
            float imag = q_val * prev_i - i_val * prev_q;
            
            // Faz farkı (Frekans)
            float angle = std::atan2(imag, real);
            outFM[k / 2] = angle;
            
            prev_i = i_val;
            prev_q = q_val;
        }
    }
};
