/*
 * scheduler_os.cpp
 * CS 4352 - Operating Systems
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
#include <climits>
#include <curl/curl.h>

// One elevator bay which stores both the static config from the file and the live status we poll from the server
struct ElevatorBay {
    std::string name;
    int lowestFloor;
    int highestFloor;
    int currentFloor;
    int capacity;
    int remaining;
    char direction;
};

struct Person {
    std::string id;
    int startFloor;
    int endFloor;
};

// Pairing a person with the elevator we picked for them, ready to be sent off by the output thread
struct Assignment {
    std::string personID;
    std::string elevatorID;
};

std::string              g_baseURL;
std::vector<ElevatorBay> g_bays;
std::mutex               g_baysMtx;
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

bool parseBuildingFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Error: cannot open " << path << std::endl;
        return false;
    }
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty()) continue;
        auto parts = splitOn(line, '\t');
        if (parts.size() < 5) continue;
        ElevatorBay bay;
        bay.name         = parts[0];
        bay.lowestFloor  = std::stoi(parts[1]);
        bay.highestFloor = std::stoi(parts[2]);
        bay.currentFloor = std::stoi(parts[3]);
        bay.capacity     = std::stoi(parts[4]);
        bay.remaining    = bay.capacity;
        bay.direction    = 'S';
        g_bays.push_back(bay);
    }
    return !g_bays.empty();
}

void updateBayStatus(ElevatorBay& bay) {
    std::string resp = httpGet(g_baseURL + "/ElevatorStatus/" + bay.name);
    if (resp.empty()) return;
    auto parts = splitOn(resp, '|');
    if (parts.size() < 5) return;
    try {
        bay.currentFloor = std::stoi(parts[1]);
        bay.direction    = parts[2].empty() ? 'S' : parts[2][0];
        bay.remaining    = std::stoi(parts[4]);
    } catch (...) {}
}

void refreshStatuses() {
    std::lock_guard<std::mutex> lk(g_baysMtx);
    for (auto& bay : g_bays)
        updateBayStatus(bay);
}

bool simComplete() {
    std::string resp = httpGet(g_baseURL + "/Simulation/check");
    return resp.find("complete") != std::string::npos ||
           resp.find("stopped")  != std::string::npos;
}

int scoreElevator(const ElevatorBay& bay, const Person& p) {
    if (p.startFloor < bay.lowestFloor  || p.startFloor > bay.highestFloor) return INT_MAX;
    if (p.endFloor   < bay.lowestFloor  || p.endFloor   > bay.highestFloor) return INT_MAX;
    if (bay.remaining <= 0) return INT_MAX;

    int dist = std::abs(bay.currentFloor - p.startFloor);

    int dirBonus = 0;
    bool goingUp   = (p.startFloor > bay.currentFloor);
    bool goingDown = (p.startFloor < bay.currentFloor);
    if ((bay.direction == 'U' && goingUp) || (bay.direction == 'D' && goingDown))
        dirBonus = -5;
    else if (bay.direction != 'S')
        dirBonus = 10;

    return dist + dirBonus - bay.remaining;
}

std::string pickBestElevator(const Person& p) {
    std::lock_guard<std::mutex> lk(g_baysMtx);
    int bestScore    = INT_MAX;
    std::string best = "";
    for (auto& bay : g_bays) {
        int s = scoreElevator(bay, p);
        if (s < bestScore) {
            bestScore = s;
            best      = bay.name;
        }
    }
    return best;
}

void decrementCapacity(const std::string& elevName) {
    std::lock_guard<std::mutex> lk(g_baysMtx);
    for (auto& bay : g_bays) {
        if (bay.name == elevName && bay.remaining > 0) {
            bay.remaining--;
            break;
        }
    }
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
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
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
    int count = 0;

    while (!g_done) {
        std::unique_lock<std::mutex> lk(g_personMtx);
        g_personCV.wait(lk, [] { return !g_personQueue.empty() || g_done.load(); });
        if (g_personQueue.empty()) break;

        Person p = g_personQueue.front();
        g_personQueue.pop();
        lk.unlock();

        if (count % 5 == 0)
            refreshStatuses();
        count++;

        std::string elevID = pickBestElevator(p);
        if (elevID.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::unique_lock<std::mutex> lk2(g_personMtx);
            g_personQueue.push(p);
            lk2.unlock();
            g_personCV.notify_one();
            continue;
        }

        decrementCapacity(elevID);

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

    // drain anything left after done
    while (true) {
        std::unique_lock<std::mutex> lk(g_assignMtx);
        if (g_assignQueue.empty()) break;
        Assignment a = g_assignQueue.front();
        g_assignQueue.pop();
        lk.unlock();
        httpPut(g_baseURL + "/AddPersonToElevator/" + a.personID + "/" + a.elevatorID);
    }
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

    curl_global_init(CURL_GLOBAL_DEFAULT);

    httpPut(g_baseURL + "/Simulation/start");

    refreshStatuses();

    std::thread t1(inputThread);
    std::thread t2(schedulerThread);
    std::thread t3(outputThread);

    t1.join();
    t2.join();
    t3.join();

    curl_global_cleanup();
    return 0;
}
