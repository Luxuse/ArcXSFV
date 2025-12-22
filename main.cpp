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
#include <queue>

#include "arcahash.h" 

#pragma comment(lib, "comctl32.lib")

using namespace std;
namespace fs = std::filesystem;

/* ===================== CONSTANTS & MSGS ===================== */
#define CLR_OK      RGB(0,180,0)
#define CLR_BAD     RGB(220,0,0)
#define CLR_WARN    RGB(255,140,0)
#define CLR_PROC    RGB(0,100,255)

#define WM_LOG      (WM_APP + 1)
#define WM_STAT     (WM_APP + 2)
#define WM_START    (WM_APP + 3)
#define WM_PROGRESS (WM_APP + 4)
#define WM_DONE     (WM_APP + 5)
#define WM_BATCH_LOG (WM_APP + 6)

/* ===================== GLOBALS ===================== */
HWND g_hwndMain, g_hwndProgress, g_hwndStatus, g_hwndList;
HWND g_hwndCreate, g_hwndVerify, g_hwndStop;

atomic<bool> g_stopRequested(false);
atomic<bool> g_processing(false);
atomic<uint64_t> g_processedBytes(0);
atomic<int> g_processedFiles(0);

struct LogMsg { 
    int idx; 
    COLORREF col; 
    wchar_t text[260]; 
    wchar_t status[64]; 
};

// Batching mechanism for UI updates
mutex g_batchMutex;
vector<LogMsg*> g_batchQueue;
UINT_PTR g_batchTimer = 0;

/* ===================== UI HELPERS ===================== */
void UpdateListSafe(int idx, const wstring& status, COLORREF col) {
    LogMsg* m = new LogMsg();
    m->idx = idx;
    m->col = col;
    wcsncpy_s(m->status, status.c_str(), _TRUNCATE);
    
    // Add to batch queue instead of immediate update
    {
        lock_guard<mutex> lock(g_batchMutex);
        g_batchQueue.push_back(m);
    }
}

void AddListItemSafe(const wstring& text, const wstring& status) {
    LogMsg* m = new LogMsg();
    m->idx = -1; 
    m->col = RGB(0, 0, 0);
    wcsncpy_s(m->text, text.c_str(), _TRUNCATE);
    wcsncpy_s(m->status, status.c_str(), _TRUNCATE);
    
    // Add to batch queue
    {
        lock_guard<mutex> lock(g_batchMutex);
        g_batchQueue.push_back(m);
    }
}

void SetStatusSafe(const wstring& s) {
    wchar_t* b = _wcsdup(s.c_str());
    PostMessage(g_hwndMain, WM_STAT, 0, (LPARAM)b);
}

void ProcessBatchUpdates() {
    vector<LogMsg*> batch;
    {
        lock_guard<mutex> lock(g_batchMutex);
        if (g_batchQueue.empty()) return;
        batch.swap(g_batchQueue);
    }
    
    // Disable redraw during batch update
    SendMessage(g_hwndList, WM_SETREDRAW, FALSE, 0);
    
    for (auto* m : batch) {
        if (m->idx == -1) {
            // Add new item
            LVITEM lv{ LVIF_TEXT | LVIF_PARAM };
            lv.iItem = ListView_GetItemCount(g_hwndList);
            lv.pszText = m->text; 
            lv.lParam = (LPARAM)m->col;
            int pos = ListView_InsertItem(g_hwndList, &lv);
            ListView_SetItemText(g_hwndList, pos, 1, m->status);
        } else {
            // Update existing item
            LVITEM lv{ LVIF_PARAM };
            lv.iItem = m->idx; 
            lv.lParam = (LPARAM)m->col;
            ListView_SetItem(g_hwndList, &lv);
            ListView_SetItemText(g_hwndList, m->idx, 1, m->status);
        }
        delete m;
    }
    
    // Re-enable redraw and update
    SendMessage(g_hwndList, WM_SETREDRAW, TRUE, 0);
    
    // Only scroll to last updated item, not every item
    int count = ListView_GetItemCount(g_hwndList);
    if (count > 0) {
        ListView_EnsureVisible(g_hwndList, count - 1, FALSE);
    }
    
    InvalidateRect(g_hwndList, NULL, TRUE);
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
struct Job {
    fs::path path;
    int listIdx;
    uint64_t size;
    uint64_t expectedHash;
    uint64_t resultHash;
    wstring relPath;
};

void GlobalWorker(vector<Job>* jobs, atomic<size_t>* nextJob, bool verifyMode) {
    while (!g_stopRequested) {
        size_t idx = nextJob->fetch_add(1);
        if (idx >= jobs->size()) break;

        Job& job = (*jobs)[idx];
        UpdateListSafe(job.listIdx, L"Hashing...", CLR_PROC);

        job.resultHash = FastHashFile(job.path);

        if (verifyMode) {
            bool ok = (job.resultHash == job.expectedHash);
            UpdateListSafe(job.listIdx, ok ? L"OK" : L"CORRUPT", ok ? CLR_OK : CLR_BAD);
        } else {
            UpdateListSafe(job.listIdx, L"Done", CLR_OK);
        }
        g_processedFiles++;
    }
}

/* ===================== ORCHESTRATOR ===================== */
void LogicManager(vector<wstring> inputs, bool verifyMode) {
    g_processing = true;
    g_stopRequested = false;
    g_processedBytes = 0;
    g_processedFiles = 0;

    PostMessage(g_hwndMain, WM_START, 0, 0);
    SetStatusSafe(L"Scanning...");

    vector<Job> jobList;
    if (verifyMode) {
        fs::path arcaPath = inputs[0];
        ifstream in(arcaPath, ios::binary);
        char magic[4]; in.read(magic, 4);
        if (memcmp(magic, "ARCA", 4) != 0) { 
            SetStatusSafe(L"Error: Invalid File"); 
            PostMessage(g_hwndMain, WM_DONE, 0, 0); 
            return; 
        }
        uint32_t ver, count; in.read((char*)&ver, 4); in.read((char*)&count, 4);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t l; in.read((char*)&l, 4);
            string p(l, '\0'); in.read(&p[0], l);
            uint64_t sz, h; in.read((char*)&sz, 8); in.read((char*)&h, 8);
            Job j; j.path = arcaPath.parent_path() / fs::u8path(p);
            j.expectedHash = h; j.listIdx = i; j.size = sz; j.relPath = fs::u8path(p).wstring();
            jobList.push_back(j);
        }
    } else {
        int idx = 0;
        for (auto& p : inputs) {
            if (fs::is_directory(p)) {
                for (auto& e : fs::recursive_directory_iterator(p)) {
                    if (e.is_regular_file()) {
                        Job j; j.path = e.path(); j.listIdx = idx++;
                        j.size = fs::file_size(e.path());
                        j.relPath = fs::relative(e.path(), fs::path(p).parent_path()).wstring();
                        jobList.push_back(j);
                    }
                }
            } else {
                Job j; j.path = p; j.listIdx = idx++;
                j.size = fs::file_size(p);
                j.relPath = fs::path(p).filename().wstring();
                jobList.push_back(j);
            }
        }
    }

    // Add all items to list in batches
    for (auto& j : jobList) {
        AddListItemSafe(j.relPath, L"Queued");
    }

    atomic<size_t> nextJob(0);
    vector<thread> workers;
    auto start = chrono::high_resolution_clock::now();
    int threads = thread::hardware_concurrency();

    for (int i = 0; i < threads; i++) 
        workers.emplace_back(GlobalWorker, &jobList, &nextJob, verifyMode);

    while (g_processedFiles < (int)jobList.size() && !g_stopRequested) {
        auto now = chrono::high_resolution_clock::now();
        double elapsed = chrono::duration<double>(now - start).count();
        double mbps = (g_processedBytes / 1024.0 / 1024.0) / (elapsed + 0.01);
        int pct = (int)((g_processedFiles * 100) / max(1, (int)jobList.size()));
        
        SetStatusSafe(to_wstring(pct) + L"% | " + to_wstring((int)mbps) + L" MB/s");
        PostMessage(g_hwndMain, WM_PROGRESS, pct, 0);
        
        // Trigger batch processing
        PostMessage(g_hwndMain, WM_BATCH_LOG, 0, 0);
        
        this_thread::sleep_for(chrono::milliseconds(150));
    }

    for (auto& t : workers) if (t.joinable()) t.join();

    // Final batch update
    PostMessage(g_hwndMain, WM_BATCH_LOG, 0, 0);

    if (!verifyMode && !g_stopRequested) {
        ofstream o("Hash.arca", ios::binary);
        uint32_t v = 1, c = (uint32_t)jobList.size();
        o.write("ARCA", 4); o.write((char*)&v, 4); o.write((char*)&c, 4);
        for (auto& j : jobList) {
            string u8 = fs::path(j.relPath).u8string();
            uint32_t l = (uint32_t)u8.size();
            o.write((char*)&l, 4); o.write(u8.data(), l);
            o.write((char*)&j.size, 8); o.write((char*)&j.resultHash, 8);
        }
    }

    SetStatusSafe(g_stopRequested ? L"Stopped" : L"100% - Done");
    PostMessage(g_hwndMain, WM_DONE, 0, 0);
    g_processing = false;
}

/* ===================== WINDOWS UI ===================== */
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        g_hwndProgress = CreateWindowEx(0, PROGRESS_CLASS, NULL, WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 0, 0, h, NULL, NULL, NULL);
        g_hwndStatus = CreateWindow(L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 0, 0, 0, h, NULL, NULL, NULL);
        g_hwndList = CreateWindowEx(WS_EX_CLIENTEDGE, WC_LISTVIEW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDRAWFIXED | WS_VSCROLL, 0, 0, 0, 0, h, NULL, NULL, NULL);
        ListView_SetExtendedListViewStyle(g_hwndList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        {
            LVCOLUMN lvc{ LVCF_TEXT | LVCF_WIDTH };
            lvc.cx = 380; lvc.pszText = (LPWSTR)L"File"; ListView_InsertColumn(g_hwndList, 0, &lvc);
            lvc.cx = 90; lvc.pszText = (LPWSTR)L"Status"; ListView_InsertColumn(g_hwndList, 1, &lvc);
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

    case WM_BATCH_LOG:
        ProcessBatchUpdates();
        break;

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT d = (LPDRAWITEMSTRUCT)l;
        if (d->CtlType == ODT_LISTVIEW) {
            LVITEM lv{ LVIF_PARAM, (int)d->itemID };
            ListView_GetItem(g_hwndList, &lv);
            COLORREF txtCol = (COLORREF)lv.lParam;
            FillRect(d->hDC, &d->rcItem, GetSysColorBrush((d->itemState & ODS_SELECTED) ? COLOR_HIGHLIGHT : COLOR_WINDOW));
            SetTextColor(d->hDC, (d->itemState & ODS_SELECTED) ? GetSysColor(COLOR_HIGHLIGHTTEXT) : txtCol);
            SetBkMode(d->hDC, TRANSPARENT);
            wchar_t buf[260];
            ListView_GetItemText(g_hwndList, d->itemID, 0, buf, 260);
            RECT r = d->rcItem; r.left += 4; DrawText(d->hDC, buf, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
            ListView_GetItemText(g_hwndList, d->itemID, 1, buf, 64);
            r = d->rcItem; r.left = r.right - 90; DrawText(d->hDC, buf, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }
        break;
    }

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
        EnableWindow(g_hwndCreate, 0); 
        EnableWindow(g_hwndVerify, 0); 
        EnableWindow(g_hwndStop, 1);
        ListView_DeleteAllItems(g_hwndList); 
        break;
        
    case WM_DONE: 
        EnableWindow(g_hwndCreate, 1); 
        EnableWindow(g_hwndVerify, 1); 
        EnableWindow(g_hwndStop, 0);
        ProcessBatchUpdates(); // Final update
        break;
        
    case WM_STAT: { 
        wchar_t* s = (wchar_t*)l; 
        SetWindowText(g_hwndStatus, s); 
        free(s); 
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
    g_hwndMain = CreateWindow(L"ArcSFV", L"ArcXSFV V1.0", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 600, 480, 0, 0, h, 0);
    ShowWindow(g_hwndMain, n);
    MSG m; 
    while (GetMessage(&m, 0, 0, 0)) { 
        TranslateMessage(&m); 
        DispatchMessage(&m); 
    }
    return 0;
}