#define _WIN32_WINNT _WIN32_WINNT_WIN10
#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <wchar.h>

#define BUFFER_SIZE 0x10000

void PrintReason(DWORD reason)
{
    if (reason & USN_REASON_FILE_CREATE)
        printf("[CREATE] ");
    else if (reason & USN_REASON_FILE_DELETE)
        printf("[DELETE] ");
    else if (reason & USN_REASON_RENAME_NEW_NAME)
        printf("[RENAME] ");
    else if ((reason & USN_REASON_DATA_OVERWRITE) ||
        (reason & USN_REASON_DATA_EXTEND))
        printf("[WRITE ] ");
    else
        printf("[OTHER ] ");
}

HANDLE OpenVolume(const wchar_t* drive) {
    wchar_t volume[64];
    swprintf_s(volume, L"\\\\.\\%s", drive);

    return CreateFileW(
        volume,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        NULL
    );
}

static inline ULONGLONG GetFRNLow64(const FILE_ID_128* id) {
    return *(const ULONGLONG*)id;
}

void MonitorUSN(HANDLE hVolume)
{
    DWORD bytes = 0;
    USN_JOURNAL_DATA jData = { 0 };

    if (!DeviceIoControl(hVolume, FSCTL_QUERY_USN_JOURNAL, NULL, 0,
        &jData, sizeof(jData), &bytes, NULL)) {
        printf("[!] Failed to query USN journal (err=%lu)\n", GetLastError());
        return;
    }

    printf("Journal ready. FirstUsn=%lld, NextUsn=%lld\n",
        jData.FirstUsn, jData.NextUsn);
    printf("Monitoring REAL-TIME events (new changes only)...\n\n");

    READ_USN_JOURNAL_DATA_V1 readData = { 0 };

    readData.StartUsn = jData.NextUsn;

    readData.ReasonMask =
        USN_REASON_FILE_CREATE |
        USN_REASON_FILE_DELETE |
        USN_REASON_RENAME_NEW_NAME |
        USN_REASON_DATA_OVERWRITE |
        USN_REASON_DATA_EXTEND;
    readData.ReturnOnlyOnClose = FALSE;
    readData.Timeout = 0;
    readData.BytesToWaitFor = 0;
    readData.UsnJournalID = jData.UsnJournalID;
    readData.MinMajorVersion = 3; // Focus on modern records (V3+)
    readData.MaxMajorVersion = 4;

    BYTE buffer[BUFFER_SIZE];

    while (1) {
        DWORD outBytes = 0;

        if (!DeviceIoControl(
            hVolume,
            FSCTL_READ_USN_JOURNAL,
            &readData,
            sizeof(READ_USN_JOURNAL_DATA_V1),
            buffer,
            BUFFER_SIZE,
            &outBytes,
            NULL))
        {
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

        if (outBytes <= sizeof(USN)) {
            Sleep(100);
            continue;
        }

        USN nextUsn = *(USN*)buffer;
        DWORD offset = sizeof(USN);

        while (offset < outBytes) {
            // Validate minimum header size
            if (offset + sizeof(USN_RECORD_COMMON_HEADER) > outBytes) break;

            PUSN_RECORD_COMMON_HEADER common = (PUSN_RECORD_COMMON_HEADER)(buffer + offset);
            if (common->RecordLength == 0 || offset + common->RecordLength > outBytes) break;

            // Skip unsupported versions (focus on V3+ where FILE_ID_128 exists)
            if (common->MajorVersion < 3 || common->MajorVersion > 4) {
                offset += common->RecordLength;
                continue;
            }

            // SAFE CAST FOR MODERN RECORDS (V3/V4)
            USN_RECORD_V3* rec = (USN_RECORD_V3*)(buffer + offset);

            // CRITICAL FIX #2: CORRECT FILENAME VALIDATION
            if (rec->FileNameLength == 0 ||
                rec->FileNameLength > rec->RecordLength ||
                rec->FileNameOffset >= rec->RecordLength ||
                (rec->FileNameOffset + rec->FileNameLength) > rec->RecordLength ||
                (offset + rec->FileNameOffset + rec->FileNameLength) > outBytes) {
                offset += rec->RecordLength;
                continue;
            }

            // Extract filename safely
            wchar_t* fileName = (wchar_t*)((BYTE*)rec + rec->FileNameOffset);
            int nameLen = rec->FileNameLength / 2;
            if (nameLen > 255) nameLen = 255; // Prevent wprintf overflow

            // Get Parent FRN (lower 64 bits for NTFS)
            ULONGLONG parentFRN = GetFRNLow64(&rec->ParentFileReferenceNumber);

            // OUTPUT EVENT
            PrintReason(rec->Reason);
            wprintf(L"[V%u.%u][ParentFRN:%llX] %.*s\n",
                rec->MajorVersion,
                rec->MinorVersion,
                parentFRN,
                nameLen,
                fileName);

            offset += rec->RecordLength;
        }

        readData.StartUsn = nextUsn;
        Sleep(10);
    }
}

int main(void) {
    HANDLE hVolume = OpenVolume(L"C:");

    if (hVolume == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        printf("[!] Failed to open volume (error %lu)\n", err);
        if (err == ERROR_ACCESS_DENIED)
            printf("[!] RUN AS ADMINISTRATOR REQUIRED\n");

        return 1;
    }

    printf("NTFS USN Journal Monitor (Real-Time Mode)\n");
    printf("Create/delete/rename files on C: to see events below:\n");
    printf("--------------------------------------------------------\n");

    MonitorUSN(hVolume);

    CloseHandle(hVolume);

    return 0;
}