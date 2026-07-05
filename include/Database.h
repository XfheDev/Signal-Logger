#pragma once

#include <iostream>
#include <fstream>
#include <string>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <atomic>

class Database {
public:
    explicit Database(const std::string& path) : path_(path) {
        std::ifstream test(path_);
        if (!test.good()) {
            std::ofstream f(path_, std::ios::out);
            if (f.is_open())
                f << "Timestamp,Protocol,Identifier,Callsign,Squawk,Category,"
                     "Altitude,Speed,Heading,VertRate,"
                     "Latitude,Longitude,Frequency,SNR_dB,Emergency\n";
        }
        checkRotation();
    }

    // ── Full log with all decoded fields ──────────────────────────────────
    void logFull(const std::string& protocol,
                 const std::string& identifier,
                 const std::string& callsign,
                 const std::string& squawk,
                 const std::string& category,
                 int                altitude,
                 double             speed,
                 double             heading,
                 int                vertRate,
                 double             latitude,
                 double             longitude,
                 float              frequency,
                 float              snr_db     = 0.0f,
                 bool               emergency  = false,
                 const std::string& emergStr   = "")
    {
        std::lock_guard<std::mutex> lock(mu_);
        std::ofstream f(path_, std::ios::app);
        if (!f.is_open()) return;

        f << timestamp()       << ","
          << protocol          << ","
          << identifier        << ","
          << callsign          << ","
          << squawk            << ","
          << category          << ","
          << altitude          << ","
          << std::fixed << std::setprecision(1) << speed    << ","
          << std::fixed << std::setprecision(1) << heading  << ","
          << vertRate          << ","
          << std::fixed << std::setprecision(6) << latitude << ","
          << std::fixed << std::setprecision(6) << longitude << ","
          << frequency         << ","
          << std::fixed << std::setprecision(1) << snr_db   << ","
          << (emergency ? emergStr : "")
          << "\n";

        ++rowsSinceRotCheck_;
        if (rowsSinceRotCheck_ > 5000) {
            rowsSinceRotCheck_ = 0;
            checkRotation();
        }
    }

    // ── Legacy compatibility ───────────────────────────────────────────────
    void logPacket(const std::string& protocol, const std::string& id,
                   int alt, double lat, double lon, float freq) {
        logFull(protocol, id, "", "", "", alt, 0.0, 0.0, 0, lat, lon, freq);
    }

    uint64_t rowCount() const { return rowCount_.load(); }

private:
    std::string path_;
    std::mutex  mu_;
    std::atomic<uint64_t> rowCount_{0};
    int rowsSinceRotCheck_ = 0;

    static std::string timestamp() {
        auto now   = std::chrono::system_clock::now();
        auto now_t = std::chrono::system_clock::to_time_t(now);
        auto ms    = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now.time_since_epoch()) % 1000;
        std::tm tm{}; localtime_s(&tm, &now_t);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
        return std::string(buf) + "." + std::to_string(ms.count());
    }

    // ── Auto-rotate: rename to .bak when file exceeds 100 MB ─────────────
    void checkRotation() {
        std::ifstream f(path_, std::ios::binary | std::ios::ate);
        if (!f.is_open()) return;
        auto sz = f.tellg();
        f.close();
        if (sz > 100LL * 1024 * 1024) {      // > 100 MB
            std::string bak = path_ + ".bak";
            std::rename(path_.c_str(), bak.c_str());
            // Create fresh file with header
            std::ofstream nf(path_);
            if (nf.is_open())
                nf << "Timestamp,Protocol,Identifier,Callsign,Squawk,Category,"
                      "Altitude,Speed,Heading,VertRate,"
                      "Latitude,Longitude,Frequency,SNR_dB,Emergency\n";
        }
    }
};
