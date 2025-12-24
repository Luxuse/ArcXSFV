#define UNICODE
#define _UNICODE

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <cstdlib> // Ajouté pour strtoull

#include "arcahash.h" 

#pragma comment(lib, "comctl32.lib")

using namespace std;
namespace fs = std::filesystem;

/* ===================== CONSTANTES & COULEURS ===================== */
#define CLR_OK      RGB(0, 180, 0)
#define CLR_BAD     RGB(220, 0, 0)
#define CLR_MISSING RGB(255, 0, 255)
#define CLR_PROC    RGB(0, 100, 255)
#define CLR_NORM    RGB(0, 0, 0)    // Renommé pour éviter le conflit

#define WM_STAT     (WM_APP + 2)
#define WM_START    (WM_APP + 3)
#define WM_PROGRESS (WM_APP + 4)
#define WM_DONE     (WM_APP + 5)
#define WM_REFRESH_LIST (WM_APP + 6)

/* ===================== STRUCTURES ===================== */
struct Job {
    fs::path fullPath;
    wstring relPath;
    wchar_t statusText[64]; 
    COLORREF color;
    uint64_t expectedHash;
    uint64_t resultHash;
    uint64_t size;
    int sortPriority; 

    Job() {
        wcscpy_s(statusText, L"Queued");
        color = CLR_NORM; // Utilisation du nouveau nom
        sortPriority = 3;
        expectedHash = 0;
        resultHash = 0;
        size = 0;
    }
};

/* ===================== GLOBALS ===================== */
HWND g_hwndMain, g_hwndProgress, g_hwndStatus, g_hwndList;
HWND g_hwndCreate, g_hwndVerify, g_hwndStop;

atomic<bool> g_stopRequested(false);
atomic<bool> g_processing(false);
atomic<uint64_t> g_processedBytes(0);
atomic<int> g_processedFiles(0);

vector<Job> g_jobs;
mutex g_dataMutex;

/* ===================== UI HELPERS ===================== */
void SetStatusSafe(const wstring& s) {
    wchar_t* b = _wcsdup(s.c_str());
    PostMessage(g_hwndMain, WM_STAT, 0, (LPARAM)b);
}

void UpdateJobStatus(int idx, const wchar_t* text, COLORREF col, int priority) {
    if (idx < 0 || idx >= g_jobs.size()) return;
    wcscpy_s(g_jobs[idx].statusText, text);
    g_jobs[idx].color = col;
    g_jobs[idx].sortPriority = priority;
}

/* ===================== CORE HASH LOGIC ===================== */
uint64_t FastHashFile(const fs::path& p) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const DWORD grain = si.dwAllocationGranularity;

    HANDLE hFile = CreateFile(p.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, 
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return 0;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart == 0) {
        CloseHandle(hFile);
        return 0;
    }

    HANDLE hMap = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMap) { CloseHandle(hFile); return 0; }

    arca_ctx ctx;
    arca_init(&ctx, 0); 

    uint64_t remaining = size.QuadPart;
    uint64_t offset = 0;
    const uint64_t CHUNK_SIZE = 1024 * 1024 * 32; 

    while(remaining > 0 && !g_stopRequested) {
        uint64_t alignedOffset = (offset / grain) * grain;
        uint32_t padding = (uint32_t)(offset - alignedOffset);
        uint32_t toMap = (uint32_t)min((uint64_t)CHUNK_SIZE, remaining + padding);

        void* view = MapViewOfFile(hMap, FILE_MAP_READ, (DWORD)(alignedOffset >> 32), (DWORD)(alignedOffset & 0xFFFFFFFF), toMap);
        if (!view) break;

        const uint8_t* dataStart = (const uint8_t*)view + padding;
        uint32_t actualData = (uint32_t)min((uint64_t)toMap - padding, remaining);

        arca_update(&ctx, dataStart, actualData);

        UnmapViewOfFile(view);
        offset += actualData;
        remaining -= actualData;
        g_processedBytes += actualData;
    }

    CloseHandle(hMap);
    CloseHandle(hFile);
    return arca_finalize(&ctx);
}

/* ===================== WORKER LOGIC ===================== */
void GlobalWorker(atomic<size_t>* nextJob, bool verifyMode) {
    while (!g_stopRequested) {
        size_t idx = nextJob->fetch_add(1);
        if (idx >= g_jobs.size()) break;

        Job& job = g_jobs[idx];
        UpdateJobStatus((int)idx, L"Hashing...", CLR_PROC, 3);

        if (verifyMode) {
            if (!fs::exists(job.fullPath)) {
                UpdateJobStatus((int)idx, L"MISSING", CLR_MISSING, 0);
            } else {
                job.resultHash = FastHashFile(job.fullPath);
                bool ok = (job.resultHash == job.expectedHash);
                if (ok) UpdateJobStatus((int)idx, L"OK", CLR_OK, 2);
                else UpdateJobStatus((int)idx, L"CORRUPT", CLR_BAD, 0);
            }
        } else {
            if (!fs::exists(job.fullPath)) {
                UpdateJobStatus((int)idx, L"ERROR ACCESS", CLR_BAD, 0);
            } else {
                job.resultHash = FastHashFile(job.fullPath);
                UpdateJobStatus((int)idx, L"Done", CLR_OK, 2);
            }
        }
        g_processedFiles++;
    }
}

/* ===================== TEXT FORMAT I/O ===================== */
bool SaveTextArca(const fs::path& arcaPath) {
    ofstream out(arcaPath, ios::binary);
    if (!out) return false;
    out.write("\xEF\xBB\xBF", 3);
    out << "; ArcXSFV Hash File v1.0\n";
    out << "; Generated: " << fs::path(arcaPath).filename().string() << "\n;\n";
    
    for (const auto& j : g_jobs) {
        if (j.resultHash == 0 && j.sortPriority == 0) continue;
        string utf8path = fs::path(j.relPath).u8string();
        char hashbuf[17];
        snprintf(hashbuf, sizeof(hashbuf), "%016llx", j.resultHash);
        out << hashbuf << " *" << utf8path << "\n";
    }
    return out.good();
}

bool LoadTextArca(const fs::path& arcaPath) {
    ifstream in(arcaPath, ios::binary);
    if (!in) return false;
    
    char bom[3]; in.read(bom, 3);
    if (memcmp(bom, "\xEF\xBB\xBF", 3) != 0) in.seekg(0);
    
    string line;
    g_jobs.clear();

    while (getline(in, line)) {
        if (line.empty() || line[0] == ';') continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.length() < 18 || line[16] != ' ' || line[17] != '*') continue;
        
        string sHash = line.substr(0, 16);
        string utf8path = line.substr(18);
        
        // Remplacement try/catch par strtoull pour compatibilité -fno-exceptions
        char* end;
        uint64_t hash = strtoull(sHash.c_str(), &end, 16);
        
        // Si la conversion a échoué (pointeur n'a pas avancé), on ignore la ligne
        if (end == sHash.c_str()) continue;

        Job j;
        j.relPath = fs::u8path(utf8path).wstring();
        j.fullPath = arcaPath.parent_path() / fs::u8path(utf8path);
        j.expectedHash = hash;
        
        if (fs::exists(j.fullPath)) j.size = fs::file_size(j.fullPath);
        else j.size = 0;

        g_jobs.push_back(j);
    }
    return !g_jobs.empty();
}

/* ===================== ORCHESTRATOR ===================== */
void LogicManager(vector<wstring> inputs, bool verifyMode) {
    g_processing = true;
    g_stopRequested = false;
    g_processedBytes = 0;
    g_processedFiles = 0;

    PostMessage(g_hwndMain, WM_START, 0, 0);
    SetStatusSafe(L"Scanning...");

    // CORRECTION PORTÉE : outputPath déclaré ici pour être visible à la fin de la fonction
    fs::path outputPath; 

    // 1. Prepare Job List
    {
        lock_guard<mutex> lock(g_dataMutex);
        g_jobs.clear();
        
        if (verifyMode) {
            if (!LoadTextArca(inputs[0])) {
                SetStatusSafe(L"Error: Invalid .arca file");
                PostMessage(g_hwndMain, WM_DONE, 0, 0);
                return;
            }
        } else {
            for (auto& p : inputs) {
                if (fs::is_directory(p)) {
                    for (auto& e : fs::recursive_directory_iterator(p)) {
                        if (e.is_regular_file()) {
                            Job j; j.fullPath = e.path();
                            j.relPath = fs::relative(e.path(), fs::path(p).parent_path()).wstring();
                            j.size = fs::file_size(e.path());
                            g_jobs.push_back(j);
                        }
                    }
                    if (outputPath.empty()) outputPath = fs::path(p) / "Hash.arca";
                } else {
                    Job j; j.fullPath = p;
                    j.relPath = fs::path(p).filename().wstring();
                    j.size = fs::file_size(p);
                    g_jobs.push_back(j);
                    if (outputPath.empty()) outputPath = fs::path(p).parent_path() / "Hash.arca";
                }
            }
        }
    } 

    PostMessage(g_hwndMain, WM_REFRESH_LIST, g_jobs.size(), 0);

    // 2. Start Workers
    atomic<size_t> nextJob(0);
    vector<thread> workers;
    int threads = thread::hardware_concurrency();
    auto start = chrono::high_resolution_clock::now();

    for (int i = 0; i < threads; i++) 
        workers.emplace_back(GlobalWorker, &nextJob, verifyMode);

    // 3. Monitor Loop
    while (g_processedFiles < (int)g_jobs.size() && !g_stopRequested) {
        auto now = chrono::high_resolution_clock::now();
        double elapsed = chrono::duration<double>(now - start).count();
        double mbps = (g_processedBytes / 1024.0 / 1024.0) / (elapsed + 0.01);
        int pct = (int)((g_processedFiles * 100) / max(1, (int)g_jobs.size()));
        
        SetStatusSafe(to_wstring(pct) + L"% | " + to_wstring((int)mbps) + L" MB/s");
        PostMessage(g_hwndMain, WM_PROGRESS, pct, 0);
        InvalidateRect(g_hwndList, NULL, FALSE);
        
        this_thread::sleep_for(chrono::milliseconds(200));
    }

    for (auto& t : workers) if (t.joinable()) t.join();

    // 4. Final Processing
    int errCount = 0;
    int missingCount = 0;
    
    if (!g_stopRequested) {
        SetStatusSafe(L"Sorting results...");
        lock_guard<mutex> lock(g_dataMutex);
        
        for(const auto& j : g_jobs) {
            if (wcscmp(j.statusText, L"CORRUPT") == 0) errCount++;
            if (wcscmp(j.statusText, L"MISSING") == 0) missingCount++;
        }

        std::stable_sort(g_jobs.begin(), g_jobs.end(), [](const Job& a, const Job& b) {
            return a.sortPriority < b.sortPriority;
        });
    }

    PostMessage(g_hwndMain, WM_REFRESH_LIST, g_jobs.size(), 0);
    InvalidateRect(g_hwndList, NULL, TRUE);

    if (!verifyMode && !g_stopRequested) {
        // outputPath est maintenant accessible ici
        if (SaveTextArca(outputPath.empty() ? "Hash.arca" : outputPath))
            SetStatusSafe(L"Done | Saved: " + outputPath.filename().wstring());
        else 
            SetStatusSafe(L"Error saving file");
    } else {
        SetStatusSafe(g_stopRequested ? L"Stopped" : L"Done");
    }

    g_processing = false;
    PostMessage(g_hwndMain, WM_DONE, 0, 0);

    if (verifyMode && !g_stopRequested) {
        wstring msg = L"Scan Completed.\n\n";
        msg += L"Total Files: " + to_wstring(g_jobs.size()) + L"\n";
        if (errCount > 0 || missingCount > 0) {
            msg += L"❌ CORRUPT: " + to_wstring(errCount) + L"\n";
            msg += L"❌ MISSING: " + to_wstring(missingCount) + L"\n\n";
            msg += L"Files are sorted: Errors are at the top of the list.";
            MessageBox(g_hwndMain, msg.c_str(), L"Results - ATTENTION", MB_ICONWARNING);
        } else {
            msg += L"✅ All files OK.";
            MessageBox(g_hwndMain, msg.c_str(), L"Results", MB_ICONINFORMATION);
        }
    }
}

/* ===================== WINDOWS UI ===================== */
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        g_hwndProgress = CreateWindowEx(0, PROGRESS_CLASS, NULL, WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 0, 0, h, NULL, NULL, NULL);
        g_hwndStatus = CreateWindow(L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 0, 0, 0, h, NULL, NULL, NULL);
        g_hwndList = CreateWindowEx(WS_EX_CLIENTEDGE, WC_LISTVIEW, L"", 
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA | LVS_NOSORTHEADER | WS_VSCROLL, 
            0, 0, 0, 0, h, NULL, NULL, NULL);
            
        ListView_SetExtendedListViewStyle(g_hwndList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        {
            LVCOLUMN lvc{ LVCF_TEXT | LVCF_WIDTH };
            lvc.cx = 380; lvc.pszText = (LPWSTR)L"File"; ListView_InsertColumn(g_hwndList, 0, &lvc);
            lvc.cx = 100; lvc.pszText = (LPWSTR)L"Status"; ListView_InsertColumn(g_hwndList, 1, &lvc);
        }
        g_hwndCreate = CreateWindow(L"BUTTON", L"Create", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, h, (HMENU)1, NULL, NULL);
        g_hwndVerify = CreateWindow(L"BUTTON", L"Verify", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, h, (HMENU)2, NULL, NULL);
        g_hwndStop = CreateWindow(L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | WS_DISABLED, 0, 0, 0, 0, h, (HMENU)3, NULL, NULL);
        DragAcceptFiles(h, TRUE);
        break;

    case WM_SIZE: {
        int cx = LOWORD(l), cy = HIWORD(l);
        MoveWindow(g_hwndProgress, 5, 5, cx - 10, 15, 1);
        MoveWindow(g_hwndStatus, 5, 22, cx - 10, 18, 1);
        MoveWindow(g_hwndList, 5, 42, cx - 10, cy - 80, 1);
        MoveWindow(g_hwndCreate, 5, cy - 33, 100, 28, 1);
        MoveWindow(g_hwndVerify, 110, cy - 33, 100, 28, 1);
        MoveWindow(g_hwndStop, cx - 105, cy - 33, 100, 28, 1);
        break;
    }

    case WM_NOTIFY: {
        LPNMHDR hdr = (LPNMHDR)l;
        if (hdr->code == LVN_GETDISPINFO) {
            NMLVDISPINFO* plvdi = (NMLVDISPINFO*)l;
            int idx = plvdi->item.iItem;
            if (idx >= 0 && idx < (int)g_jobs.size()) {
                if (plvdi->item.mask & LVIF_TEXT) {
                    if (plvdi->item.iSubItem == 0) plvdi->item.pszText = (LPWSTR)g_jobs[idx].relPath.c_str();
                    else if (plvdi->item.iSubItem == 1) plvdi->item.pszText = g_jobs[idx].statusText;
                }
            }
            return 0;
        }
        if (hdr->code == NM_CUSTOMDRAW && hdr->hwndFrom == g_hwndList) {
            LPNMLVCUSTOMDRAW lplvcd = (LPNMLVCUSTOMDRAW)l;
            switch (lplvcd->nmcd.dwDrawStage) {
            case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT: return CDRF_NOTIFYSUBITEMDRAW;
            case CDDS_SUBITEM | CDDS_ITEMPREPAINT:
                int idx = (int)lplvcd->nmcd.dwItemSpec;
                if (idx >= 0 && idx < (int)g_jobs.size()) lplvcd->clrText = g_jobs[idx].color;
                return CDRF_DODEFAULT;
            }
        }
        break;
    }

    case WM_REFRESH_LIST:
        ListView_SetItemCountEx(g_hwndList, w, LVSICF_NOSCROLL);
        break;

    case WM_DROPFILES: {
        if (g_processing) break;
        HDROP hD = (HDROP)w;
        vector<wstring> paths;
        UINT cnt = DragQueryFile(hD, 0xFFFFFFFF, NULL, 0);
        bool isArca = false;
        for (UINT i = 0; i < cnt; i++) {
            wchar_t b[MAX_PATH]; DragQueryFile(hD, i, b, MAX_PATH);
            paths.push_back(b);
            if (fs::path(b).extension() == L".arca") isArca = true;
        }
        DragFinish(hD);
        thread(LogicManager, paths, isArca).detach();
        break;
    }

    case WM_START: 
        EnableWindow(g_hwndCreate, 0); EnableWindow(g_hwndVerify, 0); EnableWindow(g_hwndStop, 1);
        break;
        
    case WM_DONE: 
        EnableWindow(g_hwndCreate, 1); EnableWindow(g_hwndVerify, 1); EnableWindow(g_hwndStop, 0);
        break;
        
    case WM_STAT: { 
        wchar_t* s = (wchar_t*)l; SetWindowText(g_hwndStatus, s); free(s); 
        break; 
    }
    
    case WM_PROGRESS: 
        SendMessage(g_hwndProgress, PBM_SETPOS, w, 0); 
        break;
        
    case WM_COMMAND: 
        if (LOWORD(w) == 3) g_stopRequested = true; 
        break;
        
    case WM_DESTROY: 
        PostQuitMessage(0); 
        break;
        
    default: 
        return DefWindowProc(h, m, w, l);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE h, HINSTANCE, LPSTR, int n) {
    InitCommonControls();
    WNDCLASSEX wc{ sizeof(wc) }; 
    wc.lpfnWndProc = WndProc; 
    wc.hInstance = h; 
    wc.lpszClassName = L"ArcSFV";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); 
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassEx(&wc);
    g_hwndMain = CreateWindow(L"ArcSFV", L"ArcXSFV V1", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 600, 480, 0, 0, h, 0);
    ShowWindow(g_hwndMain, n);
    MSG m; 
    while (GetMessage(&m, 0, 0, 0)) { TranslateMessage(&m); DispatchMessage(&m); }
    return 0;
}