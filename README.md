# 🕵️ FileHistory: Real-Time NTFS USN Journal Monitor

[![Windows](https://img.shields.io/badge/OS-Windows_10%2F11-blue?logo=windows)](https://www.microsoft.com)
[![Language](https://img.shields.io/badge/Language-C%2B%2B17-blue?logo=c%2B%2B)](https://isocpp.org)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

A lightweight, high-performance C++ application that monitors the **NTFS Update Sequence Number (USN) Journal** to track real-time file system changes on Windows volumes. Built for developers, security researchers, and backup engineers who need low-level filesystem visibility.

https://github.com/Raulisr00t/FileHistory/assets/12345678/9a8b7c6d-5e4f-3a2b-1c0d-9f8e7d6c5b4a

## ✨ Features
- 🔍 **Real-time monitoring** of file creations, deletions, renames, and writes
- ⚡ **Kernel-level efficiency** (near-zero CPU overhead when idle)
- 🛡️ **Resilient design**: Auto-recovers from journal resets/wraps
- 🌐 **Modern Windows support**: USN_RECORD_V3/V4 (Windows 10 1709+)
- 📊 **Clean output**: Event type, record version, Parent FRN, filename
- 🚫 **No dependencies**: Pure Win32 API (no external libraries)
- 🔒 **Secure parsing**: Strict bounds checking prevents buffer overflows

## 📋 Prerequisites
| Requirement | Details |
|-------------|---------|
| **OS** | Windows 8+ (Windows 10/11 recommended) |
| **Architecture** | x64 |
| **Permissions** | **Administrator privileges required** (to access USN journal) |
| **Compiler** | Visual Studio 2019+ (with C++ desktop workload) |

## 🚀 Quick Start
### 1. Build the Project
```powershell
# Open Developer PowerShell for VS as Administrator
cd C:\Users\MrEll\source\repos\FileHistory
cl /EHsc /O2 /std:c++17 FileHistory.cpp /link /OUT:FileHistory.exe
```
### 2. Run as Administrator
```powershell
.\FileHistory.exe
```

### 3. Test It
```powershell
echo "test" > C:\Temp\usn_test.txt
ren C:\Temp\usn_test.txt usn_renamed.txt
del C:\Temp\usn_renamed.txt
```
### 📤 Sample Output
```powershell
NTFS USN Journal Monitor (Real-Time Mode)
Create/delete/rename files on C: to see events below:
--------------------------------------------------------
Journal ready. FirstUsn=12079595520, NextUsn=12115835808
Monitoring REAL-TIME events (new changes only)...

[CREATE] [V3.0][ParentFRN:80000000386C3] usn_test.txt
[WRITE ] [V3.0][ParentFRN:80000000386C3] usn_test.txt
[RENAME] [V3.0][ParentFRN:80000000386C3] usn_renamed.txt
[DELETE] [V3.0][ParentFRN:80000000386C3] usn_renamed.txt
[CREATE] [V3.0][ParentFRN:100000005FC65] data.sqlite-journal
[WRITE ] [V3.0][ParentFRN:40000000024FC] MSRDCEventProcessor_0.etl
```

## How It Works?
```graph LR
A[Open Volume Handle] --> B[Query USN Journal]
B --> C[Read Journal Records]
C --> D{Validate Record}
D -- Valid --> E[Parse V3/V4 Structure]
D -- Invalid --> F[Skip Safely]
E --> G[Extract Filename & FRN]
G --> H[Output Event]
H --> C

    Opens \\.\C: volume handle with GENERIC_READ
    Queries journal metadata via FSCTL_QUERY_USN_JOURNAL
    Reads records starting at NextUsn (skips historical backlog)
    Processes USN_RECORD_V3 with strict bounds validation
    Extracts lower 64-bits of FILE_ID_128 for NTFS FRN compatibility
    Auto-recovers on ERROR_JOURNAL_ENTRY_DELETED
```

## 🙏 Acknowledgements
Microsoft USN Journal documentation
Windows Driver Kit (WDK) examples
Community contributors to Win32 API knowledge base
