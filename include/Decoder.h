#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include <map>
#include <cmath>
#include <chrono>
#include <algorithm>
#include <stdio.h>

// ─── ADS-B character set (ICAO Annex 10) ─────────────────────────────────────
static const char ADSB_CHARSET[] =
    "#ABCDEFGHIJKLMNOPQRSTUVWXYZ#####_###############0123456789######";

// ─── Aircraft emitter categories (TC=1-4, CA field) ──────────────────────────
// TC=4 most common, index 0-7
static const char* EMITTER_CAT[] = {
    "No Info", "Light", "Small", "Large", "Hi-Vortex",
    "Heavy",   "Hi-Perf", "Rotorcraft"
};

// ─── Emergency/priority status (bits 85-87 of TC19) ──────────────────────────
static const char* EMERG_STATUS[] = {
    "",            "General",  "Lifeguard", "Min Fuel",
    "No Comm",     "Unlawful", "Downed",    "Reserved"
};

// ─── ADS-B packet ─────────────────────────────────────────────────────────────
struct ADSB_Packet {
    std::string icao_address;
    std::string callsign;
    std::string squawk;           // "1200", "7700" etc.
    std::string category;         // "Heavy", "Rotorcraft" etc.
    std::string emergencyStr;     // "General", "Unlawful" etc.
    int         altitude     = 0; // feet MSL
    double      latitude     = 0.0;
    double      longitude    = 0.0;
    double      speed        = 0.0;   // ground / air speed, knots
    double      heading      = 0.0;   // degrees true 0-360
    int         verticalRate = 0;     // ft/min (+up / -down)
    float       snr_db       = 0.0f;  // estimated SNR dB
    std::string rawHex;               // "8D4B1A2C5D0B..."
    std::string rawBits;              // first 32 bits formatted
    uint8_t     df_val       = 0;
    uint8_t     tc_val       = 0;
    bool        isValid           = false;
    bool        hasPosition       = false;
    bool        hasVelocity       = false;
    bool        hasIdentification = false;
    bool        isSurface         = false; // on ground (TC 5-8)
    bool        hasEmergency      = false;
};

// ─── CPR state per aircraft ───────────────────────────────────────────────────
struct CPR_State {
    double   lat_cpr_even = 0, lon_cpr_even = 0;
    double   lat_cpr_odd  = 0, lon_cpr_odd  = 0;
    uint64_t time_even    = 0, time_odd     = 0;
    bool     has_even = false, has_odd = false;
};

// ─────────────────────────────────────────────────────────────────────────────
class Decoder {
public:
    inline static std::map<std::string, CPR_State> cprStates;
    inline static std::map<uint64_t, uint64_t>     recentMsgs; // hash→timestamp_ms

    // ── Main entry ─────────────────────────────────────────────────────────
    static std::vector<ADSB_Packet> decodeADSB_Chunk(
        const std::vector<float>& mag)
    {
        std::vector<ADSB_Packet> out;
        if (mag.size() < 240) return out;

        uint64_t now = nowMs();

        // Prune duplicate cache (keep 500 ms window)
        for (auto it = recentMsgs.begin(); it != recentMsgs.end(); ) {
            it = (now - it->second > 500) ? recentMsgs.erase(it) : std::next(it);
        }

        for (size_t i = 0; i + 240 < mag.size(); ++i) {

            // ── Preamble detection ──────────────────────────────────────
            float snrLin = preambleSNR(mag, i);
            if (snrLin < 0.6f) continue;

            // ── Bit extraction ──────────────────────────────────────────
            std::vector<uint8_t> bits = extractBits(mag, i + 16);

            uint8_t df = (uint8_t)extractInt(bits, 0, 5);

            // Only supported Downlink Formats
            bool longDF  = (df == 17 || df == 18 || df == 16 || df == 20 || df == 21);
            bool shortDF = (df == 11 || df == 4  || df == 5);
            if (!longDF && !shortDF) continue;

            int msgBits = longDF ? 112 : 56;

            // ── CRC24 validation ────────────────────────────────────────
            if (!verifyCRC24(bits, msgBits)) continue;

            // ── Duplicate rejection ─────────────────────────────────────
            uint64_t h = bitHash(bits, msgBits);
            if (recentMsgs.count(h)) continue;
            recentMsgs[h] = now;

            // ── Build packet ─────────────────────────────────────────────
            ADSB_Packet pkt;
            pkt.isValid  = true;
            pkt.df_val   = df;
            pkt.snr_db   = 20.0f * std::log10f(snrLin + 0.001f);

            // ICAO address position depends on DF
            if (df == 17 || df == 18) {
                pkt.icao_address = extractHex(bits, 8, 32);
                pkt.tc_val       = (uint8_t)extractInt(bits, 32, 37);
            } else if (df == 11) {
                pkt.icao_address = extractHex(bits, 8, 32);
            } else {
                // DF4/5/20/21 — ICAO not directly in message, skip for now
                continue;
            }

            if (pkt.icao_address == "000000" || pkt.icao_address == "FFFFFF")
                continue;

            // ── Raw hex string ────────────────────────────────────────────
            for (int b = 0; b < msgBits/8; b++) {
                char h2[3]; sprintf_s(h2, "%02X", extractInt(bits, b*8, b*8+8));
                pkt.rawHex += h2;
            }
            // ── Raw bits (first 32 bits = DF + ICAO header) ──────────────
            for (int b = 0; b < 32 && b < (int)bits.size(); b++) {
                pkt.rawBits += ('0' + bits[b]);
                if ((b+1)%8==0 && b<31) pkt.rawBits += ' ';
            }

            // ── DF17/18 Extended Squitter ─────────────────────────────────
            if (df == 17 || df == 18) {
                uint8_t tc = pkt.tc_val;

                // TC 1-4: Identification + Aircraft Category
                if (tc >= 1 && tc <= 4) {
                    pkt.hasIdentification = true;
                    uint8_t ca = (uint8_t)extractInt(bits, 37, 40);
                    if (tc == 4 && ca < 8) pkt.category = EMITTER_CAT[ca];

                    for (int c = 0; c < 8; c++) {
                        int idx = extractInt(bits, 40 + c*6, 46 + c*6);
                        pkt.callsign += (idx >= 0 && idx < 64) ? ADSB_CHARSET[idx] : ' ';
                    }
                    while (!pkt.callsign.empty() &&
                           (pkt.callsign.back()==' '||pkt.callsign.back()=='_'))
                        pkt.callsign.pop_back();
                }

                // TC 5-8: Surface Position
                if (tc >= 5 && tc <= 8) {
                    pkt.isSurface = true;
                    pkt.altitude  = 0;

                    // Movement → speed (ICAO 9684 Table N-4)
                    int mv = extractInt(bits, 37, 44);
                    if (mv >= 2 && mv <= 8)   pkt.speed = mv - 1.0;
                    else if (mv <= 12)          pkt.speed = 2.0  + (mv-9)  * 0.5;
                    else if (mv <= 38)          pkt.speed = 4.0  + (mv-13) * 2.0;
                    else if (mv <= 93)          pkt.speed = 58.0 + (mv-39);
                    else if (mv <= 108)         pkt.speed = 113.0+ (mv-94) * 5.0;
                    else if (mv > 0)            pkt.speed = 175.0;

                    if (bits[44]) pkt.heading = extractInt(bits,45,52) * 360.0/128.0;
                    pkt.hasVelocity = (pkt.speed > 0);

                    // CPR
                    cprUpdate(pkt, bits, bits[53], extractInt(bits,54,71), extractInt(bits,71,88), now, 25000);
                }

                // TC 9-18: Airborne Position
                if (tc >= 9 && tc <= 18) {
                    int altCode = extractInt(bits, 40, 52);
                    pkt.altitude = decodeAlt(altCode, (uint8_t)bits[48]);
                    cprUpdate(pkt, bits, bits[53], extractInt(bits,54,71), extractInt(bits,71,88), now, 10000);
                }

                // TC 19: Airborne Velocity
                if (tc == 19) {
                    pkt.hasVelocity = true;
                    uint8_t st = (uint8_t)extractInt(bits, 37, 40);
                    float   sc = (st == 2 || st == 4) ? 4.0f : 1.0f; // supersonic

                    if (st == 1 || st == 2) {
                        int vEW = extractInt(bits, 46, 56) - 1;
                        int vNS = extractInt(bits, 57, 67) - 1;
                        if (vEW >= 0 && vNS >= 0) {
                            if (bits[45]) vEW = -vEW;
                            if (bits[56]) vNS = -vNS;
                            pkt.speed   = std::sqrt((double)(vEW*vEW+vNS*vNS)) * sc;
                            pkt.heading = std::fmod(std::atan2((double)vEW,(double)vNS)*180.0/M_PI+360.0,360.0);
                        }
                    } else if (st == 3 || st == 4) {
                        if (bits[45]) pkt.heading = extractInt(bits,46,56)*360.0/1024.0;
                        int a = extractInt(bits,57,67)-1;
                        if (a >= 0) pkt.speed = a * sc;
                    }

                    // Vertical rate (bits 68-78)
                    int vrVal = extractInt(bits, 69, 78) - 1;
                    if (vrVal >= 0) {
                        pkt.verticalRate = vrVal * 64;
                        if (bits[68]) pkt.verticalRate = -pkt.verticalRate;
                    }

                    // Emergency/Priority status (bits 85-87)
                    int ec = extractInt(bits, 85, 88);
                    if (ec > 0 && ec < 8) {
                        pkt.hasEmergency = true;
                        pkt.emergencyStr = EMERG_STATUS[ec];
                    }
                }

                // TC 28: Aircraft status (emergency squawk)
                if (tc == 28) {
                    int msgSub = extractInt(bits, 37, 40);
                    if (msgSub == 1) {
                        int ec = extractInt(bits, 40, 43);
                        if (ec > 0 && ec < 8) {
                            pkt.hasEmergency = true;
                            pkt.emergencyStr = EMERG_STATUS[ec];
                        }
                        // Squawk from bits 43-54 (Mode A)
                        pkt.squawk = decodeSquawk(extractInt(bits, 43, 57));
                    }
                }
            }

            // DF11: All-Call Reply — aircraft detected, no position yet
            // Already extracted ICAO above, just push as a "seen" packet

            out.push_back(pkt);

            // Skip past decoded frame
            i += (longDF ? 239 : 111);
        }
        return out;
    }

private:
    // ── Timestamp ─────────────────────────────────────────────────────────
    static uint64_t nowMs() {
        return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // ── Preamble SNR (linear ratio hi:lo) ─────────────────────────────────
    static float preambleSNR(const std::vector<float>& d, size_t s) {
        if (s + 16 >= d.size()) return 0.0f;
        float hi = d[s+0]+d[s+2]+d[s+7]+d[s+9];
        float lo = d[s+1]+d[s+3]+d[s+4]+d[s+5]+d[s+6]+
                   d[s+8]+d[s+10]+d[s+11]+d[s+12]+d[s+13]+d[s+14]+d[s+15];
        if (hi < 0.1f) return 0.0f;
        if (!(d[s+0]>d[s+1] && d[s+2]>d[s+3] && d[s+7]>d[s+6] && d[s+9]>d[s+10]))
            return 0.0f;
        return hi / (lo/12.0f + 0.001f);
    }

    // ── Bit extraction ─────────────────────────────────────────────────────
    static std::vector<uint8_t> extractBits(const std::vector<float>& d, size_t start) {
        std::vector<uint8_t> bits(112, 0);
        size_t avail = (d.size() > start) ? (d.size()-start)/2 : 0;
        int n = (int)(std::min)((size_t)112, avail);
        for (int i = 0; i < n; ++i)
            bits[i] = (d[start+i*2] > d[start+i*2+1]) ? 1 : 0;
        return bits;
    }

    // ── CRC24 (Mode S generator 0xFFF409) ─────────────────────────────────
    // Process full message; valid if remainder == 0
    static bool verifyCRC24(const std::vector<uint8_t>& bits, int msgBits) {
        int nb = msgBits / 8;
        if ((int)bits.size() < nb) return false;
        // Convert bits → bytes
        uint8_t bytes[14] = {};
        for (int i = 0; i < nb*8 && i < (int)bits.size(); i++)
            bytes[i/8] |= (uint8_t)(bits[i] << (7 - i%8));
        uint32_t crc = 0;
        for (int i = 0; i < nb; i++) {
            crc ^= (uint32_t)bytes[i] << 16;
            for (int j = 0; j < 8; j++) {
                crc <<= 1;
                if (crc & 0x1000000u) crc ^= 0xFFF409u;
            }
        }
        return (crc & 0xFFFFFFu) == 0;
    }

    // ── FNV-1a hash for duplicate detection ───────────────────────────────
    static uint64_t bitHash(const std::vector<uint8_t>& bits, int msgBits) {
        uint64_t h = 14695981039346656037ULL;
        int nb = msgBits / 8;
        for (int i = 0; i < nb && i < (int)bits.size(); i++) {
            uint8_t b = 0;
            for (int j = 0; j < 8 && i*8+j < (int)bits.size(); j++)
                b |= (uint8_t)(bits[i*8+j] << (7-j));
            h ^= b; h *= 1099511628211ULL;
        }
        return h;
    }

    // ── CPR update + decode ───────────────────────────────────────────────
    static void cprUpdate(ADSB_Packet& pkt, const std::vector<uint8_t>& bits,
                          uint8_t fmt, int cpr_lat, int cpr_lon,
                          uint64_t now, uint64_t maxAge_ms)
    {
        auto& st = cprStates[pkt.icao_address];
        if (fmt == 0) {
            st.lat_cpr_even = cpr_lat/131072.0;
            st.lon_cpr_even = cpr_lon/131072.0;
            st.time_even = now; st.has_even = true;
        } else {
            st.lat_cpr_odd  = cpr_lat/131072.0;
            st.lon_cpr_odd  = cpr_lon/131072.0;
            st.time_odd = now; st.has_odd = true;
        }
        if (st.has_even && st.has_odd) {
            int64_t dt = (int64_t)st.time_even - (int64_t)st.time_odd;
            if (std::abs(dt) < (int64_t)maxAge_ms) {
                double lat, lon;
                decodeCPR(st, fmt, lat, lon);
                if (lat>=-90&&lat<=90&&lon>=-180&&lon<=180&&!(lat==0&&lon==0)) {
                    pkt.latitude  = lat;
                    pkt.longitude = lon;
                    pkt.hasPosition = true;
                }
            }
        }
    }

    // ── Altitude decode (Q-bit or Gillham) ────────────────────────────────
    static int decodeAlt(int code, uint8_t qBit) {
        if (qBit == 1) {
            // 25-ft resolution, Q-bit removed
            int n = ((code & 0xFE0) >> 1) | (code & 0x0F);
            return n * 25 - 1000;
        }
        // Gillham (Mode C) — gray-decode interleaved 500-ft + 100-ft
        // Extract C/D bits (500-ft increments) and A/B bits (100-ft)
        int C1=(code>>11)&1, A1=(code>>10)&1, C2=(code>>9)&1;
        int B2=(code>>8)&1,  D2=(code>>7)&1,  A2=(code>>6)&1;
        int B4=(code>>5)&1,  C4=(code>>4)&1,  A4=(code>>2)&1;
        int B1=(code>>1)&1,  D4= code    &1;

        // Gray decode 500-ft Gray coded field (D1=0 always in Mode C)
        auto gray3 = [](int g)->int{
            int b=0; for(;g;g>>=1) b^=g; return b;
        };
        int gA = gray3((A1<<2)|(A2<<1)|A4);
        int gB = gray3((B1<<2)|(B2<<1)|B4);
        int gC = gray3((C1<<2)|(C2<<1)|C4);
        int gD = gray3(          (D2<<1)|D4);

        // 500-ft value
        int n500 = (gA-1)*3 + (gB ? (gB<=4 ? gB-1 : 8-gB) : 0);
        // 100-ft discriminant from C/D
        int n100 = gC - (gD?1:0);
        if (n100 <= 0 || n100 > 5) n100 = 1;

        return (n500 * 500) + (n100 * 100) - 1300;
    }

    // ── Squawk (Mode A) decode ────────────────────────────────────────────
    // Input: 13-bit Mode A code (C1 A1 B1 D1 C2 A2 B2 D2 C4 A4 B4 0 D4)
    static std::string decodeSquawk(int code) {
        // Extract A,B,C,D pulse groups
        int A = ((code>>11)&1)*1 + ((code>>8)&1)*2 + ((code>>5)&1)*4;
        int B = ((code>>10)&1)*1 + ((code>>7)&1)*2 + ((code>>4)&1)*4;
        int C = ((code>>12)&1)*1 + ((code>>9)&1)*2 + ((code>>6)&1)*4;
        int D = ((code>>2)&1)*1  + 0               + ((code>>0)&1)*4
               +((code>>3)&1)*2; // simplified D1D2D4
        char buf[8];
        sprintf_s(buf, "%d%d%d%d", A, B, C, D);
        return std::string(buf);
    }

    // ── CPR decode ────────────────────────────────────────────────────────
    static void decodeCPR(const CPR_State& st, uint8_t newest,
                          double& lat, double& lon) {
        double dLE=360.0/60.0, dLO=360.0/59.0;
        int j=(int)std::floor(59.0*st.lat_cpr_even-60.0*st.lat_cpr_odd+0.5);
        double rLE=dLE*(cprMod(j,60)+st.lat_cpr_even);
        double rLO=dLO*(cprMod(j,59)+st.lat_cpr_odd);
        if (rLE>=270) rLE-=360.0;
        if (rLO>=270) rLO-=360.0;
        lat=(newest==0)?rLE:rLO;
        int nl=cprNL(lat);
        int ni=(newest==0)?(nl>1?nl:1):((nl-1)>1?(nl-1):1);
        double dLon=360.0/ni;
        int m=(int)std::floor(st.lon_cpr_even*(nl-1)-st.lon_cpr_odd*nl+0.5);
        double lonT=dLon*(cprMod(m,ni)+(newest==0?st.lon_cpr_even:st.lon_cpr_odd));
        if (lonT>=180.0) lonT-=360.0;
        lon=lonT;
    }

    static int cprMod(int a,int b){int r=a%b;return r<0?r+b:r;}
    static int cprNL(double lat){
        if(std::abs(lat)>=87.0) return 1;
        double a=1.0-std::cos(M_PI/30.0);
        double b=std::pow(std::cos(M_PI/180.0*lat),2.0);
        if(a>b) return 1;
        return (int)std::floor(2.0*M_PI/std::acos(1.0-a/b));
    }

    static int extractInt(const std::vector<uint8_t>& b,int s,int e){
        int v=0; for(int i=s;i<e&&i<(int)b.size();++i) v=(v<<1)|b[i]; return v;
    }
    static std::string extractHex(const std::vector<uint8_t>& b,int s,int e){
        const char* h="0123456789ABCDEF"; std::string r;
        for(int i=s;i<e;i+=4) r+=h[extractInt(b,i,i+4)]; return r;
    }
};
