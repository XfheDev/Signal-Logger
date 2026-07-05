#pragma once

#include <vector>
#include <cstdint>
#include <iostream>
#include <string>
#include <cmath>
#include "Decoder.h"

class AISDecoder {
public:
    static std::vector<ADSB_Packet> decodeAIS_Chunk(const std::vector<float>& fmData, float sampleRate) {
        std::vector<ADSB_Packet> detectedShips;
        float samplesPerBit = sampleRate / 9600.0f;
        
        // 1. Clock Recovery & Bit Extraction (Zero-Crossing tabanlı basit yöntem)
        std::vector<uint8_t> rawBits;
        int i = 0;
        
        while (i < fmData.size() - 1) {
            // Sinyal sıfırı geçiyorsa senkronize ol
            if ((fmData[i] > 0 && fmData[i+1] < 0) || (fmData[i] < 0 && fmData[i+1] > 0)) {
                // Bit ortasına atla
                float bitCenter = i + (samplesPerBit / 2.0f);
                
                // Ardışık bitleri oku (Bir sonraki zero-crossing'e veya sinyal bitene kadar)
                while (bitCenter < fmData.size()) {
                    int idx = static_cast<int>(bitCenter);
                    rawBits.push_back(fmData[idx] > 0 ? 1 : 0);
                    bitCenter += samplesPerBit;
                    
                    // Sinyalin yönü değiştiyse yeniden senkronize olmak için kır
                    if (idx + 1 < fmData.size() && ((fmData[idx] > 0 && fmData[idx+1] < 0) || (fmData[idx] < 0 && fmData[idx+1] > 0))) {
                        i = idx;
                        break;
                    }
                }
            } else {
                i++;
            }
        }
        
        // 2. NRZI Decode (Değişim = 0, Sabit = 1)
        std::vector<uint8_t> nrziBits;
        if (rawBits.empty()) return detectedShips;
        
        uint8_t prevBit = rawBits[0];
        for (size_t k = 1; k < rawBits.size(); ++k) {
            nrziBits.push_back(rawBits[k] == prevBit ? 1 : 0);
            prevBit = rawBits[k];
        }
        
        // 3. HDLC Framing (0x7E bulma) ve Bit Stuffing Removal
        std::vector<uint8_t> payload;
        int consecutiveOnes = 0;
        bool inPacket = false;
        
        for (size_t k = 0; k < nrziBits.size(); ++k) {
            uint8_t b = nrziBits[k];
            
            if (b == 1) {
                consecutiveOnes++;
            } else {
                if (consecutiveOnes == 6) {
                    // 01111110 (0x7E Flag) -> Paket Başlangıcı veya Bitişi
                    if (inPacket && payload.size() > 168) {
                        // Paket bitti, çöz!
                        ADSB_Packet ship = decodePayload(payload);
                        if (ship.isValid) detectedShips.push_back(ship);
                    }
                    payload.clear();
                    inPacket = true;
                } else if (consecutiveOnes == 5 && inPacket) {
                    // Bit stuffing (5 tane 1'den sonraki 0 atılır)
                    // b'yi payload'a Eklemiyoruz.
                } else if (inPacket) {
                    payload.push_back(0);
                }
                consecutiveOnes = 0;
                continue;
            }
            
            if (inPacket && consecutiveOnes != 6) {
                payload.push_back(1);
            }
        }
        
        return detectedShips;
    }

private:
    static ADSB_Packet decodePayload(const std::vector<uint8_t>& payload) {
        ADSB_Packet ship;
        ship.isValid = false;
        ship.hasPosition = false;
        
        if (payload.size() < 168) return ship; // Minimum AIS Type 1 Message length
        
        // Message ID (6 bits)
        int msgId = extractInt(payload, 0, 6);
        if (msgId != 1 && msgId != 2 && msgId != 3) return ship; // Sadece Position Report'lar
        
        // MMSI (30 bits)
        long long mmsi = 0;
        for (int i = 8; i < 38; ++i) mmsi = (mmsi << 1) | payload[i];
        
        // Longitude (28 bits) -> 1/10000 of a minute
        long long lonRaw = 0;
        for (int i = 61; i < 89; ++i) lonRaw = (lonRaw << 1) | payload[i];
        // Sign extension (28-bit two's complement)
        if (lonRaw & 0x08000000) lonRaw |= 0xFFFFFFFFF0000000LL;
        
        // Latitude (27 bits) -> 1/10000 of a minute
        long long latRaw = 0;
        for (int i = 89; i < 116; ++i) latRaw = (latRaw << 1) | payload[i];
        // Sign extension (27-bit two's complement)
        if (latRaw & 0x04000000) latRaw |= 0xFFFFFFFFF8000000LL;
        
        double lon = lonRaw / 600000.0;
        double lat = latRaw / 600000.0;
        
        if (lat <= 90.0 && lat >= -90.0 && lon <= 180.0 && lon >= -180.0) {
            ship.icao_address = std::to_string(mmsi);
            ship.latitude = lat;
            ship.longitude = lon;
            ship.altitude = 0; // Gemilerin irtifası 0'dır (Deniz seviyesi)
            ship.isValid = true;
            ship.hasPosition = true;
        }
        
        return ship;
    }

    static int extractInt(const std::vector<uint8_t>& bits, int start, int end) {
        int val = 0;
        for (int i = start; i < end; ++i) val = (val << 1) | bits[i];
        return val;
    }
};
