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

    std::wcout << colorCode << currentTime() << resetCode << message << std::endl;
}

std::wstring extractFileName(const std::wstring& filePath) {
    size_t lastBackslash = filePath.find_last_of(L'\\');
    return (lastBackslash != std::wstring::npos) ? filePath.substr(lastBackslash + 1) : filePath;
}

bool containsSignature(const std::vector<char>& data, const std::vector<std::string>& signatures) {
    std::string content(data.begin(), data.end());
    for (const auto& sig : signatures) {
        if (content.find(sig) != std::string::npos) {
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

bool checkForAudioType(const std::wstring& filePath, const std::wstring& logFilePath, bool isExistingFile) {
    std::wifstream logFile(logFilePath);
    std::set<std::wstring> processedFiles;
    std::wstring line;

    while (std::getline(logFile, line)) {
        processedFiles.insert(line);
    }
    logFile.close();

    if (processedFiles.find(filePath) != processedFiles.end()) {
        return false;
    }

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        log(L"Error opening file " + filePath + L": " + std::to_wstring(GetLastError()));
        return false;
    }

    std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    bool copied = false;

    if (isOggFile(data)) {
        copied = copyFileToDir(filePath, "ogg");
    }
    else if (isMp3File(data)) {
        copied = copyFileToDir(filePath, "mp3");
    }

    if (copied) {
        std::wofstream logFile(logFilePath, std::ios::app);
        logFile << filePath << std::endl;
        logFile.close();

        if (isExistingFile) {
            log(L"File copied to 'saved_audios' directory: " + extractFileName(filePath) + L"." + (isOggFile(data) ? L"ogg" : L"mp3"));
        }
        else {
            log(L"New audio discovered, copied to: " + extractFileName(filePath) + L"." + (isOggFile(data) ? L"ogg" : L"mp3"));
        }
    }

    return copied;
}

void copyExistingFiles(const std::wstring& path, const std::wstring& logFilePath) {
    log(L"Scanning existing files...");
    for (const auto& entry : fs::directory_iterator(path)) {
        if (entry.is_regular_file()) {
            checkForAudioType(entry.path().wstring(), logFilePath, true);
        }
    }
    log(L"Finished scanning existing files");
}

void watchDir(const std::wstring& path, const std::wstring& logFilePath) {
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

            if (wcsncmp(fileName, L"RBX", 3) != 0) {
                std::wstring filePath = path + L"\\" + fileName;

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
        log(L"Error getting temporary directory: " + std::to_wstring(GetLastError()));
        return 1;
    }

    std::wstring directoryPath = tempDir;
    directoryPath += L"Roblox\\http";
    std::wstring logFilePath = L"processed_files.log";

    copyExistingFiles(directoryPath, logFilePath);
    watchDir(directoryPath, logFilePath);

    return 0;
}
