// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

#include "PreviewCache.h"
#include "AV1Settings.h"
#include "AV1Version.h"
#include "AV1Log.h"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

namespace av1imp {
namespace {

constexpr uint32_t kAepvMagic = 0x56504541; // 'AEPV' LE
constexpr uint32_t kAepvVersion = 1;
constexpr uint32_t kFormatBgra8 = 0;
constexpr size_t kQueueJobs = 32;
constexpr size_t kQueueBytes = 64u * 1024u * 1024u;
constexpr uint32_t kCleanupEveryWrites = 128;

#pragma pack(push, 1)
struct AepvHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t headerSize;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t rowBytes;
    uint32_t payloadSize;
    uint32_t crc32;
};
#pragma pack(pop)

static_assert(sizeof(AepvHeader) == 36, "AEPV header is a fixed 36-byte record");

const uint32_t kCrcTable[256] = {
    0x00000000,0x77073096,0xEE0E612C,0x990951BA,0x076DC419,0x706AF48F,0xE963A535,0x9E6495A3,
    0x0EDB8832,0x79DCB8A4,0xE0D5E91E,0x97D2D988,0x09B64C2B,0x7EB17CBD,0xE7B82D07,0x90BF1D91,
    0x1DB71064,0x6AB020F2,0xF3B97148,0x84BE41DE,0x1ADAD47D,0x6DDDE4EB,0xF4D4B551,0x83D385C7,
    0x136C9856,0x646BA8C0,0xFD62F97A,0x8A65C9EC,0x14015C4F,0x63066CD9,0xFA0F3D63,0x8D080DF5,
    0x3B6E20C8,0x4C69105E,0xD56041E4,0xA2677172,0x3C03E4D1,0x4B04D447,0xD20D85FD,0xA50AB56B,
    0x35B5A8FA,0x42B2986C,0xDBBBC9D6,0xACBCF940,0x32D86CE3,0x45DF5C75,0xDCD60DCF,0xABD13D59,
    0x26D930AC,0x51DE003A,0xC8D75180,0xBFD06116,0x21B4F4B5,0x56B3C423,0xCFBA9599,0xB8BDA50F,
    0x2802B89E,0x5F058808,0xC60CD9B2,0xB10BE924,0x2F6F7C87,0x58684C11,0xC1611DAB,0xB6662D3D,
    0x76DC4190,0x01DB7106,0x98D220BC,0xEFD5102A,0x71B18589,0x06B6B51F,0x9FBFE4A5,0xE8B8D433,
    0x7807C9A2,0x0F00F934,0x9609A88E,0xE10E9818,0x7F6A0DBB,0x086D3D2D,0x91646C97,0xE6635C01,
    0x6B6B51F4,0x1C6C6162,0x856530D8,0xF262004E,0x6C0695ED,0x1B01A57B,0x8208F4C1,0xF50FC457,
    0x65B0D9C6,0x12B7E950,0x8BBEB8EA,0xFCB9887C,0x62DD1DDF,0x15DA2D49,0x8CD37CF3,0xFBD44C65,
    0x4DB26158,0x3AB551CE,0xA3BC0074,0xD4BB30E2,0x4ADFA541,0x3DD895D7,0xA4D1C46D,0xD3D6F4FB,
    0x4369E96A,0x346ED9FC,0xAD678846,0xDA60B8D0,0x44042D73,0x33031DE5,0xAA0A4C5F,0xDD0D7CC9,
    0x5005713C,0x270241AA,0xBE0B1010,0xC90C2086,0x5768B525,0x206F85B3,0xB966D409,0xCE61E49F,
    0x5EDEF90E,0x29D9C998,0xB0D09822,0xC7D7A8B4,0x59B33D17,0x2EB40D81,0xB7BD5C3B,0xC0BA6CAD,
    0xEDB88320,0x9ABFB3B6,0x03B6E20C,0x74B1D29A,0xEAD54739,0x9DD277AF,0x04DB2615,0x73DC1683,
    0xE3630B12,0x94643B84,0x0D6D6A3E,0x7A6A5AA8,0xE40ECF0B,0x9309FF9D,0x0A00AE27,0x7D079EB1,
    0xF00F9344,0x8708A3D2,0x1E01F268,0x6906C2FE,0xF762575D,0x806567CB,0x196C3671,0x6E6B06E7,
    0xFED41B76,0x89D32BE0,0x10DA7A5A,0x67DD4ACC,0xF9B9DF6F,0x8EBEEFF9,0x17B7BE43,0x60B08ED5,
    0xD6D6A3E8,0xA1D1937E,0x38D8C2C4,0x4FDFF252,0xD1BB67F1,0xA6BC5767,0x3FB506DD,0x48B2364B,
    0xD80D2BDA,0xAF0A1B4C,0x36034AF6,0x41047A60,0xDF60EFC3,0xA867DF55,0x316E8EEF,0x4669BE79,
    0xCB61B38C,0xBC66831A,0x256FD2A0,0x5268E236,0xCC0C7795,0xBB0B4703,0x220216B9,0x5505262F,
    0xC5BA3BBE,0xB2BD0B28,0x2BB45A92,0x5CB36A04,0xC2D7FFA7,0xB5D0CF31,0x2CD99E8B,0x5BDEAE1D,
    0x9B64C2B0,0xEC63F226,0x756AA39C,0x026D930A,0x9C0906A9,0xEB0E363F,0x72076785,0x05005713,
    0x95BF4A82,0xE2B87A14,0x7BB12BAE,0x0CB61B38,0x92D28E9B,0xE5D5BE0D,0x7CDCEFB7,0x0BDBDF21,
    0x86D3D2D4,0xF1D4E242,0x68DDB3F8,0x1FDA836E,0x81BE16CD,0xF6B9265B,0x6FB077E1,0x18B74777,
    0x88085AE6,0xFF0F6A70,0x66063BCA,0x11010B5C,0x8F659EFF,0xF862AE69,0x616BFFD3,0x166CCF45,
    0xA00AE278,0xD70DD2EE,0x4E048354,0x3903B3C2,0xA7672661,0xD06016F7,0x4969474D,0x3E6E77DB,
    0xAED16A4A,0xD9D65ADC,0x40DF0B66,0x37D83BF0,0xA9BCAE53,0xDEBB9EC5,0x47B2CF7F,0x30B5FFE9,
    0xBDBDF21C,0xCABAC28A,0x53B39330,0x24B4A3A6,0xBAD03605,0xCDD70693,0x54DE5729,0x23D967BF,
    0xB3667A2E,0xC4614AB8,0x5D681B02,0x2A6F2B94,0xB40BBE37,0xC30C8EA1,0x5A05DF1B,0x2D02EF8D
};

uint32_t Crc32(const uint8_t* data, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc = kCrcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

bool Sha256(const void* data, size_t n, uint8_t out[32])
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        return false;
    }
    bool ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0 &&
              BCryptHashData(hash, (PUCHAR)data, (ULONG)n, 0) >= 0 &&
              BCryptFinishHash(hash, out, 32, 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

std::wstring Utf8ToWide(const std::string& utf8)
{
    if (utf8.empty()) return {};
    const int need = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (need <= 1) return {};
    std::wstring out((size_t)need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &out[0], need);
    out.pop_back();
    return out;
}

std::string WideToUtf8(const std::wstring& wide)
{
    if (wide.empty()) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};
    std::string out((size_t)need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &out[0], need, nullptr, nullptr);
    out.pop_back();
    return out;
}

std::wstring LocalAetherDir()
{
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        return {};
    }
    std::wstring dir = local;
    CoTaskMemFree(local);
    dir += L"\\Aether";
    return dir;
}

bool PayloadFits(int width, int height, size_t* bytes)
{
    if (width <= 0 || height <= 0) return false;
    const uint64_t n = (uint64_t)width * (uint64_t)height * 4ull;
    if (n == 0 || n > kPreviewCacheMaxPayload) return false;
    if (bytes) *bytes = (size_t)n;
    return true;
}

struct Job {
    std::array<uint8_t, 32> key{};
    int width = 0;
    int height = 0;
    std::vector<uint8_t> payload;
};

struct Engine {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Job> queue;
    size_t queuedBytes = 0;
    size_t activeJobs = 0;
    bool stop = false;
    bool started = false;
    std::thread worker;
    uint32_t writesSinceCleanup = 0;
    int64_t approxBytes = 0;
    std::unordered_set<std::string> touched;
    PreviewCacheProcessStats stats;
    std::string lastWarning;
};

Engine g_eng;

void Warn(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(g_eng.mutex);
    g_eng.lastWarning = msg;
    g_eng.stats.lastWarning = msg;
}

std::wstring CacheRoot()
{
    wchar_t env[2048] = {};
    if (GetEnvironmentVariableW(L"AETHER_PREVIEW_CACHE_DIR", env, 2048) > 0 && env[0]) {
        std::wstring dir = env;
        while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
        return dir;
    }
    return LocalAetherDir() + L"\\preview-cache";
}

std::wstring VersionDir()
{
    return CacheRoot() + L"\\v1";
}

std::string Hex(const uint8_t* p, size_t n)
{
    static const char* kDigits = "0123456789abcdef";
    std::string out(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        out[i * 2]     = kDigits[p[i] >> 4];
        out[i * 2 + 1] = kDigits[p[i] & 0xF];
    }
    return out;
}

std::wstring FilePathFor(const std::array<uint8_t, 32>& key)
{
    const std::string hex = Hex(key.data(), key.size());
    std::wstring dir = VersionDir();
    dir += L'\\';
    dir += (wchar_t)hex[0];
    dir += (wchar_t)hex[1];
    dir += L'\\';
    dir += (wchar_t)hex[2];
    dir += (wchar_t)hex[3];
    return dir + L'\\' + Utf8ToWide(hex) + L".aepv";
}

void EnsureParents(const std::wstring& file)
{
    const size_t slash = file.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    SHCreateDirectoryExW(nullptr, file.substr(0, slash).c_str(), nullptr);
}

struct FileRec {
    std::wstring path;
    uint64_t size = 0;
    FILETIME write{};
};

void CollectFiles(const std::wstring& dir, std::vector<FileRec>* out, uint64_t* bytes, uint64_t* files)
{
    WIN32_FIND_DATAW fd = {};
    const std::wstring pattern = dir + L"\\*";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) {
            continue;
        }
        const std::wstring path = dir + L'\\' + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CollectFiles(path, out, bytes, files);
            continue;
        }
        const bool aepv = wcsstr(fd.cFileName, L".aepv") != nullptr;
        const bool tmp  = wcsstr(fd.cFileName, L".tmp") != nullptr;
        if (!aepv && !tmp) continue;

        ULARGE_INTEGER sz;
        sz.LowPart = fd.nFileSizeLow;
        sz.HighPart = fd.nFileSizeHigh;
        if (out) {
            FileRec rec;
            rec.path = path;
            rec.size = sz.QuadPart;
            rec.write = fd.ftLastWriteTime;
            out->push_back(rec);
        }
        if (bytes) *bytes += sz.QuadPart;
        if (files && aepv) ++*files;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void DeleteTree(const std::wstring& dir)
{
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(dir.c_str());
        return;
    }
    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) {
            continue;
        }
        const std::wstring path = dir + L'\\' + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(path);
        else DeleteFileW(path.c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    RemoveDirectoryW(dir.c_str());
}

void Cleanup(uint64_t limitBytes)
{
    std::vector<FileRec> files;
    uint64_t bytes = 0, count = 0;
    CollectFiles(VersionDir(), &files, &bytes, &count);

    const uint64_t now = [] {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER u;
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return u.QuadPart;
    }();
    const uint64_t day = 864000000000ull; // 100ns

    for (const FileRec& f : files) {
        const bool tmp = f.path.size() >= 4 &&
                         _wcsicmp(f.path.c_str() + f.path.size() - 4, L".tmp") == 0;
        if (!tmp) continue;
        ULARGE_INTEGER w;
        w.LowPart = f.write.dwLowDateTime;
        w.HighPart = f.write.dwHighDateTime;
        if (now > w.QuadPart && now - w.QuadPart > day) {
            DeleteFileW(f.path.c_str());
        }
    }

    files.clear();
    bytes = 0;
    count = 0;
    CollectFiles(VersionDir(), &files, &bytes, &count);
    {
        std::lock_guard<std::mutex> lock(g_eng.mutex);
        g_eng.approxBytes = (int64_t)bytes;
    }

    if (bytes <= limitBytes) return;

    const uint64_t target = (limitBytes / 10ull) * 9ull;
    std::sort(files.begin(), files.end(), [](const FileRec& a, const FileRec& b) {
        return CompareFileTime(&a.write, &b.write) < 0;
    });
    for (const FileRec& f : files) {
        if (bytes <= target) break;
        if (DeleteFileW(f.path.c_str())) bytes -= f.size;
    }
    {
        std::lock_guard<std::mutex> lock(g_eng.mutex);
        g_eng.approxBytes = (int64_t)bytes;
    }
}

bool WriteAtomic(const std::wstring& finalPath, const AepvHeader& hdr, const uint8_t* payload)
{
    EnsureParents(finalPath);
    wchar_t temp[4096];
    swprintf_s(temp, L"%s.%lu.%lu.%llu.tmp",
               finalPath.c_str(),
               GetCurrentProcessId(),
               GetCurrentThreadId(),
               (unsigned long long)GetTickCount64());

    HANDLE h = CreateFileW(temp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Warn("preview cache write failed");
        return false;
    }
    DWORD written = 0;
    const BOOL okHdr = WriteFile(h, &hdr, sizeof(hdr), &written, nullptr) && written == sizeof(hdr);
    const BOOL okPay = okHdr && WriteFile(h, payload, hdr.payloadSize, &written, nullptr) &&
                       written == hdr.payloadSize;
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!okPay) {
        DeleteFileW(temp);
        Warn("preview cache write failed");
        return false;
    }
    if (!MoveFileExW(temp, finalPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp);
        Warn("preview cache publish failed");
        return false;
    }
    return true;
}

void WorkerLoop()
{
    try {
        const uint64_t limit =
            (uint64_t)CurrentSettings().previewCacheMB * 1024ull * 1024ull;
        Cleanup(limit);
    } catch (...) {
        Warn("preview cache startup cleanup failed");
    }
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(g_eng.mutex);
            g_eng.cv.wait(lock, [] { return g_eng.stop || !g_eng.queue.empty(); });
            if (g_eng.stop && g_eng.queue.empty()) return;
            if (g_eng.queue.empty()) continue;
            job = std::move(g_eng.queue.front());
            g_eng.queue.pop_front();
            g_eng.queuedBytes -= job.payload.size();
            ++g_eng.activeJobs;
        }

        AepvHeader hdr{};
        hdr.magic = kAepvMagic;
        hdr.version = kAepvVersion;
        hdr.headerSize = sizeof(AepvHeader);
        hdr.width = (uint32_t)job.width;
        hdr.height = (uint32_t)job.height;
        hdr.format = kFormatBgra8;
        hdr.rowBytes = (uint32_t)job.width * 4u;
        hdr.payloadSize = (uint32_t)job.payload.size();
        hdr.crc32 = Crc32(job.payload.data(), job.payload.size());

        const std::wstring path = FilePathFor(job.key);
        if (WriteAtomic(path, hdr, job.payload.data())) {
            bool cleanup = false;
            uint64_t limit = 0;
            {
                std::lock_guard<std::mutex> lock(g_eng.mutex);
                g_eng.approxBytes += (int64_t)(sizeof(AepvHeader) + job.payload.size());
                ++g_eng.writesSinceCleanup;
                limit = (uint64_t)CurrentSettings().previewCacheMB * 1024ull * 1024ull;
                cleanup = g_eng.writesSinceCleanup >= kCleanupEveryWrites ||
                          g_eng.approxBytes > (int64_t)limit;
                if (cleanup) g_eng.writesSinceCleanup = 0;
            }
            if (cleanup) {
                try {
                    Cleanup(limit);
                } catch (...) {
                    Warn("preview cache cleanup failed");
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_eng.mutex);
            if (g_eng.activeJobs) --g_eng.activeJobs;
        }
        g_eng.cv.notify_all();
    }
}

void StartWorker()
{
    if (g_eng.started) return;
    g_eng.started = true;
    g_eng.stop = false;
    g_eng.worker = std::thread(WorkerLoop);
}

bool ReadFileAll(const std::wstring& path, std::vector<uint8_t>* out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < (LONGLONG)sizeof(AepvHeader) ||
        sz.QuadPart > (LONGLONG)(sizeof(AepvHeader) + kPreviewCacheMaxPayload)) {
        CloseHandle(h);
        return false;
    }
    out->resize((size_t)sz.QuadPart);
    DWORD got = 0;
    const BOOL ok = ReadFile(h, out->data(), (DWORD)out->size(), &got, nullptr) &&
                    got == out->size();
    CloseHandle(h);
    return ok != 0;
}

void MaybeTouch(const std::wstring& path, const std::string& hex)
{
    std::lock_guard<std::mutex> lock(g_eng.mutex);
    if (g_eng.touched.count(hex)) return;
    g_eng.touched.insert(hex);
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFileTime(h, nullptr, nullptr, &now);
    CloseHandle(h);
}

} // namespace

void CopyBgraWithStride(const uint8_t* src, int srcStride,
                        uint8_t* dst, int dstStride,
                        int width, int height)
{
    if (!src || !dst || width <= 0 || height <= 0) return;
    const int row = width * 4;
    for (int y = 0; y < height; ++y) {
        const uint8_t* s = src + (ptrdiff_t)y * srcStride;
        uint8_t* d = dst + (ptrdiff_t)y * dstStride;
        memcpy(d, s, (size_t)row);
    }
}

SourceFingerprint FingerprintSource(const std::string& utf8Path)
{
    SourceFingerprint fp;
    const std::wstring wide = Utf8ToWide(utf8Path);
    if (wide.empty()) return fp;

    wchar_t full[4096] = {};
    const DWORD n = GetFullPathNameW(wide.c_str(), 4096, full, nullptr);
    if (n == 0 || n >= 4096) wcsncpy_s(full, wide.c_str(), _TRUNCATE);

    HANDLE file = CreateFileW(full, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return fp;

    BY_HANDLE_FILE_INFORMATION info{};
    const bool haveInfo = GetFileInformationByHandle(file, &info) != 0;
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);

    std::vector<uint8_t> head, mid, tail;
    const DWORD kSample = 256u * 1024u;
    auto readChunk = [&](int64_t offset, DWORD want, std::vector<uint8_t>* out) {
        LARGE_INTEGER at;
        at.QuadPart = offset;
        if (!SetFilePointerEx(file, at, nullptr, FILE_BEGIN)) {
            out->clear();
            return;
        }
        out->assign(want, 0);
        DWORD got = 0;
        if (!ReadFile(file, out->data(), want, &got, nullptr)) {
            out->clear();
            return;
        }
        out->resize(got);
    };

    readChunk(0, kSample, &head);
    if (size.QuadPart > static_cast<LONGLONG>(kSample) * 2) {
        const int64_t midAt = (size.QuadPart - kSample) / 2;
        readChunk(midAt, kSample, &mid);
        readChunk(size.QuadPart - kSample, kSample, &tail);
    } else if (size.QuadPart > kSample) {
        readChunk(size.QuadPart - kSample, kSample, &tail);
    }
    CloseHandle(file);

    std::vector<uint8_t> blob;
    auto addU32 = [&](uint32_t v) {
        blob.insert(blob.end(), (uint8_t*)&v, (uint8_t*)&v + 4);
    };
    auto addU64 = [&](uint64_t v) {
        blob.insert(blob.end(), (uint8_t*)&v, (uint8_t*)&v + 8);
    };
    addU32(kPreviewCacheKeySchema);
    wchar_t normalized[4096] = {};
    wcsncpy_s(normalized, full, _TRUNCATE);
    for (wchar_t* p = normalized; *p; ++p) *p = (wchar_t)towlower(*p);
    const size_t pathBytes = wcslen(normalized) * sizeof(wchar_t);
    addU32((uint32_t)pathBytes);
    blob.insert(blob.end(), (uint8_t*)normalized, (uint8_t*)normalized + pathBytes);
    addU64((uint64_t)size.QuadPart);
    if (haveInfo) {
        addU64(((uint64_t)info.ftLastWriteTime.dwHighDateTime << 32) |
               info.ftLastWriteTime.dwLowDateTime);
        addU32(info.dwVolumeSerialNumber);
        addU32(info.nFileIndexHigh);
        addU32(info.nFileIndexLow);
    }
    uint8_t sparse[32] = {};
    std::vector<uint8_t> sample = head;
    sample.insert(sample.end(), mid.begin(), mid.end());
    sample.insert(sample.end(), tail.begin(), tail.end());
    Sha256(sample.data(), sample.size(), sparse);
    blob.insert(blob.end(), sparse, sparse + 32);

    if (!Sha256(blob.data(), blob.size(), fp.hash.data())) return fp;
    fp.valid = true;
    return fp;
}

std::array<uint8_t, 32> MakePreviewKey(const PreviewKeyInput& in)
{
    std::array<uint8_t, 32> out{};
    std::vector<uint8_t> blob;
    auto addU32 = [&](uint32_t v) {
        blob.insert(blob.end(), (uint8_t*)&v, (uint8_t*)&v + 4);
    };
    auto addU64 = [&](uint64_t v) {
        blob.insert(blob.end(), (uint8_t*)&v, (uint8_t*)&v + 8);
    };
    auto addStr = [&](const std::string& s) {
        addU32((uint32_t)s.size());
        blob.insert(blob.end(), s.begin(), s.end());
    };

    addU32(kPreviewCacheKeySchema);
    addStr(in.aetherVersion.empty() ? AETHER_VERSION_STR : in.aetherVersion);
    addStr(in.ffmpegVersion);
    blob.insert(blob.end(), in.source.hash.begin(), in.source.hash.end());
    addU32((uint32_t)in.videoStream);
    addU64((uint64_t)in.frameIndex);
    addU32((uint32_t)in.width);
    addU32((uint32_t)in.height);
    addU32(kFormatBgra8);
    addU32(in.fastScaling ? 1u : 0u);
    addU32(in.hardware ? 1u : 0u);
    addStr(in.decoderName);
    Sha256(blob.data(), blob.size(), out.data());
    return out;
}

std::string PreviewKeyHex(const std::array<uint8_t, 32>& key)
{
    return Hex(key.data(), key.size());
}

PreviewCache& PreviewCache::Instance()
{
    static PreviewCache cache;
    return cache;
}

void PreviewCache::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(g_eng.mutex);
        g_eng.stop = true;
        g_eng.queue.clear();
        g_eng.queuedBytes = 0;
    }
    g_eng.cv.notify_all();
    if (g_eng.worker.joinable()) g_eng.worker.join();
    g_eng.started = false;
}

bool PreviewCache::TryRead(const std::array<uint8_t, 32>& key,
                           int width, int height,
                           uint8_t* dst, int dstStride)
{
    size_t want = 0;
    if (!dst || !PayloadFits(width, height, &want)) return false;

    const auto t0 = std::chrono::steady_clock::now();
    const std::wstring path = FilePathFor(key);
    std::vector<uint8_t> file;
    if (!ReadFileAll(path, &file) || file.size() < sizeof(AepvHeader)) {
        std::lock_guard<std::mutex> lock(g_eng.mutex);
        ++g_eng.stats.misses;
        return false;
    }

    AepvHeader hdr{};
    memcpy(&hdr, file.data(), sizeof(hdr));
    const bool bad =
        hdr.magic != kAepvMagic ||
        hdr.version != kAepvVersion ||
        hdr.headerSize != sizeof(AepvHeader) ||
        hdr.width != (uint32_t)width ||
        hdr.height != (uint32_t)height ||
        hdr.format != kFormatBgra8 ||
        hdr.rowBytes != (uint32_t)width * 4u ||
        hdr.payloadSize != want ||
        file.size() != sizeof(AepvHeader) + want ||
        Crc32(file.data() + sizeof(AepvHeader), want) != hdr.crc32;

    if (bad) {
        DeleteFileW(path.c_str());
        std::lock_guard<std::mutex> lock(g_eng.mutex);
        ++g_eng.stats.misses;
        return false;
    }

    CopyBgraWithStride(file.data() + sizeof(AepvHeader), width * 4,
                       dst, dstStride, width, height);
    MaybeTouch(path, Hex(key.data(), key.size()));

    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    std::lock_guard<std::mutex> lock(g_eng.mutex);
    ++g_eng.stats.hits;
    g_eng.stats.readMs += ms;
    return true;
}

bool PreviewCache::QueueWrite(const std::array<uint8_t, 32>& key,
                              int width, int height,
                              const uint8_t* src, int srcStride)
{
    size_t want = 0;
    if (!src || !PayloadFits(width, height, &want)) return false;
    if (!CurrentSettings().previewCache) return false;

    Job job;
    job.key = key;
    job.width = width;
    job.height = height;
    job.payload.resize(want);
    CopyBgraWithStride(src, srcStride, job.payload.data(), width * 4, width, height);

    {
        std::lock_guard<std::mutex> lock(g_eng.mutex);
        if (g_eng.stop) return false;
        if (g_eng.queue.size() >= kQueueJobs || g_eng.queuedBytes + want > kQueueBytes) {
            ++g_eng.stats.writesDropped;
            return false;
        }
        g_eng.queuedBytes += want;
        g_eng.queue.push_back(std::move(job));
        ++g_eng.stats.writesQueued;
        StartWorker();
    }
    g_eng.cv.notify_one();
    return true;
}

PreviewCacheUsage PreviewCache::Usage() const
{
    PreviewCacheUsage u;
    u.directory = VersionDir();
    CollectFiles(u.directory, nullptr, &u.bytes, &u.files);
    return u;
}

bool PreviewCache::Clear()
{
    {
        std::lock_guard<std::mutex> lock(g_eng.mutex);
        g_eng.queue.clear();
        g_eng.queuedBytes = 0;
        g_eng.approxBytes = 0;
        g_eng.touched.clear();
    }
    DeleteTree(VersionDir());
    return true;
}

PreviewCacheProcessStats PreviewCache::ProcessStats() const
{
    std::lock_guard<std::mutex> lock(g_eng.mutex);
    PreviewCacheProcessStats s = g_eng.stats;
    s.lastWarning = g_eng.lastWarning;
    return s;
}

std::wstring PreviewCache::Directory() const
{
    return VersionDir();
}

std::string PreviewCache::Json() const
{
    const Settings set = CurrentSettings();
    const PreviewCacheUsage u = Usage();
    const PreviewCacheProcessStats st = ProcessStats();
    std::string json = "{";
    json += "\"enabled\":";
    json += set.previewCache ? "true" : "false";
    json += ",\"limit_mb\":";
    json += std::to_string(set.previewCacheMB);
    json += ",\"bytes\":";
    json += std::to_string(u.bytes);
    json += ",\"files\":";
    json += std::to_string(u.files);
    json += ",\"hits\":";
    json += std::to_string(st.hits);
    json += ",\"misses\":";
    json += std::to_string(st.misses);
    json += ",\"writes_queued\":";
    json += std::to_string(st.writesQueued);
    json += ",\"writes_dropped\":";
    json += std::to_string(st.writesDropped);
    json += ",\"directory\":\"";
    const std::string dir = WideToUtf8(u.directory);
    for (unsigned char c : dir) {
        if (c == '\\' || c == '"') json += '\\';
        json += (char)c;
    }
    json += "\"}";
    return json;
}

void PreviewCache::FlushForTests()
{
    std::unique_lock<std::mutex> lock(g_eng.mutex);
    g_eng.cv.wait(lock, [] {
        return g_eng.queue.empty() && g_eng.activeJobs == 0;
    });
}

void PreviewCache::CleanupNowForTests()
{
    const uint64_t limit = (uint64_t)CurrentSettings().previewCacheMB * 1024ull * 1024ull;
    Cleanup(limit);
}

} // namespace av1imp
