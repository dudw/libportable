#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <io.h>
#include <process.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include "general.h"
#include "ini_parser.h"
#include "lz4.h"
#include "cjson.h"
#include "json_paser.h"

#ifdef _MSC_VER
#include <stdarg.h>
#endif

#if defined _MSC_VER && _MSC_VER > 1500 && !defined(__clang__)
#pragma intrinsic(__cpuid, _xgetbv)
#elif defined(__GNUC__)
extern void __cpuid(int CPUInfo[4], int info_type);
#else
#endif

#define CPUID(func, a, b, c, d) do {\
    int regs[4];\
    __cpuid(regs, func); \
    *a = regs[0]; *b = regs[1];  *c = regs[2];  *d = regs[3];\
  } while(0)

#define MAX_MESSAGE 1024
#define MAX_ALLSECTIONS 640
#define SECTION_NAMES 32
#define MAX_SECTION 16

typedef DWORD(WINAPI *PFNGFVSW)(LPCWSTR, LPDWORD);
typedef DWORD(WINAPI *PFNGFVIW)(LPCWSTR, DWORD, DWORD, LPVOID);
typedef bool(WINAPI *PFNVQVW)(LPCVOID, LPCWSTR, LPVOID, PUINT);

typedef struct _LANGANDCODEPAGE
{
    uint16_t wLanguage;
    uint16_t wCodePage;
} LANGANDCODEPAGE;

static PFNGFVSW pfnGetFileVersionInfoSizeW;
static PFNGFVIW pfnGetFileVersionInfoW;
static PFNVQVW pfnVerQueryValueW;
char ini_portable_path[MAX_PATH + 1] = {0};
WCHAR xre_profile_path[MAX_BUFF] = {0};
WCHAR xre_profile_local_path[MAX_BUFF] = {0};
LoadLibraryExPtr sLoadLibraryExStub = NULL;

/* we need link to the old msvcrt sometimes, so ... */
#if defined(_MSC_VER) && !defined(VC12_CRT)

sprintf_ptr   crt_sprintf   = NULL;
snprintf_ptr  crt_snprintf  = NULL;
sscanf_ptr    crt_sscanf    = NULL;
strtoui64_ptr crt_strtoui64 = NULL;

bool WINAPI
init_crt_funcs(void)
{
    HMODULE h_crt = GetModuleHandleW(L"msvcrt.dll");
    if (h_crt)
    {
        crt_sprintf = (sprintf_ptr)GetProcAddress(h_crt, "sprintf");
        crt_snprintf = (snprintf_ptr)GetProcAddress(h_crt, "_snprintf");
        crt_sscanf = (sscanf_ptr)GetProcAddress(h_crt, "sscanf");
        crt_strtoui64 = (strtoui64_ptr)GetProcAddress(h_crt, "_strtoui64");
    }
    if (crt_sprintf&&crt_sscanf&&crt_sscanf&&crt_strtoui64)
    {
        return true;
    }
    return false;
}

#endif

#ifdef _LOGDEBUG
#define LOG_FILE L"run_hook.log"
static WCHAR logfile_buf[MAX_PATH + 1];

void WINAPI
init_logs(void)
{
    if (*logfile_buf == '\0' && GetEnvironmentVariableW(L"APPDATA", logfile_buf, MAX_PATH) > 0)
    {
        wcsncat(logfile_buf, L"\\", MAX_PATH);
        wcsncat(logfile_buf, LOG_FILE, MAX_PATH);
    }
}

void __cdecl
logmsg(const char *format, ...)
{
    va_list args;
    char buffer[MAX_MESSAGE];
    va_start(args, format);
    if (wcslen(logfile_buf) > 0)
    {
        FILE *pFile = NULL;
        int len = wvnsprintfA(buffer, MAX_MESSAGE, format, args);
        if (len > 0 && len < MAX_MESSAGE)
        {
            buffer[len] = '\0';
            if ((pFile = _wfopen(logfile_buf, L"a+")) != NULL)
            {
                fwrite(buffer, strlen(buffer), 1, pFile);
                fclose(pFile);
            }
        }
    }
    va_end(args);
    return;
}
#endif

static bool
ini_path_init(void)
{
    bool  ret = false;
    WCHAR ini_path[MAX_PATH + 1] = {0};
    if (*ini_portable_path != '\0' && strlen(ini_portable_path) > 10)
    {
        return true;
    }
    if (!GetModuleFileNameW(dll_module, ini_path, MAX_PATH))
    {
        return false;
    }
    if ((ret = PathRemoveFileSpecW(ini_path) && PathAppendW(ini_path, L"portable.ini")) == false)
    {
        return false;
    }
    if (!PathFileExistsW(ini_path))
    {
        WCHAR ini_example[MAX_PATH + 1] = {0};
        wcsncpy(ini_example, ini_path, MAX_PATH);
        if ((ret = PathRemoveFileSpecW(ini_example) && PathAppendW(ini_example, L"tmemutil.ini") &&
                   PathFileExistsW(ini_example)) == false)
        {
            if (PathRemoveFileSpecW(ini_example) && PathAppendW(ini_example, L"portable(example).ini") &&
                PathFileExistsW(ini_example))
            {
                ret = true;
            }
        }
        if (ret)
        {
            ret = CopyFileW(ini_example, ini_path, true);
        }
    }
    return (ret && WideCharToMultiByte(CP_UTF8, 0, ini_path, -1, ini_portable_path, MAX_PATH, NULL, NULL) > 0);
}

static HMODULE
init_verinfo(void) /* 初始化version.dll里面的三个函数 */
{
    HMODULE h_ver = LoadLibraryW(L"version.dll");
    if (h_ver != NULL)
    {
        pfnGetFileVersionInfoSizeW = (PFNGFVSW) GetProcAddress(h_ver, "GetFileVersionInfoSizeW");
        pfnGetFileVersionInfoW = (PFNGFVIW) GetProcAddress(h_ver, "GetFileVersionInfoW");
        pfnVerQueryValueW = (PFNVQVW) GetProcAddress(h_ver, "VerQueryValueW");
        if (!(pfnGetFileVersionInfoSizeW && pfnGetFileVersionInfoW && pfnVerQueryValueW))
        {
            FreeLibrary(h_ver);
            h_ver = NULL;
        }
    }
    return h_ver;
}

static bool
get_product_name(LPCWSTR filepath, LPWSTR out_string, size_t len, bool plugin)
{
    HMODULE h_ver = NULL;
    bool ret = false;
    DWORD dwHandle = 0;
    DWORD dwSize = 0;
    uint16_t i;
    uint32_t cbTranslate = 0;
    LPWSTR pBuffer = NULL;
    PVOID pTmp = NULL;
    WCHAR dwBlock[NAMES_LEN + 1] = { 0 };
    LANGANDCODEPAGE *lpTranslate = NULL;
    do
    {
        if ((h_ver = init_verinfo()) == NULL)
        {
            break;
        }
        if ((dwSize = pfnGetFileVersionInfoSizeW(filepath, &dwHandle)) == 0)
        {
        #ifdef _LOGDEBUG
            logmsg("pfnGetFileVersionInfoSizeW return false\n");
        #endif
            break;
        }
        if ((pBuffer = (LPWSTR) SYS_MALLOC(dwSize * sizeof(WCHAR))) == NULL)
        {
            break;
        }
        if (!pfnGetFileVersionInfoW(filepath, 0, dwSize, (LPVOID) pBuffer))
        {
        #ifdef _LOGDEBUG
            logmsg("pfnpfnGetFileVersionInfoW return false\n");
        #endif
            break;
        }
        pfnVerQueryValueW((LPCVOID) pBuffer, L"\\VarFileInfo\\Translation", (LPVOID *) &lpTranslate, &cbTranslate);
        if (NULL == lpTranslate)
        {
            break;
        }
        for (i = 0; i < (cbTranslate / sizeof(LANGANDCODEPAGE)); i++)
        {
            if (plugin)
            {
                wnsprintfW(dwBlock, NAMES_LEN, L"\\StringFileInfo\\%ls\\ProductName", L"040904e4");
            }
            else
            {
                wnsprintfW(dwBlock,
                           NAMES_LEN,
                           L"\\StringFileInfo\\%04x%04x\\ProductName",
                           lpTranslate[i].wLanguage,
                           lpTranslate[i].wCodePage);
            }
            ret = pfnVerQueryValueW((LPCVOID) pBuffer, (LPCWSTR) dwBlock, (LPVOID *) &pTmp, &cbTranslate);
            if (ret)
            {
                out_string[0] = L'\0';
                wcsncpy(out_string, (LPCWSTR) pTmp, len);
                ret = wcslen(out_string) > 1;
                if (ret) break;
            }
        }
    } while (0);
    if (pBuffer)
    {
        SYS_FREE(pBuffer);
    }
    if (h_ver)
    {
        FreeLibrary(h_ver);
    }
    return ret;
}

int WINAPI
get_file_version(void)
{
    HMODULE h_ver = NULL;
    DWORD dwHandle = 0;
    DWORD dwSize = 0;
    VS_FIXEDFILEINFO *pversion = NULL;
    LPWSTR pbuffer = NULL;
    WCHAR dw_block[NAMES_LEN] = { 0 };
    WCHAR filepath[MAX_PATH] = { 0 };
    uint32_t cb = 0;
    int ver = 0;
    do
    {
        if (GetModuleFileNameW(NULL, filepath, MAX_PATH) == 0)
        {
            break;
        }
        if ((h_ver = init_verinfo()) == NULL)
        {
            break;
        }
        if ((dwSize = pfnGetFileVersionInfoSizeW(filepath, &dwHandle)) == 0)
        {
        #ifdef _LOGDEBUG
            logmsg("pfnGetFileVersionInfoSizeW return false\n");
        #endif
            break;
        }
        if ((pbuffer = (LPWSTR) SYS_MALLOC(dwSize * sizeof(WCHAR))) == NULL)
        {
            break;
        }
        if (!pfnGetFileVersionInfoW(filepath, 0, dwSize, (LPVOID) pbuffer))
        {
        #ifdef _LOGDEBUG
            logmsg("pfnpfnGetFileVersionInfoW return false\n");
        #endif
            break;
        }
        if (!pfnVerQueryValueW((LPCVOID) pbuffer, L"\\", (void **) &pversion, &cb))
        {
        #ifdef _LOGDEBUG
            logmsg("pfnVerQueryValueW return false\n");
        #endif
            break;
        }
        if (pversion != NULL)
        {
            wnsprintfW(dw_block, NAMES_LEN, L"%d%d", HIWORD(pversion->dwFileVersionMS), LOWORD(pversion->dwFileVersionMS));
            ver = _wtoi(dw_block);
        }
    } while (0);
    if (pbuffer)
    {
        SYS_FREE(pbuffer);
    }
    if (h_ver)
    {
        FreeLibrary(h_ver);
    }
    return ver;
}

static WCHAR *
util_make_u16(const char *utf8, WCHAR *utf16, int len)
{
    *utf16 = 0;
    int m = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, utf16, len);
    if (m > 0 && m <= len)
    {
        utf16[m-1] = 0;
    }
    else if (len > 0)
    {
        utf16[len-1] = 0;
    }
    return utf16;
}

static char *
util_make_u8(const WCHAR *utf16, char *utf8, int len)
{
    *utf8 = 0;
    int m = WideCharToMultiByte(CP_UTF8, 0, utf16, -1, utf8, len, NULL, NULL);
    if (m > 0 && m <= len)
    {
        utf8[m-1] = 0;
    }
    else if (len > 0)
    {
        utf8[len-1] = 0;
    }
    return utf8;
}

/** * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * 替换unix风格的路径符号
 */
static char *
chr_replace(char *path)
{
    const char *lp = NULL;
    intptr_t pos;
    do
    {
        lp = strchr(path, '/');
        if (lp)
        {
            pos = lp - path;
            path[pos] = '\\';
        }
    } while (lp != NULL);
    return path;
}


static WCHAR *
wchr_replace(WCHAR *path)
{
    LPCWSTR lp = NULL;
    intptr_t pos;
    do
    {
        lp = wcschr(path, '/');
        if (lp)
        {
            pos = lp - path;
            path[pos] = L'\\';
        }
    } while (lp != NULL);
    return path;
}


/** * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * c风格的unicode字符串替换函数
 */
LPWSTR WINAPI
wstr_replace(LPWSTR in, const size_t in_size, LPCWSTR pattern, LPCWSTR by)
{
    WCHAR *in_ptr = in;
    WCHAR res[MAX_PATH + 1] = { 0 };
    size_t resoffset = 0;
    WCHAR *needle;
    while ((needle = wcsstr(in, pattern)) && resoffset < in_size)
    {
        wcsncpy(res + resoffset, in, needle - in);
        resoffset += needle - in;
        in = needle + (int) wcslen(pattern);
        wcsncpy(res + resoffset, by, wcslen(by));
        resoffset += (int) wcslen(by);
    }
    wcscpy(res + resoffset, in);
    wnsprintfW(in_ptr, (int) in_size, L"%ls", res);
    return in_ptr;
}

char* WINAPI
str_replace(char *in, const size_t in_size, const char *pattern, const char *by)
{
    WCHAR in_ptr[MAX_PATH + 1] = {0};
    WCHAR by_ptr[MAX_PATH + 1] = {0};
    WCHAR pattern_ptr[MAX_PATH + 1] = {0};
    util_make_u16(in, in_ptr, MAX_PATH);
    util_make_u16(by, by_ptr, MAX_PATH);
    util_make_u16(pattern, pattern_ptr, MAX_PATH);
    wstr_replace(in_ptr, in_size, pattern_ptr, by_ptr);
    util_make_u8(in_ptr, in, (int)in_size);
    return in;
}

static bool
path_strip_suffix(char *path)
{
    uint8_t mark = 0;
    int len = (int) strlen(path);
    char *lp = chr_replace(path);
    if (!lp)
    {
        return false;
    }
    if (path[len - 1] == '\"')
    {
        mark = '\"';
    }
    for (int i = len; i > 0; --i)
    {
        if (lp[i] == '\\')
        {
            if (mark)
            {
                lp[i] = mark;
                lp[i+1] = '\0';
            }
            else
            {
                lp[i] = '\0';
            }
            break;
        }
    }
    return true;
}

static WCHAR *
rand_str(WCHAR *str, const int len)
{
    srand((uint32_t)time(0) + GetCurrentProcessId());
    for (int i = 0; i < len; ++i)
    {
        str[i] = L'A' + rand() % 26;
    }
    str[len] = 0;
    return str;
}

static bool
try_write(LPCWSTR dir)
{
#define LEN_NAME 6
    HANDLE pfile = INVALID_HANDLE_VALUE;
    WCHAR dist_path[MAX_PATH] = {0};
    WCHAR temp[LEN_NAME + 1] = {0};
    wnsprintfW(dist_path,MAX_PATH,L"%ls\\%ls", dir, rand_str(temp, LEN_NAME));
    pfile = CreateFileW(dist_path, GENERIC_READ |
            GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_TEMPORARY |
            FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (pfile == INVALID_HANDLE_VALUE)
    {
    #ifdef _LOGDEBUG
        logmsg("create %ls failed\n", dist_path);
    #endif
    }
    CloseHandle(pfile);
    return (pfile != INVALID_HANDLE_VALUE);
#undef LEN_NAME
}

static bool
wexist_dir(LPCWSTR path)
{
    DWORD fileattr = GetFileAttributesW(path);
    if (fileattr != INVALID_FILE_ATTRIBUTES)
    {
        return (fileattr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    return false;
}

bool WINAPI
wexist_file(LPCWSTR path)
{
    DWORD fileattr = GetFileAttributesW(path);
    if (fileattr != INVALID_FILE_ATTRIBUTES)
    {
        return (fileattr & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }
    return false;
}

bool WINAPI
wcreate_dir(LPCWSTR dir)
{
    LPWSTR p = NULL;
    WCHAR tmp_name[MAX_PATH];
    wcscpy(tmp_name, dir);
    p = wcschr(tmp_name, L'\\');
    for (; p != NULL; *p = L'\\', p = wcschr(p + 1, L'\\'))
    {
        *p = L'\0';
        if (wexist_dir(tmp_name))
        {
            continue;
        }
        if (!CreateDirectoryW(tmp_name, NULL))
        {
            return false;
        }
    }
    return (wexist_dir(tmp_name)? true: (CreateDirectoryW(tmp_name, NULL)));
}


/** * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * mode = 0, 目录是否存在
 * mode > 0, 目录是否存在并可写.
 */
bool WINAPI
exist_dir(const char *path, int mode)
{
    bool  res = false;
    DWORD fileattr = 0;
    LPWSTR u16_path = ini_utf8_utf16(path, NULL);
    if (!u16_path)
    {
        return false;
    }
    if ((fileattr = GetFileAttributesW(u16_path)) !=
        INVALID_FILE_ATTRIBUTES)
    {
        res = (fileattr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    if (res && mode > 0)
    {
        res = try_write(u16_path);
    }
    free(u16_path);
    return res;
}

/** * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * mscrt _access for utf-8
 */
bool WINAPI
exist_file(const char *path, int mode)
{
    bool  res = false;
    LPWSTR u16_path = ini_utf8_utf16(path, NULL);
    if (!u16_path)
    {
        return false;
    }
    if (_waccess(u16_path, mode) != -1)
    {
        res = true;
    }
    free(u16_path);
    return res;
}

bool WINAPI
create_dir(const char *dir)
{
    bool   res = false;
    LPWSTR u16_path = ini_utf8_utf16(dir, NULL);
    if (!u16_path)
    {
        return false;
    }
    res = wcreate_dir(u16_path);
    free(u16_path);
    return res;
}

bool WINAPI
path_to_absolute(LPWSTR path, int len)
{
    WCHAR lpfile[MAX_PATH + 1] = { 0 };
    int n = 1;
    if (NULL == path || *path == L'\0' || *path == L' ')
    {
        return false;
    }
    if (*path == L'"')
    {
        if (wcslen(path) < 3 || path[wcslen(path) - 1] != L'"')
        {
            return false;
        }
        wnsprintfW(lpfile, MAX_PATH, L"%ls", &path[1]);
        lpfile[wcslen(lpfile) - 1] = L'\0';
    }
    else
    {
        wnsprintfW(lpfile, MAX_PATH, L"%ls", path);
    }
    wchr_replace(lpfile);
    if (lpfile[0] == L'%')
    {
        WCHAR buf_env[VALUE_LEN + 1] = { 0 };
        while (lpfile[n] != L'\0')
        {
            if (lpfile[n++] == L'%')
            {
                break;
            }
        }
        if (n < len)
        {
            wnsprintfW(buf_env, n + 1, L"%ls", lpfile);
        }
        if (wcslen(buf_env) > 1 && ExpandEnvironmentStringsW(buf_env, buf_env, VALUE_LEN) > 0)
        {
            if (lpfile[n] != 0 && lpfile[n] == '\\')
            {
                ++n;
            }
            return (NULL != PathCombineW(path, buf_env, &lpfile[n]));
        }
    }
    else if (lpfile[1] != L':')
    {
        WCHAR name[MAX_PATH + 1] = { 0 };
        if (GetModuleFileNameW(NULL, name, MAX_PATH) > 0 && PathRemoveFileSpecW(name))
        {
            return (NULL != PathCombineW(path, name, lpfile));
        }
    }
    return (NULL != PathCombineW(path, NULL, lpfile));
}

/** * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * 等待主窗口并获取句柄,增加线程退出倒计时8s
 */
HWND WINAPI
get_moz_hwnd(LPWNDINFO pInfo)
{
    HWND hwnd = NULL;
    int i = 10;
    while (!pInfo->hFF && i--)
    {
        bool m_loop = false;
        DWORD dwProcessId = 0;
        hwnd = FindWindowExW(NULL, hwnd, L"MozillaWindowClass", NULL);
        GetWindowThreadProcessId(hwnd, &dwProcessId);
        m_loop = (dwProcessId > 0 && dwProcessId == pInfo->hPid);
        if (!m_loop)
        {
            pInfo->hFF = NULL;
        }
        if (NULL != hwnd && m_loop)
        {
            pInfo->hFF = hwnd;
            break;
        }
        SleepEx(800, false);
    }
    return (hwnd != NULL ? hwnd : pInfo->hFF);
}

bool WINAPI
is_gui(LPCWSTR lpFileName)
{
    IMAGE_DOS_HEADER dos_header;
    IMAGE_NT_HEADERS pe_header;
    bool ret = false;
    HANDLE hFile = CreateFileW(lpFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (!goodHandle(hFile))
    {
        return ret;
    }
    do
    {
        DWORD readed = 0;
        DWORD m_ptr = SetFilePointer(hFile, 0, NULL, FILE_BEGIN);
        if (INVALID_SET_FILE_POINTER == m_ptr)
        {
            break;
        }
        ret = ReadFile(hFile, &dos_header, sizeof(IMAGE_DOS_HEADER), &readed, NULL);
        if (ret && readed != sizeof(IMAGE_DOS_HEADER) && dos_header.e_magic != 0x5a4d)
        {
            break;
        }
        m_ptr = SetFilePointer(hFile, dos_header.e_lfanew, NULL, FILE_BEGIN);
        if (INVALID_SET_FILE_POINTER == m_ptr)
        {
            break;
        }
        ret = ReadFile(hFile, &pe_header, sizeof(IMAGE_NT_HEADERS), &readed, NULL);
        if (ret && readed != sizeof(IMAGE_NT_HEADERS))
        {
            break;
        }
        ret = pe_header.OptionalHeader.Subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI;
    } while (0);
    CloseHandle(hFile);
    return ret;
}

/** * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * 获取当前进程所在目录，宽字符版本
 */
bool WINAPI
wget_process_directory(LPWSTR lpstrName, DWORD len)
{
    int i = 0;
    if (GetModuleFileNameW(NULL, lpstrName, len) > 0)
    {
        for (i = (int) wcslen(lpstrName); i > 0; i--)
        {
            if (lpstrName[i] == L'\\')
            {
                lpstrName[i] = L'\0';
                break;
            }
        }
    }
    return (i > 0 && i < (int) len);
}

/** * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * 获取当前进程所在目录，utf8版本
 */
bool WINAPI
get_process_directory(char *name, uint32_t len)
{
    WCHAR lpstrName[MAX_PATH+1] = {0};
    if (!wget_process_directory(lpstrName, MAX_PATH))
    {
        return false;
    }
    return (WideCharToMultiByte(CP_UTF8, 0, lpstrName, -1, name, len, NULL, NULL) > 0);
}


/** * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * 对比文件产品信息，是否为mozilla家族产品
 */
m_family WINAPI
is_ff_official(void)
{
    int i = 0;
    m_family var = MOZ_UNKOWN;
    WCHAR *moz_array[] = {L"Iceweasel",
                          L"Firefox",
                          L"Firefox Beta",
                          L"Firefox Developer Edition",
                          L"Firefox Nightly",
                          NULL
                         };
    WCHAR process_name[MAX_PATH + 1];
    WCHAR product_name[NAMES_LEN + 1] = { 0 };
    if (GetModuleFileNameW(NULL, process_name, MAX_PATH) > 0 &&
        get_product_name(process_name, product_name, NAMES_LEN, false))
    {
        for (; moz_array[i]; ++i)
        {
            if (_wcsicmp(product_name, moz_array[i]) == 0)
            {
                break;
            }
        }
        switch (i)
        {
        case 0:
            var = MOZ_ICEWEASEL;
            break;
        case 1:
            var = MOZ_FIREFOX;
            break;
        case 2:
            var = MOZ_BETA;
            break;
        case 3:
            var = MOZ_DEV;
            break;
        case 4:
            var = MOZ_NIGHTLY;
            break;
        default:
            var = MOZ_UNKOWN;
            break;
        }
    }
    return var;
}

/** * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * 通过调用地址,获取当前进程所在地址的模块名称的完整路径
 */
static bool
get_module_name(uintptr_t caller, WCHAR *out, int len)
{
    bool res = false;
    HMODULE module = NULL;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR) caller, &module))
    {
        *out = L'\0';
        if (GetModuleFileNameW(module, out, len) > 0)
        {
            res = true;
        }
    }
    if (module)
    {
        res = FreeLibrary(module);
    }
    return res;
}

bool WINAPI
cmd_has_profile(char *pout, const int size)
{
    int     count = 0;
    bool    ret = false;
    LPWSTR  *args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (NULL != args)
    {
        for (int i = 0; i < count; ++i)
        {
            WCHAR *p = NULL;
            if (args[i][0] == '-')
            {
                p = &args[i][1];
                if (args[i][1] == '-')
                {
                    p = &args[i][2];
                }
            }
            if (p && (_wcsicmp(p, L"p") == 0 || _wcsicmp(p, L"profile") == 0)  && (i + 1 < count))
            {
                ret = true;
                if (pout)
                {
                    util_make_u8(args[i + 1], pout, (int)size);
                }
                break;
            }
        }
        LocalFree(args);
    }
    return ret;
}

bool WINAPI
cmd_has_setup(void)
{
    int     count = 0;
    bool    ret = false;
    LPWSTR  *args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (NULL != args)
    {
        for (int i = 0; i < count; i++)
        {
            WCHAR *p = NULL;
            if (args[i][0] == '-')
            {
                p = &args[i][1];
                if (args[i][1] == '-')
                {
                    p = &args[i][2];
                }
            }
            if (p)
            {
                if ((_wcsicmp(p, L"p") == 0 || _wcsicmp(p, L"ProfileManager") == 0) && (i + 1 == count))
                {
                    ret = true;
                    break;
                }
            }
        }
        LocalFree(args);
    }
    return ret;
}

bool WINAPI
is_specialdll(uintptr_t caller, LPCWSTR files)
{
    bool res = false;
    WCHAR module[VALUE_LEN + 1] = {0};
    if (get_module_name(caller, module, VALUE_LEN))
    {
        if (wcschr(files, L'*') || wcschr(files, L'?'))
        {
            if (PathMatchSpecW(module, files))
            {
                res = true;
            }
        }
        else if (StrStrIW(module, files))
        {
            res = true;
        }
    }
    return res;
}

bool WINAPI
is_flash_plugins(uintptr_t caller)
{
    bool res = false;
    WCHAR dll_name[VALUE_LEN + 1];
    WCHAR product_name[NAMES_LEN + 1] = { 0 };
    if (get_module_name(caller, dll_name, VALUE_LEN) &&
        get_product_name(dll_name, product_name, NAMES_LEN, true))
    {
        res = _wcsicmp(L"Shockwave Flash", product_name) == 0;
    }
    return res;
}

/**************************************************************************************
 *获取profiles.ini文件绝对路径,保存到in_dir数组
 */
static bool
get_mozilla_inifile(char *in_dir, int len, const char *appdt)
{
    int m = crt_snprintf(in_dir, (size_t) len, "%s%s", appdt, "\\Mozilla\\Firefox\\profiles.ini");
    return (m > 0 && m < len);
}

/**************************************************************************************
 * 如果是参数启动,通过命令行参数，获取配置文件所在目录
 */
static bool
get_arg_path(const ini_cache *ini, char **out_path, bool *relative)
{
    bool ret = false;
    char name[NAMES_LEN+1] = {0};
    if (cmd_has_profile(name, NAMES_LEN))
    {
        if (name[0])
        {
            char *var = NULL;
            char u8_arg[MAX_PATH] = {0};
            crt_snprintf(u8_arg, MAX_PATH - 1, "Name=%s", name);
            if (inicache_search_string(u8_arg, &var, (ini_cache *)ini) && inicache_read_string(var, "Path", out_path, (ini_cache *)ini))
            {
                if (relative)
                {
                    *relative = inicache_read_int(var, "IsRelative", (ini_cache *)ini) > 0;
                }
                free(var);
                ret = true;
            }
        }
    }
    return ret;
}

/**************************************************************************************
 * 当ini区段内有Default=1时，下面的Path=为默认配置目录所在.
 * 当ini区段内有Locked=1时， 下面的Default=为默认配置目录所在.
 */
static bool
get_default_path(const ini_cache *ini, char **out_path, bool *relative)
{
    bool res = false;
    if (ini && out_path)
    {
        char *m_section = NULL;
        do
        {
            if (inicache_search_string("Default=1", &m_section, (ini_cache *)ini) &&
                inicache_read_string(m_section, "Path", out_path,(ini_cache *) ini))
            {
                res = true;
            #ifdef _LOGDEBUG
                logmsg("Path[%s]\n", *out_path);
            #endif
                break;
            }
            ini_safe_free(m_section);
            if (is_ff_official() == MOZ_DEV)
            {
                if (inicache_search_string("Name=dev-edition-default", &m_section, (ini_cache *)ini) &&
                    inicache_read_string(m_section, "Path", out_path, (ini_cache *)ini))
                {
                    res = true;
                    break;
                }
            }
            else
            {
                if (inicache_search_string("Name=default", &m_section, (ini_cache *)ini) &&
                    inicache_read_string(m_section, "Path", out_path, (ini_cache *)ini))
                {
                    res = true;
                    break;
                }
            }
        } while(0);
        if (m_section)
        {
            if (relative)
            {
                *relative = inicache_read_int(m_section, "IsRelative", (ini_cache *)ini) > 0;
            }
            free(m_section);
        }
    }
    return res;
}

static bool
get_extern_path(const ini_cache *ini, char **out_path, bool *relative)
{
    if (ini && out_path && inicache_read_string("Profile0", "Path", out_path, (ini_cache *)ini))
    {
        if (relative)
        {
            *relative = inicache_read_int("Profile0", "IsRelative", (ini_cache *)ini) > 0;
        }
        return true;
    }
    return false;
}

static int
get_last_section(const ini_cache *ini)
{
    int m = -1;
    const int k = 7;
    char data[VALUE_LEN][NAMES_LEN] = {0};
    inicache_foreach_section(data, VALUE_LEN, (ini_cache *)ini);
    for (int i = 0; i < VALUE_LEN; ++i)
    {
        if (*data[i] && strncmp(data[i], "Profile", k) == 0 && data[i][k] != 0)
        {
            int v = atoi(&data[i][k]);
            if (m < v)
            {
                m = v;
            }
        }
    }
    return m;
}

static void
rel2abs(char *in_dir, int len)
{
    WCHAR u16[MAX_PATH] = {0};
    WCHAR tmp[MAX_PATH] = {0};
    if (util_make_u16(in_dir, u16, MAX_PATH - 1)[0])
    {
        _wfullpath(tmp, u16, MAX_PATH - 1);
        util_make_u8(tmp, in_dir, len);
    }
}

/**************************************************************************************
 * 获取firefox配置文件目录或者便携式配置目录路径
 */
static bool
get_profile_path(char *in_dir, int len, const char *appdt, const ini_cache *handle)
{
    char *path = NULL;
    bool relative = false;
    if (!handle || !get_mozilla_inifile(in_dir, len, appdt))
    {
        *in_dir = 0;
        return false;
    }
    if (!(get_arg_path(handle, &path, &relative) ||
        get_default_path(handle, &path, &relative) ||
        get_extern_path(handle, &path, &relative)))
    {
    #ifdef _LOGDEBUG
        logmsg("get_profile_path error\n");
    #endif
        *in_dir = 0;
        return false;
    }
    if (relative && path_strip_suffix(in_dir))
    {
        strncat(in_dir, "\\", len);
        strncat(in_dir, path, len);
        rel2abs(in_dir, len);
        if (in_dir[strlen(in_dir) - 1] == '\\')
        {
            in_dir[strlen(in_dir) - 1] = '\0';
        }
    }
    if (path)
    {
        free(path);
    }
    return true;
}

 
static bool
write_section_header(ini_cache *ini, const int num)
{
    char data[VALUE_LEN + 1] = {0};
    if (is_ff_official() == MOZ_DEV)
    {
        wnsprintfA(data, VALUE_LEN, "[Profile%d]\r\nName=dev-edition-default\r\nIsRelative=1\r\nPath=../../../\r\n\r\n", num);
    }
    else
    {
        wnsprintfA(data, VALUE_LEN, "[Profile%d]\r\nName=default\r\nIsRelative=1\r\nPath=../../../\r\nDefault=1\r\n\r\n", num);
        
    }
    return inicache_new_section(data, ini);
}

static bool
search_default_section(ini_cache *ini, char **pvalue)
{
    if (is_ff_official() == MOZ_DEV)
    {
        return inicache_search_string("Name=dev-edition-default", pvalue, ini);
    }
    return inicache_search_string("Default=1", pvalue, ini);
}

static bool
write_ini_file(ini_cache *ini)
{
    if (inicache_new_section("[General]\r\nStartWithLastProfile=1\r\nVersion=2\r\n\r\n", ini))
    {
        return write_section_header(ini, 0);
    }
    return false;
}

static unsigned WINAPI
write_json_file(void *lparam)
{
    if (*xre_profile_path)
    {
        cJSON *json = NULL;
        char path[MAX_BUFF+1] = {0};
        WCHAR url[MAX_BUFF+1] = {0};
        wnsprintfW(url, MAX_BUFF, L"%ls\\addonStartup.json.lz4", xre_profile_path);
        if (wexist_file((LPCWSTR)url))
        {
            if ((json = json_lookup(url, path)) != NULL && PathRemoveFileSpecW(url) && get_process_directory(path, MAX_BUFF))
            {
            #ifdef _LOGDEBUG
                logmsg("we need rewrite addonStartup.json.lz4\n");
            #endif
                json_parser((void *)json, url, path);
            }
        }
        if (json)
        {
            cJSON_Delete(json);
        }
    }
    return 0;
}

bool WINAPI
write_file(LPCWSTR appdata_path)
{
    bool ret = false;
    bool exists = false;
    ini_cache handle = NULL;
    char appdt[MAX_BUFF + 1] = {0};
    char moz_profile[MAX_BUFF + 1] =  {0};
    _wputenv(L"LIBPORTABLE_FILEIO_DEFINED=1");
    if (ini_read_int("General", "Portable", ini_portable_path, true) <= 0)
    {
        return ret;
    }
    if (cmd_has_setup())
    {
    #ifdef _LOGDEBUG
        logmsg("browser setup profile\n");
    #endif
        return ret;
    }
    if (cmd_has_profile(NULL, 0))
    {
    #ifdef _LOGDEBUG
        logmsg("browser profile boot\n");
    #endif
        return ret;
    }
    if (!WideCharToMultiByte(CP_UTF8, 0, appdata_path, -1, appdt, MAX_BUFF, NULL, NULL))
    {
        return ret;
    }
    if (!get_mozilla_inifile(moz_profile, MAX_BUFF, appdt))
    {
        return ret;
    }
    exists = exist_file(moz_profile, 0);
    if ((handle = iniparser_create_cache(moz_profile, true, true)) == NULL)
    {
        return ret;
    }
    if (exists)
    {
        int n = -1;
        char *psection = NULL;
        char *out_path = NULL;
        if (search_default_section(&handle, &psection))
        {
            if (inicache_read_string(psection, "Path", &out_path, &handle))
            {
                if (strcmp(out_path, "../../../") != 0)
                {
                    if ((n = get_last_section(&handle)) >= 0)
                    {
                        inicache_write_string(psection, "Name", NULL, &handle);
                        inicache_write_string(psection, "Default", NULL, &handle);
                        write_section_header(&handle, n + 1);
                    }
                }
            }
            else
            {
                ret = inicache_write_string(psection, "Path", "../../../", &handle);
                ret = inicache_write_string(psection, "IsRelative", "1", &handle);
            }
        }
        else
        {
            n = get_last_section(&handle);
            write_section_header(&handle, n + 1);
        }
        if (psection)
        {
            free(psection);
        }
        if (out_path)
        {
            free(out_path);
        }
    }
    else
    {
        ret = write_ini_file(&handle);
    }
    if (handle)
    {
        iniparser_destroy_cache(&handle);
    }
    if (true)
    {
        wnsprintfW(xre_profile_path, MAX_BUFF, L"%ls", appdata_path);
        PathRemoveFileSpecW(xre_profile_path);
        ret = get_localdt_path(xre_profile_local_path, MAX_BUFF);
    }
    if (ret && ini_read_int("General", "DisableExtensionPortable", ini_portable_path, true) != 1)
    {
    #ifdef _LOGDEBUG
        logmsg("DisableExtensionPortable return false\n", ini_portable_path);
    #endif
        CloseHandle((HANDLE) _beginthreadex(NULL, 0, &write_json_file, NULL, 0, NULL));
    }
    return ret;
}

bool WINAPI
get_appdt_path(WCHAR *path, int len)
{
    char *buf = NULL;
    if (!ini_path_init())
    {
    #ifdef _LOGDEBUG
        logmsg("ini_path_init(%ls) return false!\n", ini_portable_path);
    #endif
        return false;
    }
    if (ini_read_int("General", "Portable", ini_portable_path, true) <= 0)
    {
        return (ExpandEnvironmentStringsW(L"%APPDATA%", path, len) > 0);
    }
    if (!ini_read_string("General", "PortableDataPath", &buf, ini_portable_path, true))
    {
    #ifdef _LOGDEBUG
        logmsg("(PortableDataPath be set default path!\n");
    #endif
        wnsprintfW(path, len, L"%ls", L"../Profiles");
    }
    else
    {
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, path, len);
    }
    if (buf)
    {
        free(buf);
    }
    if (!path_to_absolute(path, len))
    {
        return false;
    }
    return !!wcsncat(path, L"\\AppData", len);
}

bool WINAPI
get_localdt_path(WCHAR *path, int len)
{
    char *buf = NULL;
    if (!ini_path_init())
    {
        return false;
    }
    if (ini_read_int("General", "Portable", ini_portable_path, true) <= 0)
    {
        return (ExpandEnvironmentStringsW(L"%LOCALAPPDATA%", path, len) > 0);
    }
    if (ini_read_string("Env", "TmpDataPath", &buf, ini_portable_path, true));
    else if (!ini_read_string("General", "PortableDataPath", &buf, ini_portable_path, true))
    {
        wnsprintfW(path, len, L"%ls", L"../Profiles");
    }
    if (buf)
    {
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, path, len);
        free(buf);
    }
    if (!path_to_absolute(path, len))
    {
        return false;
    }
    return !!wcsncat(path, L"\\LocalAppData\\Temp\\Fx", len);
}

DWORD WINAPI
get_os_version(void)
{
    typedef void (WINAPI *RtlGetNtVersionNumbersPtr)(DWORD*, DWORD*, DWORD*);
    RtlGetNtVersionNumbersPtr fnRtlGetNtVersionNumbers = NULL;
    DWORD dwMajorVer, dwMinorVer, dwBuildNumber;
    DWORD ver = 0;
    HMODULE nt_dll = GetModuleHandleW(L"ntdll.dll");
    if (nt_dll)
    {
        fnRtlGetNtVersionNumbers = (RtlGetNtVersionNumbersPtr)GetProcAddress(nt_dll, "RtlGetNtVersionNumbers");
    }
    if (fnRtlGetNtVersionNumbers)
    {
    #define VER_NUM 5
        WCHAR pszOS[VER_NUM] = { 0 };
        fnRtlGetNtVersionNumbers(&dwMajorVer, &dwMinorVer,&dwBuildNumber);
        wnsprintfW(pszOS, VER_NUM, L"%lu%d%lu", dwMajorVer, 0, dwMinorVer);
        ver = wcstol(pszOS, NULL, 10);
    #undef VER_NUM
    }
    return ver;
}

static LIB_INLINE uint32_t
get_cache_size(void)
{
    int eax, ebx, ecx, edx;
    int size = 0;
    CPUID(0x80000000, &eax, &ebx, &ecx, &edx);
    if ((uint32_t)eax >= 0x80000006)
    {
        CPUID(0x80000006, &eax, &ebx, &ecx, &edx);
        size = (ecx >> 16) & 0xffff;
    }
    return size * 1024;
}

bool WINAPI
cpu_has_avx(void)
{
    bool has_avx = false;
    bool has_avx_hardware = false;
    int  eax, ebx, ecx, edx;
    CPUID(0x1, &eax, &ebx, &ecx, &edx);
    if ( eax >= 0x2)
    {
        /* check OSXSAVE flags */
        has_avx_hardware = (ecx & 0x10000000) != 0;
    }
    /* check AVX feature flags and XSAVE enabled by kernel */
    has_avx = has_avx_hardware && (ecx & 0x08000000) != 0 \
              &&(_xgetbv(0) & 6) == 6;
    return has_avx;
}

uint32_t __stdcall
get_level_size(void)
{
    return cpu_has_avx() ? get_cache_size() : 0;
}

bool WINAPI
print_process_module(DWORD pid)
{
    BOOL ret = true;
    HANDLE hmodule = NULL;
    MODULEENTRY32 me32 = { 0 };
    hmodule = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (hmodule == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    me32.dwSize = sizeof(MODULEENTRY32);
    if (Module32First(hmodule, &me32))
    {
        do
        {
        #ifdef _LOGDEBUG
            logmsg("szModule = %ls, start_addr = 0x%x, end_addr = 0x%x\n", me32.szExePath, me32.modBaseAddr, me32.modBaseAddr + me32.modBaseSize);
        #endif
        } while (Module32Next(hmodule, &me32));
    }
    else
    {
        ret = false;
    }
    CloseHandle(hmodule);
    return ret;
}