#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>

#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <queue>
#include <mutex>
#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>
#include "DSP.h"
#include "Decoder.h"
#include "AISDecoder.h"
#include "Database.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")


// RTL-SDR Types
typedef struct rtlsdr_dev rtlsdr_dev_t;
typedef void(*rtlsdr_read_async_cb_t)(unsigned char *buf, uint32_t len, void *ctx);

// Function Pointers for RTL-SDR
typedef uint32_t (*rtlsdr_get_device_count_ptr)(void);
typedef const char* (*rtlsdr_get_device_name_ptr)(uint32_t index);
typedef int (*rtlsdr_open_ptr)(rtlsdr_dev_t **dev, uint32_t index);
typedef int (*rtlsdr_close_ptr)(rtlsdr_dev_t *dev);
typedef int (*rtlsdr_set_center_freq_ptr)(rtlsdr_dev_t *dev, uint32_t freq);
typedef int (*rtlsdr_set_sample_rate_ptr)(rtlsdr_dev_t *dev, uint32_t rate);
typedef int (*rtlsdr_set_tuner_gain_mode_ptr)(rtlsdr_dev_t *dev, int manual);
typedef int (*rtlsdr_set_tuner_gain_ptr)(rtlsdr_dev_t *dev, int gain);
typedef int (*rtlsdr_set_agc_mode_ptr)(rtlsdr_dev_t *dev, int on);
typedef int (*rtlsdr_reset_buffer_ptr)(rtlsdr_dev_t *dev);
typedef int (*rtlsdr_read_async_ptr)(rtlsdr_dev_t *dev, rtlsdr_read_async_cb_t cb, void *ctx, uint32_t buf_num, uint32_t buf_len);
typedef int (*rtlsdr_cancel_async_ptr)(rtlsdr_dev_t *dev);

// State
std::queue<std::vector<unsigned char>> packetQueue;
std::mutex queueMutex;
bool isRunning = true;
rtlsdr_dev_t* sdrDev = nullptr;
rtlsdr_cancel_async_ptr cancel_async_fn = nullptr;
Database dataLake("data_lake.csv");

// Config
int sourceType = 1;     // 1: Local USB, 2: Network TCP
int localMode = 1;      // 1: ADS-B, 2: AIS
int networkFormat = 1;  // 1: RTL-TCP (Raw), 2: BaseStation (CSV)
std::string netIP = "127.0.0.1";
int netPort = 30003;

// ============================================================
// AIRCRAFT STATE TABLE (In-memory DB)
// ============================================================
struct AircraftState {
    std::string icao;
    std::string callsign;
    std::string squawk;           // "1200", "7700" etc.
    std::string category;         // "Light", "Heavy" etc.
    std::string emergency;        // emergency string or ""
    int         altitude    = 0;
    double      lat         = 0.0;
    double      lon         = 0.0;
    double      speed       = 0.0;
    double      heading     = 0.0;
    int         vertRate    = 0;
    float       snr_db      = 0.0f; // latest message SNR
    int         msgCount    = 0;
    int         msgRate     = 0;    // msgs / last 10 sec
    bool        isSurface   = false;
    bool        hasEmergency= false;
    ULONGLONG   firstSeen   = 0;    // GetTickCount64() at first message
    ULONGLONG   lastSeen    = 0;
    std::string lastRaw;
    // For rolling msg-rate
    std::vector<ULONGLONG> recentMsgTimes;
};

std::map<std::string, AircraftState> g_aircraft;
std::mutex g_aircraftMutex;

// Raw data log queue (from worker threads -> GUI thread)
struct RawEntry {
    std::string line;  // formatted log line
};
std::queue<RawEntry> g_rawQueue;
std::mutex g_rawMutex;

// GUI Handles (global so threads can PostMessage)
HWND g_hMainWnd = nullptr;
HWND g_hListView = nullptr;
HWND g_hRawLog  = nullptr;

std::string getTimestamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    sprintf_s(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

void pushRawEntry(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_rawMutex);
    RawEntry e; e.line = line;
    g_rawQueue.push(e);
    if (g_rawQueue.size() > 2000) g_rawQueue.pop(); // cap
}

// Hızlı string split fonksiyonu
std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

// ---------------------------------------------------------
// LOCAL USB (RTL-SDR) CALLBACK
// ---------------------------------------------------------
void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx) {
    if (!isRunning) return;
    std::vector<unsigned char> data(buf, buf + len);
    std::lock_guard<std::mutex> lock(queueMutex);
    packetQueue.push(std::move(data));
}

// ---------------------------------------------------------
// NETWORK TCP THREAD
// ---------------------------------------------------------
void networkIngestorThread() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "[Network] WSAStartup failed." << std::endl;
        return;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        std::cerr << "[Network] Error creating socket." << std::endl;
        WSACleanup();
        return;
    }

    sockaddr_in serverAddress;
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons((u_short)netPort);
    serverAddress.sin_addr.s_addr = inet_addr(netIP.c_str());

    std::cout << "[Network] Connecting to " << netIP << ":" << netPort << "..." << std::endl;

    if (connect(sock, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) < 0) {
        std::cerr << "[Network] Connection Failed!" << std::endl;
        closesocket(sock);
        WSACleanup();
        isRunning = false;
        return;
    }
    
    std::cout << "[Network] Connected successfully!" << std::endl;

    if (networkFormat == 1) { // RTL-TCP RAW IQ
        // İlk 12 byte RTL-TCP Header bilgisini yut
        char header[12];
        recv(sock, header, 12, 0);
        
        // Frekansı ve Sample Rate'i ayarlamak için komut gönderebiliriz (Opsiyonel)
        // Şimdilik sadece Raw veri alıyoruz
        const int bufSize = 262144;
        std::vector<char> buffer(bufSize);
        
        while (isRunning) {
            int bytesReceived = recv(sock, buffer.data(), bufSize, 0);
            if (bytesReceived > 0) {
                std::vector<unsigned char> data(buffer.begin(), buffer.begin() + bytesReceived);
                std::lock_guard<std::mutex> lock(queueMutex);
                packetQueue.push(std::move(data));
            } else {
                std::cout << "[Network] Stream disconnected." << std::endl;
                break;
            }
        }
    } 
    else if (networkFormat == 2) { // BASESTATION (Port 30003) CSV
        char buf[4096];
        std::string leftover = "";
        
        while (isRunning) {
            int bytes = recv(sock, buf, sizeof(buf), 0);
            if (bytes <= 0) break;
            
            std::string chunk(buf, bytes);
            std::string data = leftover + chunk;
            
            size_t pos = 0;
            while ((pos = data.find('\n')) != std::string::npos) {
                std::string line = data.substr(0, pos);
                if (!line.empty() && line.back() == '\r') line.pop_back(); // \r temizle
                data.erase(0, pos + 1);
                
                // Parse BaseStation
                std::vector<std::string> parts = split(line, ',');
                if (parts.size() >= 22 && parts[0] == "MSG") {
                    std::string icao = parts[4];
                    int alt = 0;
                    double lat = 0.0, lon = 0.0;
                    
                    try { if (!parts[11].empty()) alt = std::stoi(parts[11]); } catch(...) {}
                    try { if (!parts[14].empty()) lat = std::stod(parts[14]); } catch(...) {}
                    try { if (!parts[15].empty()) lon = std::stod(parts[15]); } catch(...) {}
                    
                    if (lat != 0.0 && lon != 0.0) {
                        std::cout << "[Network] 📡 BASESTATION POS: " << icao << " | Lat: " << lat << " | Lon: " << lon << std::endl;
                        dataLake.logPacket("ADS-B", icao, alt, lat, lon, 1090.0f);
                    }
                }
            }
            leftover = data;
        }
    }

    closesocket(sock);
    WSACleanup();
}

// ---------------------------------------------------------
// LOCAL USB INGESTOR THREAD
// ---------------------------------------------------------
void localIngestorCore() {
    std::cout << "[Ingestor] Loading librtlsdr.dll..." << std::endl;
    HMODULE hRtlsdr = LoadLibraryA("third_party/librtlsdr/librtlsdr.dll");
    if (!hRtlsdr) {
        std::cerr << "[Ingestor] ERROR: Could not load librtlsdr.dll! Running in network-only?" << std::endl;
        return;
    }
    
    auto get_dev_count = (rtlsdr_get_device_count_ptr)GetProcAddress(hRtlsdr, "rtlsdr_get_device_count");
    auto get_dev_name = (rtlsdr_get_device_name_ptr)GetProcAddress(hRtlsdr, "rtlsdr_get_device_name");
    auto rtlsdr_open = (rtlsdr_open_ptr)GetProcAddress(hRtlsdr, "rtlsdr_open");
    auto rtlsdr_close = (rtlsdr_close_ptr)GetProcAddress(hRtlsdr, "rtlsdr_close");
    auto rtlsdr_set_freq = (rtlsdr_set_center_freq_ptr)GetProcAddress(hRtlsdr, "rtlsdr_set_center_freq");
    auto rtlsdr_set_rate = (rtlsdr_set_sample_rate_ptr)GetProcAddress(hRtlsdr, "rtlsdr_set_sample_rate");
    auto rtlsdr_set_gain_mode = (rtlsdr_set_tuner_gain_mode_ptr)GetProcAddress(hRtlsdr, "rtlsdr_set_tuner_gain_mode");
    auto rtlsdr_set_gain = (rtlsdr_set_tuner_gain_ptr)GetProcAddress(hRtlsdr, "rtlsdr_set_tuner_gain");
    auto rtlsdr_set_agc = (rtlsdr_set_agc_mode_ptr)GetProcAddress(hRtlsdr, "rtlsdr_set_agc_mode");
    auto rtlsdr_reset = (rtlsdr_reset_buffer_ptr)GetProcAddress(hRtlsdr, "rtlsdr_reset_buffer");
    auto rtlsdr_read_async = (rtlsdr_read_async_ptr)GetProcAddress(hRtlsdr, "rtlsdr_read_async");
    cancel_async_fn = (rtlsdr_cancel_async_ptr)GetProcAddress(hRtlsdr, "rtlsdr_cancel_async");

    if (get_dev_count() == 0) {
        std::cerr << "[Ingestor] No RTL-SDR devices found." << std::endl;
        FreeLibrary(hRtlsdr);
        isRunning = false;
        return;
    }

    rtlsdr_open(&sdrDev, 0);

    if (localMode == 1) { // ADS-B
        rtlsdr_set_rate(sdrDev, 2000000); 
        rtlsdr_set_freq(sdrDev, 1090000000); 
        rtlsdr_set_gain_mode(sdrDev, 1); 
        if (rtlsdr_set_gain) rtlsdr_set_gain(sdrDev, 496);
        if (rtlsdr_set_agc) rtlsdr_set_agc(sdrDev, 1); 
        std::cout << "[Ingestor] Mode: ADS-B (Planes) | 1090 MHz | 2 MSPS" << std::endl;
    } else { // AIS
        rtlsdr_set_rate(sdrDev, 1024000); 
        rtlsdr_set_freq(sdrDev, 161975000); 
        rtlsdr_set_gain_mode(sdrDev, 1); 
        if (rtlsdr_set_gain) rtlsdr_set_gain(sdrDev, 496);
        if (rtlsdr_set_agc) rtlsdr_set_agc(sdrDev, 1); 
        std::cout << "[Ingestor] Mode: AIS (Ships) | 161.975 MHz | 1.024 MSPS" << std::endl;
    }

    rtlsdr_reset(sdrDev);
    rtlsdr_read_async(sdrDev, rtlsdr_callback, nullptr, 0, 0);

    rtlsdr_close(sdrDev);
    FreeLibrary(hRtlsdr);
}

// ---------------------------------------------------------
// DSP & DECODER THREAD (Raw IQ + Aircraft State Update)
// ---------------------------------------------------------
void consumerThread() {
    if (sourceType == 2 && networkFormat == 2) return; 

    while (isRunning) {
        std::vector<unsigned char> rawData;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (!packetQueue.empty()) {
                rawData = std::move(packetQueue.front());
                packetQueue.pop();
            }
        }

        if (!rawData.empty()) {
            // --- ADS-B ---
            if ((sourceType == 1 && localMode == 1) || (sourceType == 2 && networkFormat == 1)) {
                std::vector<float> mag(rawData.size() / 2);
                for (size_t i = 0; i < rawData.size(); i += 2) {
                    float iv = (static_cast<float>(rawData[i])   - 127.5f) / 128.0f;
                    float qv = (static_cast<float>(rawData[i+1]) - 127.5f) / 128.0f;
                    mag[i / 2] = std::sqrt(iv * iv + qv * qv);
                }

                std::vector<ADSB_Packet> pkts = Decoder::decodeADSB_Chunk(mag);
                for (const auto& pkt : pkts) {
                    if (!pkt.isValid) continue;

                    // Update in-memory aircraft table
                    {
                        ULONGLONG tick = GetTickCount64();
                        std::lock_guard<std::mutex> lock(g_aircraftMutex);
                        auto& ac = g_aircraft[pkt.icao_address];
                        ac.icao  = pkt.icao_address;
                        if (ac.firstSeen == 0) ac.firstSeen = tick;
                        ac.lastSeen = tick;
                        ac.msgCount++;
                        ac.recentMsgTimes.push_back(tick);
                        ac.snr_db = pkt.snr_db;
                        if (pkt.hasIdentification && !pkt.callsign.empty())
                            ac.callsign = pkt.callsign;
                        if (!pkt.category.empty())  ac.category = pkt.category;
                        if (!pkt.squawk.empty())     ac.squawk   = pkt.squawk;
                        if (pkt.hasPosition) {
                            ac.lat = pkt.latitude; ac.lon = pkt.longitude;
                            ac.altitude = pkt.altitude; ac.isSurface = pkt.isSurface;
                        }
                        if (pkt.hasVelocity) {
                            ac.speed = pkt.speed; ac.heading = pkt.heading;
                            ac.vertRate = pkt.verticalRate;
                        }
                        if (pkt.hasEmergency) {
                            ac.hasEmergency = true;
                            ac.emergency = pkt.emergencyStr;
                        }
                        ac.lastRaw = pkt.rawHex;
                    }

                    // Build formatted log line
                    char logBuf[512];
                    sprintf_s(logBuf,
                        "%s | %s | DF%02d TC%02d | ICAO:%s | SNR:%+.1fdB",
                        getTimestamp().c_str(),
                        pkt.rawHex.c_str(),
                        pkt.df_val, pkt.tc_val,
                        pkt.icao_address.c_str(),
                        pkt.snr_db);
                    std::string logLine = logBuf;

                    if (pkt.hasIdentification) {
                        logLine += " CS:" + pkt.callsign;
                        if (!pkt.category.empty()) logLine += " CAT:" + pkt.category;
                    }
                    if (pkt.hasPosition) {
                        char pos[80];
                        sprintf_s(pos, " ALT:%d LAT:%.4f LON:%.4f%s",
                            pkt.altitude, pkt.latitude, pkt.longitude,
                            pkt.isSurface ? " [GND]" : "");
                        logLine += pos;
                    }
                    if (pkt.hasVelocity) {
                        char vel[64];
                        sprintf_s(vel, " SPD:%.0f HDG:%.0f VRT:%+d",
                            pkt.speed, pkt.heading, pkt.verticalRate);
                        logLine += vel;
                    }
                    if (!pkt.squawk.empty()) logLine += " SQK:" + pkt.squawk;
                    if (pkt.hasEmergency)    logLine += " *** EMERGENCY: " + pkt.emergencyStr + " ***";

                    // Raw bits header
                    logLine += "\r\n    BITS: ";
                    for (int b = 0; b < 32 && b < (int)pkt.rawBits.size(); b++) logLine += pkt.rawBits[b];

                    pushRawEntry(logLine);
                    dataLake.logFull("ADS-B", pkt.icao_address, pkt.callsign,
                        pkt.squawk, pkt.category,
                        pkt.altitude, pkt.speed, pkt.heading, pkt.verticalRate,
                        pkt.latitude, pkt.longitude, 1090.0f,
                        pkt.snr_db, pkt.hasEmergency, pkt.emergencyStr);
                }
            }
            // --- AIS ---
            else if (sourceType == 1 && localMode == 2) {
                std::vector<float> fmSignal;
                DSP::fmDemodulate(rawData, fmSignal);
                std::vector<ADSB_Packet> ships = AISDecoder::decodeAIS_Chunk(fmSignal, 1024000.0f);
                for (const auto& ship : ships) {
                    if (!ship.isValid) continue;
                    {
                        std::lock_guard<std::mutex> lock(g_aircraftMutex);
                        auto& ac    = g_aircraft[ship.icao_address];
                        ac.icao     = ship.icao_address;
                        ac.msgCount++;
                        ac.lastSeen = GetTickCount64();
                        if (ship.hasPosition) { ac.lat = ship.latitude; ac.lon = ship.longitude; ac.altitude = ship.altitude; }
                    }
                    std::string logLine = getTimestamp() + " | AIS MMSI:" + ship.icao_address;
                    char pos[64];
                    sprintf_s(pos, sizeof(pos), " LAT:%.4f LON:%.4f SPD:%.0f", ship.latitude, ship.longitude, ship.speed);
                    logLine += pos;
                    pushRawEntry(logLine);
                    dataLake.logPacket("AIS", ship.icao_address, ship.altitude, ship.latitude, ship.longitude, 161.975f);
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

// ---------------------------------------------------------
// NATIVE GUI (WIN32) - CONTROL PANEL
// ---------------------------------------------------------
#define ID_BTN_START    1001
#define ID_BTN_STOP     1002
#define ID_COMBO_SOURCE 1003
#define ID_COMBO_MODE   1004
#define ID_EDIT_IP      1005
#define ID_EDIT_PORT    1006
#define ID_TIMER_REFRESH 1

HWND g_hComboSource, g_hComboMode, g_hEditIP, g_hEditPort, g_hBtnStart, g_hBtnStop, g_hStatus;
std::thread prod, cons;

// ListView column definitions
struct ColDef { const char* name; int width; };
static ColDef g_cols[] = {
    {"ICAO",      72}, {"Callsign",  80}, {"Alt (ft)",  72},
    {"Spd (kt)",  65}, {"Hdg (\xb0)", 58}, {"VRate",     68},
    {"Latitude",  90}, {"Longitude", 90}, {"Category",  72},
    {"Squawk",    55}, {"SNR (dB)",  62}, {"Msgs/10s",  62},
    {"Last Seen", 75}
};
static const int NUM_COLS = 13;

void RefreshListView() {
    if (!g_hListView) return;
    ULONGLONG now = GetTickCount64();

    std::lock_guard<std::mutex> lock(g_aircraftMutex);
    SendMessage(g_hListView, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_hListView);

    int row = 0;
    int totalMsgs = 0;
    for (auto& [icao, ac] : g_aircraft) {
        if (now - ac.lastSeen > 90000) continue; // 90s timeout (matches backend)

        // Compute rolling msg rate (msgs in last 10 sec)
        ULONGLONG cutoff = now - 10000ULL;
        auto& times = ac.recentMsgTimes;
        times.erase(std::remove_if(times.begin(), times.end(),
            [&](ULONGLONG t){ return t < cutoff; }), times.end());
        ac.msgRate = (int)times.size();
        totalMsgs += ac.msgCount;

        // Prefix ICAO with [!] if emergency
        std::string icaoDisp = ac.hasEmergency ? ("[!]" + icao) : icao;

        LVITEMA lvi = {0};
        lvi.mask     = LVIF_TEXT;
        lvi.iItem    = row;
        lvi.iSubItem = 0;
        lvi.pszText  = (LPSTR)icaoDisp.c_str();
        ListView_InsertItem(g_hListView, &lvi);

        char buf[128];
        // Col 1: Callsign
        ListView_SetItemText(g_hListView, row, 1,
            (LPSTR)(ac.callsign.empty() ? "-" : ac.callsign.c_str()));
        // Col 2: Altitude
        if (ac.isSurface) sprintf_s(buf, "GND");
        else              sprintf_s(buf, "%d", ac.altitude);
        ListView_SetItemText(g_hListView, row, 2, buf);
        // Col 3-5: Speed / Heading / VRate
        sprintf_s(buf, "%.0f", ac.speed);    ListView_SetItemText(g_hListView, row, 3, buf);
        sprintf_s(buf, "%.0f", ac.heading);  ListView_SetItemText(g_hListView, row, 4, buf);
        sprintf_s(buf, "%+d",  ac.vertRate); ListView_SetItemText(g_hListView, row, 5, buf);
        // Col 6-7: Position
        sprintf_s(buf, "%.5f", ac.lat);      ListView_SetItemText(g_hListView, row, 6, buf);
        sprintf_s(buf, "%.5f", ac.lon);      ListView_SetItemText(g_hListView, row, 7, buf);
        // Col 8: Category
        ListView_SetItemText(g_hListView, row, 8,
            (LPSTR)(ac.category.empty() ? "-" : ac.category.c_str()));
        // Col 9: Squawk
        std::string sqBuf = ac.squawk.empty() ? "-" : ac.squawk;
        if (ac.squawk == "7700") sqBuf = "!EMRG";
        else if (ac.squawk == "7600") sqBuf = "!RCOM";
        else if (ac.squawk == "7500") sqBuf = "!HIJACK";
        ListView_SetItemText(g_hListView, row, 9, (LPSTR)sqBuf.c_str());
        // Col 10: SNR
        sprintf_s(buf, "%.1f", ac.snr_db);   ListView_SetItemText(g_hListView, row, 10, buf);
        // Col 11: Msg rate
        sprintf_s(buf, "%d", ac.msgRate);    ListView_SetItemText(g_hListView, row, 11, buf);
        // Col 12: Age
        ULONGLONG ageSec = (now - ac.lastSeen) / 1000ULL;
        sprintf_s(buf, "%llus", (unsigned long long)ageSec);
        ListView_SetItemText(g_hListView, row, 12, buf);
        row++;
    }

    SendMessage(g_hListView, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hListView, NULL, TRUE);

    // Update status bar with live stats
    if (g_hStatus) {
        char sb[128];
        sprintf_s(sb, "Status: RUNNING | Contacts: %d | Total msgs: %d", row, totalMsgs);
        SetWindowTextA(g_hStatus, sb);
    }
}

void AppendRawLog(const std::string& line) {
    if (!g_hRawLog) return;
    int len = GetWindowTextLengthA(g_hRawLog);
    // Cap at ~50000 chars to avoid slowdown
    if (len > 50000) {
        SetWindowTextA(g_hRawLog, "");
        len = 0;
    }
    SendMessageA(g_hRawLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    std::string full = line + "\r\n";
    SendMessageA(g_hRawLog, EM_REPLACESEL, 0, (LPARAM)full.c_str());
    SendMessageA(g_hRawLog, WM_VSCROLL, SB_BOTTOM, 0);
}

void StartEngine() {
    isRunning = true;
    char ipBuf[256] = {}, portBuf[64] = {};
    GetWindowTextA(g_hEditIP, ipBuf, 256);
    GetWindowTextA(g_hEditPort, portBuf, 64);
    netIP = std::string(ipBuf);
    try { netPort = std::stoi(portBuf); } catch(...) { netPort = 30003; }

    int src = (int)SendMessage(g_hComboSource, CB_GETCURSEL, 0, 0);
    int mod = (int)SendMessage(g_hComboMode,   CB_GETCURSEL, 0, 0);
    sourceType = (src == 0) ? 1 : 2;
    if (sourceType == 1) localMode     = (mod == 0) ? 1 : 2;
    else                  networkFormat = (mod == 0) ? 1 : 2;

    if (sourceType == 1) prod = std::thread(localIngestorCore);
    else                  prod = std::thread(networkIngestorThread);
    cons = std::thread(consumerThread);

    EnableWindow(g_hBtnStart, FALSE);
    EnableWindow(g_hBtnStop,  TRUE);
    SetWindowTextA(g_hStatus, "Status: RUNNING");
}

void StopEngine() {
    isRunning = false;
    if (sdrDev && cancel_async_fn) cancel_async_fn(sdrDev);
    if (prod.joinable()) prod.join();
    if (cons.joinable()) cons.join();
    EnableWindow(g_hBtnStart, TRUE);
    EnableWindow(g_hBtnStop,  FALSE);
    SetWindowTextA(g_hStatus, "Status: STOPPED");
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hMainWnd = hwnd;
        int y = 8;

        // --- Row 1: Source + Mode ---
        CreateWindowA("STATIC", "Source:",     WS_CHILD|WS_VISIBLE, 8,  y+3, 55, 20, hwnd, NULL,NULL,NULL);
        g_hComboSource = CreateWindowA("COMBOBOX","",CBS_DROPDOWNLIST|WS_CHILD|WS_VISIBLE, 65, y, 190, 200, hwnd,(HMENU)ID_COMBO_SOURCE,NULL,NULL);
        SendMessageA(g_hComboSource, CB_ADDSTRING,0,(LPARAM)"Local USB (RTL-SDR)");
        SendMessageA(g_hComboSource, CB_ADDSTRING,0,(LPARAM)"Network TCP Stream");
        SendMessageA(g_hComboSource, CB_SETCURSEL,0,0);

        CreateWindowA("STATIC", "Mode:",       WS_CHILD|WS_VISIBLE, 265,y+3, 45, 20, hwnd, NULL,NULL,NULL);
        g_hComboMode = CreateWindowA("COMBOBOX","",CBS_DROPDOWNLIST|WS_CHILD|WS_VISIBLE, 310, y, 210, 200, hwnd,(HMENU)ID_COMBO_MODE,NULL,NULL);
        SendMessageA(g_hComboMode, CB_ADDSTRING,0,(LPARAM)"ADS-B / RTL-TCP");
        SendMessageA(g_hComboMode, CB_ADDSTRING,0,(LPARAM)"AIS  / BaseStation");
        SendMessageA(g_hComboMode, CB_SETCURSEL,0,0);

        // --- Row 2: IP + Port + Buttons + Status ---
        y += 35;
        CreateWindowA("STATIC","IP:",          WS_CHILD|WS_VISIBLE, 8,  y+3, 25, 20, hwnd,NULL,NULL,NULL);
        g_hEditIP   = CreateWindowA("EDIT","127.0.0.1",WS_CHILD|WS_VISIBLE|WS_BORDER, 35, y, 155, 22, hwnd,(HMENU)ID_EDIT_IP,NULL,NULL);
        CreateWindowA("STATIC","Port:",        WS_CHILD|WS_VISIBLE, 200,y+3, 35, 20, hwnd,NULL,NULL,NULL);
        g_hEditPort = CreateWindowA("EDIT","30003",    WS_CHILD|WS_VISIBLE|WS_BORDER, 237, y,  70, 22, hwnd,(HMENU)ID_EDIT_PORT,NULL,NULL);

        g_hBtnStart = CreateWindowA("BUTTON","START",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,315,y-2,90,26,hwnd,(HMENU)ID_BTN_START,NULL,NULL);
        g_hBtnStop  = CreateWindowA("BUTTON","STOP", WS_CHILD|WS_VISIBLE,               413,y-2,90,26,hwnd,(HMENU)ID_BTN_STOP, NULL,NULL);
        EnableWindow(g_hBtnStop, FALSE);

        g_hStatus = CreateWindowA("STATIC","Status: READY",WS_CHILD|WS_VISIBLE,510,y+3,200,20,hwnd,NULL,NULL,NULL);

        // --- Aircraft ListView ---
        y += 34;
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
        InitCommonControlsEx(&icc);
        g_hListView = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
            WS_CHILD | WS_VISIBLE | LVS_REPORT,
            8, y, 854, 330, hwnd, NULL, GetModuleHandle(NULL), NULL);
        ListView_SetExtendedListViewStyle(g_hListView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        for (int c = 0; c < NUM_COLS; c++) {
            LVCOLUMNA col = {0};
            col.mask = LVCF_TEXT|LVCF_WIDTH|LVCF_SUBITEM;
            col.iSubItem  = c;
            col.cx        = g_cols[c].width;
            col.pszText   = (LPSTR)g_cols[c].name;
            ListView_InsertColumn(g_hListView, c, &col);
        }

        // --- Raw Data Log ---
        y += 338;
        CreateWindowA("STATIC","Raw Frame Log:",WS_CHILD|WS_VISIBLE,8,y,300,18,hwnd,NULL,NULL,NULL);
        y += 20;
        g_hRawLog = CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT","",
            WS_CHILD|WS_VISIBLE|ES_MULTILINE|ES_READONLY|WS_VSCROLL|ES_AUTOVSCROLL,
            8, y, 854, 155, hwnd, NULL, NULL, NULL);

        // Monospace font for raw log
        HFONT hMono = CreateFontA(14,0,0,0,FW_NORMAL,0,0,0,
            DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY,FIXED_PITCH|FF_MODERN,"Courier New");
        SendMessage(g_hRawLog, WM_SETFONT, (WPARAM)hMono, TRUE);

        // Refresh timer every 500ms
        SetTimer(hwnd, ID_TIMER_REFRESH, 500, NULL);
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == ID_BTN_START) StartEngine();
        if (id == ID_BTN_STOP)  StopEngine();
        break;
    }
    case WM_TIMER: {
        // Refresh aircraft table
        RefreshListView();

        // Drain raw log queue
        std::queue<RawEntry> toLog;
        { std::lock_guard<std::mutex> lock(g_rawMutex); std::swap(toLog, g_rawQueue); }
        while (!toLog.empty()) {
            AppendRawLog(toLog.front().line);
            toLog.pop();
        }
        break;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lParam;
        mm->ptMinTrackSize = { 900, 700 };
        break;
    }
    case WM_DESTROY: {
        KillTimer(hwnd, ID_TIMER_REFRESH);
        StopEngine();
        PostQuitMessage(0);
        break;
    }
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "SIGINT_MAIN";
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowA("SIGINT_MAIN",
        "Signal Logger  |  Advanced SIGINT Platform  |  C++ Engine",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1120, 720,
        NULL, NULL, hInstance, NULL);

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
