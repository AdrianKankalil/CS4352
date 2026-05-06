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
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <curl/curl.h>

// Stores information about an elevator bay
struct ElevatorBay {
    std::string name;
    int lowestFloor;
    int highestFloor;
    int currentFloor;
    int capacity;
};

// Stores information about one person requesting an elevator
struct Person {
    std::string id;
    int startFloor;
    int endFloor;
};
// Stores the final assignment of a person to the elevator
struct Assignment {
    std::string personID;
    std::string elevatorID;
};

// Global shared data used by the input, scheduler, and output threads to safely communicate
std::string              g_baseURL;
std::vector<ElevatorBay> g_bays;
std::atomic<bool>        g_done(false);

std::queue<Person>       g_personQueue;
std::mutex               g_personMtx;
std::condition_variable  g_personCV;

std::queue<Assignment>   g_assignQueue;
std::mutex               g_assignMtx;
std::condition_variable  g_assignCV;

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

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> splitOn(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::istringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, delim))
        out.push_back(trim(tok));
    return out;
}

bool simComplete() {
    std::string resp = httpGet(g_baseURL + "/Simulation/check");
    return resp.find("complete") != std::string::npos ||
           resp.find("stopped")  != std::string::npos;
}

std::string pickElevator(const Person& p) {
    for (auto& bay : g_bays) {
        if (p.startFloor >= bay.lowestFloor && p.startFloor <= bay.highestFloor &&
            p.endFloor   >= bay.lowestFloor && p.endFloor   <= bay.highestFloor)
            return bay.name;
    }
    return "";
}

void inputThread() {
    while (!g_done) {
        std::string resp = trim(httpGet(g_baseURL + "/NextInput"));

        if (resp.empty() || resp.find("NONE") != std::string::npos) {
            if (simComplete()) {
                g_done = true;
                g_personCV.notify_all();
                g_assignCV.notify_all();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        auto parts = splitOn(resp, '|');
        if (parts.size() < 3) continue;

        try {
            Person p;
            p.id         = parts[0];
            p.startFloor = std::stoi(parts[1]);
            p.endFloor   = std::stoi(parts[2]);
            std::unique_lock<std::mutex> lk(g_personMtx);
            g_personQueue.push(p);
            lk.unlock();
            g_personCV.notify_one();
        } catch (...) {}
    }
}

void schedulerThread() {
    while (!g_done) {
        std::unique_lock<std::mutex> lk(g_personMtx);
        g_personCV.wait(lk, [] { return !g_personQueue.empty() || g_done.load(); });
        if (g_personQueue.empty()) break;

        Person p = g_personQueue.front();
        g_personQueue.pop();
        lk.unlock();

        std::string elevID = pickElevator(p);
        if (elevID.empty()) continue;

        std::unique_lock<std::mutex> lk2(g_assignMtx);
        g_assignQueue.push({p.id, elevID});
        lk2.unlock();
        g_assignCV.notify_one();
    }
    g_assignCV.notify_all();
}

void outputThread() {
    while (true) {
        std::unique_lock<std::mutex> lk(g_assignMtx);
        g_assignCV.wait(lk, [] { return !g_assignQueue.empty() || g_done.load(); });
        if (g_assignQueue.empty()) break;

        Assignment a = g_assignQueue.front();
        g_assignQueue.pop();
        lk.unlock();

        std::string url = g_baseURL + "/AddPersonToElevator/" + a.personID + "/" + a.elevatorID;
        httpPut(url);
    }
}
// Main function. 
// This validates arguments, loads data, starts the simulation, creates worker threads, and waits for them to finish.
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

    curl_global_init(CURL_GLOBAL_DEFAULT);

    httpPut(g_baseURL + "/Simulation/start");

    std::thread t1(inputThread);
    std::thread t2(schedulerThread);
    std::thread t3(outputThread);

    t1.join();
    t2.join();
    t3.join();

    curl_global_cleanup();
    return 0;
}
