#define _WIN32_WINNT _WIN32_WINNT_WIN10
#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <wchar.h>
#include <unordered_map>
#include <string>
#include <mutex>

#define BUFFER_SIZE 0x10000

static std::unordered_map<ULONGLONG, std::wstring> g_frnCache;
static std::mutex g_cacheMutex;
static ULONGLONG g_rootFRN = 0;
static HANDLE g_hVolume = nullptr; 

static inline ULONGLONG GetFRNLow64(const FILE_ID_128* id) {
    return *(const ULONGLONG*)id;
}

ULONGLONG GetFRNFromHandle(HANDLE hFile) {
    BY_HANDLE_FILE_INFORMATION info = { 0 };
    if (GetFileInformationByHandle(hFile, &info)) {
        return ((ULONGLONG)info.nFileIndexHigh << 32) | info.nFileIndexLow;
    }
    return 0;
}

std::wstring ResolveParentPath(ULONGLONG parentFRN) {
    std::lock_guard<std::mutex> lock(g_cacheMutex);

    // Check cache first
    auto it = g_frnCache.find(parentFRN);
    if (it != g_frnCache.end()) {
        return it->second;
    }

    FILE_ID_DESCRIPTOR fid = { 0 };
    fid.dwSize = sizeof(fid);
    fid.Type = FileIdType;
    *(ULONGLONG*)&fid.FileId = parentFRN; 

    HANDLE hDir = OpenFileById(
        g_hVolume,
        &fid,
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT
    );

    if (hDir == INVALID_HANDLE_VALUE) {
        if (parentFRN == g_rootFRN || parentFRN == 0x5) {
            g_frnCache[parentFRN] = L"C:\\";
            return L"C:\\";
        }
        return L"";
    }

    wchar_t pathBuf[MAX_PATH * 2] = { 0 }; 
    DWORD len = GetFinalPathNameByHandleW(hDir, pathBuf, MAX_PATH * 2, FILE_NAME_NORMALIZED);
    CloseHandle(hDir);

    if (len > 0 && len < MAX_PATH * 2) {
        std::wstring path(pathBuf);
        if (path.compare(0, 4, L"\\\\?\\") == 0) 
            path = path.substr(4);
        
        g_frnCache[parentFRN] = path;
        return path;
    }

    return L"";
}

void PrintReason(DWORD reason) {
    if (reason & USN_REASON_FILE_CREATE) printf("[CREATE] ");
    else if (reason & USN_REASON_FILE_DELETE) printf("[DELETE] ");
    else if (reason & USN_REASON_RENAME_NEW_NAME) printf("[RENAME] ");
    else if ((reason & USN_REASON_DATA_OVERWRITE) || (reason & USN_REASON_DATA_EXTEND)) printf("[WRITE ] ");
    else printf("[OTHER ] ");
}

HANDLE OpenVolume(const wchar_t* drive) {
    wchar_t volume[64];
    swprintf_s(volume, L"\\\\.\\%s", drive);
    return CreateFileW(
        volume,
        GENERIC_READ | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, 
        NULL
    );
}

std::wstring ResolveFullPath(ULONGLONG parentFRN, const wchar_t* fileName, int nameLen) {
    std::wstring parentPath = ResolveParentPath(parentFRN);

    if (parentPath.empty()) {
        wchar_t buf[64];
        swprintf_s(buf, L"[FRN:%llX]\\%.*s", parentFRN, nameLen, fileName);
        return std::wstring(buf);
    }

    if (!parentPath.empty() && parentPath.back() != L'\\') 
        parentPath += L"\\";
    
    return parentPath + std::wstring(fileName, nameLen);
}

void CacheNewDirectory(ULONGLONG fileFRN, ULONGLONG parentFRN, const wchar_t* name, int nameLen) {
    std::wstring parentPath = ResolveParentPath(parentFRN);
    if (parentPath.empty()) return;

    if (!parentPath.empty() && parentPath.back() != L'\\') 
        parentPath += L"\\";
    
    std::wstring fullPath = parentPath + std::wstring(name, nameLen);

    std::lock_guard<std::mutex> lock(g_cacheMutex);
    g_frnCache[fileFRN] = fullPath;
}

void MonitorUSN(HANDLE hVolume) {
    g_hVolume = hVolume;

    // Pre-cache CRITICAL directories (instant - <100ms)
    auto TryCache = [](const wchar_t* path) {
        HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);

        if (h != INVALID_HANDLE_VALUE) {
            ULONGLONG frn = GetFRNFromHandle(h);
            CloseHandle(h);
  
            if (frn) {
                std::lock_guard<std::mutex> lock(g_cacheMutex);
                g_frnCache[frn] = path;
            }
        }
    };

    TryCache(L"C:\\");
    TryCache(L"C:\\Users");
    TryCache(L"C:\\ProgramData");
    TryCache(L"C:\\Windows");
    TryCache(L"C:\\Program Files");

    wchar_t userProf[MAX_PATH];
    if (GetEnvironmentVariableW(L"USERPROFILE", userProf, MAX_PATH)) 
        TryCache(userProf);

    DWORD bytes = 0;
    USN_JOURNAL_DATA jData = { 0 };
    
    if (!DeviceIoControl(hVolume, FSCTL_QUERY_USN_JOURNAL, NULL, 0,
        &jData, sizeof(jData), &bytes, NULL)) {
        printf("[!] Failed to query USN journal (err=%lu)\n", GetLastError());
        return;
    }

    printf("\n✅ Monitoring LIVE with ON-DEMAND PATH RESOLUTION\n");
    printf("   Cache size: %zu critical directories pre-loaded\n", g_frnCache.size());
    printf("   Unknown paths resolved instantly when needed\n");
    printf("--------------------------------------------------------\n\n");

    READ_USN_JOURNAL_DATA_V1 readData = { 0 };
    
    readData.StartUsn = jData.NextUsn;
    readData.ReasonMask = USN_REASON_FILE_CREATE | USN_REASON_FILE_DELETE |
        USN_REASON_RENAME_NEW_NAME | USN_REASON_DATA_OVERWRITE |
        USN_REASON_DATA_EXTEND | USN_REASON_CLOSE;
    readData.ReturnOnlyOnClose = FALSE;
    readData.Timeout = 0;
    readData.BytesToWaitFor = 0;
    readData.UsnJournalID = jData.UsnJournalID;
    readData.MinMajorVersion = 3;
    readData.MaxMajorVersion = 4;

    BYTE buffer[BUFFER_SIZE];

    while (1) {
        DWORD outBytes = 0;
        if (!DeviceIoControl(hVolume, FSCTL_READ_USN_JOURNAL, &readData,
            sizeof(readData), buffer, BUFFER_SIZE, &outBytes, NULL)) {
            DWORD err = GetLastError();
            if (err == ERROR_JOURNAL_ENTRY_DELETED || err == ERROR_JOURNAL_NOT_ACTIVE) {
                printf("\n[!] Journal reset! Re-initializing...\n");
                if (DeviceIoControl(hVolume, FSCTL_QUERY_USN_JOURNAL, NULL, 0,
                    &jData, sizeof(jData), &bytes, NULL)) {
                    readData.UsnJournalID = jData.UsnJournalID;
                    readData.StartUsn = jData.NextUsn;
                    continue;
                }
            }
            Sleep(500);
            continue;
        }

        if (outBytes <= sizeof(USN)) { Sleep(100); continue; }

        USN nextUsn = *(USN*)buffer;
        DWORD offset = sizeof(USN);

        while (offset < outBytes) {
            if (offset + sizeof(USN_RECORD_COMMON_HEADER) > outBytes) break;
            PUSN_RECORD_COMMON_HEADER common = (PUSN_RECORD_COMMON_HEADER)(buffer + offset);
            if (common->RecordLength == 0 || offset + common->RecordLength > outBytes) break;
            if (common->MajorVersion < 3 || common->MajorVersion > 4) {
                offset += common->RecordLength;
                continue;
            }

            USN_RECORD_V3* rec = (USN_RECORD_V3*)(buffer + offset);
            if (rec->FileNameLength == 0 || rec->FileNameLength > rec->RecordLength ||
                rec->FileNameOffset >= rec->RecordLength ||
                (rec->FileNameOffset + rec->FileNameLength) > rec->RecordLength ||
                (offset + rec->FileNameOffset + rec->FileNameLength) > outBytes) {
                offset += rec->RecordLength;
                continue;
            }

            wchar_t* fileName = (wchar_t*)((BYTE*)rec + rec->FileNameOffset);
            int nameLen = rec->FileNameLength / 2;
            if (nameLen > 255) nameLen = 255;

            ULONGLONG parentFRN = GetFRNLow64(&rec->ParentFileReferenceNumber);
            ULONGLONG fileFRN = GetFRNLow64(&rec->FileReferenceNumber);

            if ((rec->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                (rec->Reason & USN_REASON_FILE_CREATE)) {
                CacheNewDirectory(fileFRN, parentFRN, fileName, nameLen);
            }

            std::wstring fullPath = ResolveFullPath(parentFRN, fileName, nameLen);

            PrintReason(rec->Reason);
            wprintf(L"[V%u.%u] %s\n", rec->MajorVersion, rec->MinorVersion, fullPath.c_str());

            offset += rec->RecordLength;
        }

        readData.StartUsn = nextUsn;
        Sleep(10);
    }
}

BOOL WINAPI CtrlHandler(DWORD dwCtrlType) {
    if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT) {
        printf("\n\n[*] Shutting down... Final cache size: %zu directories\n", g_frnCache.size());
        exit(0);
        return TRUE;
    }

    return FALSE;
}

int main(void) {
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    HANDLE hVolume = OpenVolume(L"C:");
   
    if (hVolume == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        
        printf("[!] Failed to open volume (error %lu)\n", err);
        if (err == ERROR_ACCESS_DENIED)
            printf("[!] RUN AS ADMINISTRATOR REQUIRED\n");
        
        return 1;
    }

    printf("NTFS USN Journal Monitor (Real-Time Mode)\n");
    printf("Create/Delete/Rename files on C: to see events below:\n");
    printf("--------------------------------------------------------\n");

    MonitorUSN(hVolume);
    CloseHandle(hVolume);
    
    return 0;
}
