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

namespace fs = std::filesystem;
using namespace std;

void enableAnsi();
wstring currentTime();
void log(const wstring& message);
wstring extractFileName(const wstring& filePath);
bool containsSignature(const string& data, const vector<string>& signatures);
bool isOggFile(const string& data);
bool isMp3File(const string& data);
bool copyFileToDir(const wstring& filePath, const string& audioType);
bool checkForAudioType(const wstring& filePath, const wstring& logFilePath, bool isExistingFile);
void copyExistingFiles(const wstring& path, const wstring& logFilePath);
void watchDir(const wstring& path, const wstring& logFilePath);

void enableAnsi() {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hConsole == INVALID_HANDLE_VALUE) return;

    DWORD dwMode = 0;
    if (GetConsoleMode(hConsole, &dwMode)) {
        dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(hConsole, dwMode);
    }
}

wstring currentTime() {
    using namespace chrono;
    auto now = system_clock::now();
    auto time = system_clock::to_time_t(now);

    tm tm;
    localtime_s(&tm, &time);

    wostringstream oss;
    oss << L"[ " << put_time(&tm, L"%H:%M:%S") << L" ] ";
    return oss.str();
}

void log(const wstring& message) {
    const wstring colorCode = L"\033[1;32m";
    const wstring resetCode = L"\033[0m";

    wcout << colorCode << currentTime() << resetCode << message << endl;
}

wstring extractFileName(const wstring& filePath) {
    return fs::path(filePath).filename().wstring();
}

bool containsSignature(const string& data, const vector<string>& signatures) {
    for (const auto& sig : signatures) {
        if (data.find(sig) != string::npos) {
            return true;
        }
    }
    return false;
}

bool isOggFile(const string& data) {
    static const vector<string> signatures = { "OggS", "vorbis" };
    return containsSignature(data, signatures);
}

bool isMp3File(const string& data) {
    static const vector<string> signatures = { "ID3", "LAME", "matroska" };
    return containsSignature(data, signatures);
}

bool copyFileToDir(const wstring& filePath, const string& audioType) {
    wstring savedAudiosDir = L"saved_audios";

    if (!fs::exists(savedAudiosDir)) {
        try {
            fs::create_directory(savedAudiosDir);
        } catch (const fs::filesystem_error& e) {
            log(L"Error creating 'saved_audios' directory: " + wstring(e.what()));
            return false;
        }
    }

    wstring fileName = extractFileName(filePath);
    wstring destinationPath = savedAudiosDir + L"\\" + fileName + L"." + wstring(audioType.begin(), audioType.end());

    try {
        fs::copy(filePath, destinationPath, fs::copy_options::overwrite_existing);
        return true;
    } catch (const fs::filesystem_error& e) {
        log(L"Error copying file to 'saved_audios' directory: " + wstring(e.what()));
        return false;
    }
}

bool checkForAudioType(const wstring& filePath, const wstring& logFilePath, bool isExistingFile) {
    static set<wstring> processedFiles;
    if (processedFiles.empty()) {
        wifstream logFile(logFilePath);
        wstring line;
        while (getline(logFile, line)) {
            processedFiles.insert(line);
        }
        logFile.close();
    }

    if (processedFiles.find(filePath) != processedFiles.end()) {
        return false;
    }

    ifstream file(filePath, ios::binary | ios::ate);
    if (!file.is_open()) {
        log(L"Error opening file " + filePath + L": " + to_wstring(GetLastError()));
        return false;
    }

    streamsize size = file.tellg();
    file.seekg(0, ios::beg);
    string data(size, '\0');
    if (!file.read(&data[0], size)) {
        log(L"Error reading file " + filePath + L": " + to_wstring(GetLastError()));
        return false;
    }
    file.close();

    bool copied = false;

    if (isOggFile(data)) {
        copied = copyFileToDir(filePath, "ogg");
    } else if (isMp3File(data)) {
        copied = copyFileToDir(filePath, "mp3");
    }

    if (copied) {
        processedFiles.insert(filePath);
        wofstream logFile(logFilePath, ios::app);
        logFile << filePath << endl;
        logFile.close();

        log(L"File copied to 'saved_audios' directory: " + extractFileName(filePath) + L"." + (isOggFile(data) ? L"ogg" : L"mp3"));
    }

    return copied;
}

void copyExistingFiles(const wstring& path, const wstring& logFilePath) {
    using namespace chrono;

    auto start = high_resolution_clock::now();

    log(L"Scanning existing files...");
    for (const auto& entry : fs::directory_iterator(path)) {
        if (entry.is_regular_file()) {
            checkForAudioType(entry.path().wstring(), logFilePath, true);
        }
    }

    auto end = high_resolution_clock::now();
    duration<double> elapsed = end - start;
    wostringstream oss;
    oss << fixed << setprecision(2);
    oss << L"Finished scanning existing files, took " << elapsed.count() << L" seconds";
    log(oss.str());
}

void watchDir(const wstring& path, const wstring& logFilePath) {
    HANDLE hDir = CreateFile(path.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);

    if (hDir == INVALID_HANDLE_VALUE) {
        log(L"Error opening directory: " + to_wstring(GetLastError()));
        return;
    }

    DWORD dwBytesReturned;
    char buffer[4096];

    while (ReadDirectoryChangesW(hDir, buffer, sizeof(buffer), TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE, &dwBytesReturned, nullptr, nullptr)) {
        FILE_NOTIFY_INFORMATION* pInfo = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);

        while (pInfo) {
            wchar_t fileName[MAX_PATH];
            wcsncpy_s(fileName, pInfo->FileName, pInfo->FileNameLength / sizeof(wchar_t));

            if (wcsncmp(fileName, L"RBX", 3) != 0) {
                wstring filePath = path + L"\\" + fileName;

                WIN32_FILE_ATTRIBUTE_DATA fileAttr;
                if (GetFileAttributesEx(filePath.c_str(), GetFileExInfoStandard, &fileAttr)) {
                    checkForAudioType(filePath, logFilePath, false);
                }
            }

            if (pInfo->NextEntryOffset == 0)
                break;

            pInfo = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<char*>(pInfo) + pInfo->NextEntryOffset);
        }
    }

    CloseHandle(hDir);
}

int main() {
    enableAnsi();

    log(L"rbxtractor written by sewshee https://sewshee.derg.lol/discord\n");

    wchar_t tempDir[MAX_PATH];
    if (GetTempPath(MAX_PATH, tempDir) == 0) {
        log(L"Error getting temporary directory: " + to_wstring(GetLastError()));
        return 1;
    }

    wstring directoryPath = tempDir;
    directoryPath += L"Roblox\\http";
    wstring logFilePath = L"processed_files.log";

    copyExistingFiles(directoryPath, logFilePath);
    watchDir(directoryPath, logFilePath);

    return 0;
}
