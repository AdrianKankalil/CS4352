/*
 * scheduler_os.cpp
 * CS 4352 - Operating Systems
 * Sim Singh | R11815168
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <curl/curl.h>

struct ElevatorBay {
    std::string name;
    int lowestFloor;
    int highestFloor;
    int currentFloor;
    int capacity;
};

std::string              g_baseURL;
std::vector<ElevatorBay> g_bays;
std::mutex               g_printMtx;

static size_t writeCB(void* ptr, size_t size, size_t nmemb, std::string* out) {
    out->append((char*)ptr, size * nmemb);
    return size * nmemb;
}

std::string httpGet(const std::string& url) {
    CURL* curl = curl_easy_init();
    std::string resp;
    if (!curl) return resp;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCB);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return resp;
}

std::string httpPut(const std::string& url) {
    CURL* curl = curl_easy_init();
    std::string resp;
    if (!curl) return resp;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCB);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return resp;
}

bool parseBuildingFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Error: cannot open " << path << std::endl;
        return false;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        ElevatorBay bay;
        std::string tmp;
        std::getline(ss, bay.name, '\t');
        std::getline(ss, tmp, '\t'); bay.lowestFloor  = std::stoi(tmp);
        std::getline(ss, tmp, '\t'); bay.highestFloor = std::stoi(tmp);
        std::getline(ss, tmp, '\t'); bay.currentFloor = std::stoi(tmp);
        std::getline(ss, tmp);       bay.capacity     = std::stoi(tmp);
        g_bays.push_back(bay);
    }
    return !g_bays.empty();
}

void inputThread() {
    std::lock_guard<std::mutex> lk(g_printMtx);
    std::cout << "input thread running" << std::endl;
}

void schedulerThread() {
    std::lock_guard<std::mutex> lk(g_printMtx);
    std::cout << "scheduler thread running" << std::endl;
}

void outputThread() {
    std::lock_guard<std::mutex> lk(g_printMtx);
    std::cout << "output thread running" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: scheduler_os <building_file> <port>" << std::endl;
        return 1;
    }

    std::string buildingFile = argv[1];
    std::string portStr      = argv[2];

    for (char c : portStr) {
        if (!std::isdigit(c)) {
            std::cerr << "Error: invalid port: " << portStr << std::endl;
            return 1;
        }
    }

    g_baseURL = "http://127.0.0.1:" + portStr;

    if (!parseBuildingFile(buildingFile)) {
        std::cerr << "Error: failed to parse building file" << std::endl;
        return 1;
    }

    std::cout << "Loaded " << g_bays.size() << " bay(s)" << std::endl;

    curl_global_init(CURL_GLOBAL_DEFAULT);

    std::string resp = httpPut(g_baseURL + "/Simulation/start");
    std::cout << resp << std::endl;

    std::thread t1(inputThread);
    std::thread t2(schedulerThread);
    std::thread t3(outputThread);

    t1.join();
    t2.join();
    t3.join();

    curl_global_cleanup();
    return 0;
}
