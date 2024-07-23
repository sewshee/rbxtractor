#include <Windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <set>
#include <thread>
#include <atomic>
#include <algorithm>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <functional>

namespace fs = std::filesystem;

std::mutex logMutex;

void enableAnsi() {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hConsole == INVALID_HANDLE_VALUE) return;

    DWORD dwMode = 0;
    if (GetConsoleMode(hConsole, &dwMode)) {
        dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(hConsole, dwMode);
    }
}

std::wstring currentTime() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto time = system_clock::to_time_t(now);

    std::tm tm;
    localtime_s(&tm, &time);

    std::wostringstream oss;
    oss << L"[ " << std::put_time(&tm, L"%H:%M:%S") << L" ] ";
    return oss.str();
}

void log(const std::wstring& message) {
    const std::wstring colorCode = L"\033[1;32m";
    const std::wstring resetCode = L"\033[0m";

    std::lock_guard<std::mutex> guard(logMutex);
    std::wcout << colorCode << currentTime() << resetCode << message << std::endl;
}

std::wstring extractFileName(const std::wstring& filePath) {
    size_t lastBackslash = filePath.find_last_of(L'\\');
    return (lastBackslash != std::wstring::npos) ? filePath.substr(lastBackslash + 1) : filePath;
}

bool containsSignature(const std::vector<char>& data, const std::vector<std::string>& signatures) {
    for (const auto& sig : signatures) {
        if (std::search(data.begin(), data.end(), sig.begin(), sig.end()) != data.end()) {
            return true;
        }
    }
    return false;
}

bool isOggFile(const std::vector<char>& data) {
    static const std::vector<std::string> signatures = { "OggS", "vorbis" };
    return containsSignature(data, signatures);
}

bool isMp3File(const std::vector<char>& data) {
    static const std::vector<std::string> signatures = { "ID3", "LAME", "matroska" };
    return containsSignature(data, signatures);
}

bool copyFileToDir(const std::wstring& filePath, const std::string& audioType) {
    std::wstring savedAudiosDir = L"saved_audios";

    if (!CreateDirectory(savedAudiosDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        log(L"Error creating 'saved_audios' directory: " + std::to_wstring(GetLastError()));
        return false;
    }

    std::wstring fileName = extractFileName(filePath);
    std::wstring destinationPath = savedAudiosDir + L"\\" + fileName + L"." + std::wstring(audioType.begin(), audioType.end());

    if (CopyFile(filePath.c_str(), destinationPath.c_str(), FALSE)) {
        return true;
    }
    else {
        log(L"Error copying file to 'saved_audios' directory: " + std::to_wstring(GetLastError()));
        return false;
    }
}

std::set<std::wstring> loadProcessedFiles(const std::wstring& logFilePath) {
    std::set<std::wstring> processedFiles;
    std::wifstream logFile(logFilePath);
    std::wstring line;

    while (std::getline(logFile, line)) {
        processedFiles.insert(line);
    }

    return processedFiles;
}

void saveProcessedFile(const std::wstring& filePath, const std::wstring& logFilePath) {
    std::wofstream logFile(logFilePath, std::ios::app);
    logFile << filePath << std::endl;
}

bool checkForAudioType(const std::wstring& filePath, const std::wstring& logFilePath, bool isExistingFile, std::set<std::wstring>& processedFiles) {
    if (processedFiles.find(filePath) != processedFiles.end()) {
        return false;
    }

    const int maxRetries = 10;
    const int retryDelay = 1000;
    bool fileOpened = false;

    std::ifstream file;

    for (int retry = 0; retry < maxRetries; ++retry) {
        file.open(filePath, std::ios::binary);
        if (file.is_open()) {
            fileOpened = true;
            break;
        }

        DWORD error = GetLastError();
        if (error == ERROR_SHARING_VIOLATION) {
            std::this_thread::sleep_for(std::chrono::milliseconds(retryDelay));
        }
        else {
            log(L"Error opening file " + filePath + L": " + std::to_wstring(error));
            return false;
        }
    }

    if (!fileOpened) {
        log(L"Failed to open file after " + std::to_wstring(maxRetries) + L" retries: " + filePath);
        return false;
    }

    std::vector<char> buffer(1024);
    bool copied = false;

    while (file.read(buffer.data(), buffer.size())) {
        if (isOggFile(buffer)) {
            copied = copyFileToDir(filePath, "ogg");
            break;
        }
        else if (isMp3File(buffer)) {
            copied = copyFileToDir(filePath, "mp3");
            break;
        }
    }

    if (!copied) {
        if (isOggFile(buffer)) {
            copied = copyFileToDir(filePath, "ogg");
        }
        else if (isMp3File(buffer)) {
            copied = copyFileToDir(filePath, "mp3");
        }
    }

    if (copied) {
        saveProcessedFile(filePath, logFilePath);
        processedFiles.insert(filePath);

        log(L"File copied to 'saved_audios' directory: " + extractFileName(filePath) + L"." + (isOggFile(buffer) ? L"ogg" : L"mp3"));
    }

    return copied;
}

void scanFile(const std::wstring& filePath, const std::wstring& logFilePath, std::atomic<int>& fileCount, std::set<std::wstring>& processedFiles) {
    checkForAudioType(filePath, logFilePath, true, processedFiles);
    fileCount++;
}

class ThreadPool {
public:
    ThreadPool(size_t numThreads);
    ~ThreadPool();

    template<class F>
    void enqueue(F&& f);

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queueMutex;
    std::condition_variable condition;
    bool stop;
};

ThreadPool::ThreadPool(size_t numThreads) : stop(false) {
    for (size_t i = 0; i < numThreads; ++i) {
        workers.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(queueMutex);
                    condition.wait(lock, [this] { return stop || !tasks.empty(); });
                    if (stop && tasks.empty()) return;
                    task = std::move(tasks.front());
                    tasks.pop();
                }
                task();
            }
            });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        stop = true;
    }
    condition.notify_all();
    for (std::thread& worker : workers) {
        worker.join();
    }
}

template<class F>
void ThreadPool::enqueue(F&& f) {
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        if (stop) throw std::runtime_error("enqueue on stopped ThreadPool");
        tasks.emplace(std::forward<F>(f));
    }
    condition.notify_one();
}

void copyExistingFiles(const std::wstring& path, const std::wstring& logFilePath, std::set<std::wstring>& processedFiles) {
    using namespace std::chrono;

    auto start = high_resolution_clock::now();
    log(L"Scanning existing files...");

    unsigned int numThreads = std::thread::hardware_concurrency();
    ThreadPool pool(numThreads);

    std::atomic<int> fileCount(0);
    std::atomic<int> tasksPending(0);

    for (const auto& entry : fs::directory_iterator(path)) {
        if (entry.is_regular_file()) {
            tasksPending++;
            pool.enqueue([filePath = entry.path().wstring(), logFilePath, &processedFiles, &fileCount, &tasksPending] {
                scanFile(filePath, logFilePath, fileCount, processedFiles);
                tasksPending--;
                });
        }
    }

    while (tasksPending > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    auto end = high_resolution_clock::now();
    duration<double> elapsed = end - start;
    std::wostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << L"Finished scanning " << fileCount << L" files, took " << elapsed.count() << L" seconds";
    log(oss.str());
}

void watchDir(const std::wstring& path, const std::wstring& logFilePath, std::set<std::wstring>& processedFiles) {
    HANDLE hDir = CreateFile(path.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);

    if (hDir == INVALID_HANDLE_VALUE) {
        log(L"Error opening directory: " + std::to_wstring(GetLastError()));
        return;
    }

    DWORD dwBytesReturned;
    char buffer[4096];

    while (ReadDirectoryChangesW(hDir, buffer, sizeof(buffer), TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE, &dwBytesReturned, nullptr, nullptr)) {
        FILE_NOTIFY_INFORMATION* pInfo = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);

        while (pInfo) {
            wchar_t fileName[MAX_PATH];
            wcsncpy_s(fileName, pInfo->FileName, pInfo->FileNameLength / sizeof(wchar_t));
            fileName[pInfo->FileNameLength / sizeof(wchar_t)] = L'\0';

            if (wcsncmp(fileName, L"RBX", 3) != 0) {
                std::wstring filePath = path + L"\\" + fileName;

                WIN32_FILE_ATTRIBUTE_DATA fileAttr;
                if (GetFileAttributesEx(filePath.c_str(), GetFileExInfoStandard, &fileAttr)) {
                    checkForAudioType(filePath, logFilePath, false, processedFiles);
                }
            }

            if (pInfo->NextEntryOffset == 0) break;
            pInfo = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<char*>(pInfo) + pInfo->NextEntryOffset);
        }
    }

    CloseHandle(hDir);
}

int main() {
    enableAnsi();

    log(L"rbxtractor written by sewshee and contributors https://sewshee.derg.lol/discord https://github.com/sewshee\n");

    wchar_t tempDir[MAX_PATH];
    if (GetTempPath(MAX_PATH, tempDir) == 0) {
        log(L"Error getting temporary directory: " + std::to_wstring(GetLastError()));
        return 1;
    }

    std::wstring directoryPath = tempDir;
    directoryPath += L"Roblox\\http";
    std::wstring logFilePath = L"processed_files.log";

    auto processedFiles = loadProcessedFiles(logFilePath);

    copyExistingFiles(directoryPath, logFilePath, processedFiles);
    watchDir(directoryPath, logFilePath, processedFiles);

    return 0;
}