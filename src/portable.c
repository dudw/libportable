#ifndef LIBPORTABLE_STATIC
#define TETE_BUILD
#endif

#include "portable.h"
#include "general.h"
#include "safe_ex.h"
#include "ice_error.h"
#include "bosskey.h"
#include "new_process.h"
#include "cpu_info.h"
#include "balance.h"
#include "set_env.h"
#include "win_registry.h"
#include "on_tabs.h"
#include "ini_parser.h"
#include "json_paser.h"
#include "MinHook.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <knownfolders.h>
#include <versionhelpers.h>
#include <process.h>
#include <stdio.h>
#include <time.h>
#include <wchar.h>

#if defined(VC12_CRT) || defined(__MINGW32__)
#undef _DllMainCRTStartup
#define _DllMainCRTStartup DllMain
#endif

#ifndef SHSTDAPI
#define SHSTDAPI STDAPI
#endif

#define _UPDATE L"upcheck.exe "

typedef  LPITEMIDLIST PIDLIST_ABSOLUTE;

SHSTDAPI SHILCreateFromPath (PCWSTR pszPath, PIDLIST_ABSOLUTE *ppidl, DWORD *rgfInOut);

typedef HRESULT (WINAPI *SHGetFolderPathWPtr)(HWND hwndOwner,
        int    nFolder,
        HANDLE hToken,
        DWORD  dwFlags,
        LPWSTR pszPath);
typedef HRESULT (WINAPI *SHGetSpecialFolderLocationPtr)(HWND hwndOwner,
        int nFolder,
        LPITEMIDLIST *ppidl);
typedef bool (WINAPI *SHGetSpecialFolderPathWPtr)(HWND hwndOwner,
        LPWSTR lpszPath,
        int    csidl,
        bool   fCreate);
typedef HRESULT (WINAPI *SHGetKnownFolderIDListPtr)(REFKNOWNFOLDERID rfid,
        DWORD            dwFlags,
        HANDLE           hToken,
        PIDLIST_ABSOLUTE *ppidl);
typedef HRESULT (WINAPI *SHGetKnownFolderPathPtr)(REFKNOWNFOLDERID rfid,
        DWORD            dwFlags,
        HANDLE           hToken,
        PWSTR            *ppszPath);

HMODULE dll_module = NULL;
static  uintptr_t m_target[EXCLUDE_NUM];
static  SHGetFolderPathWPtr           sSHGetFolderPathWStub;
static  SHGetSpecialFolderLocationPtr sSHGetSpecialFolderLocationStub;
static  SHGetSpecialFolderPathWPtr    sSHGetSpecialFolderPathWStub;
static  SHGetKnownFolderIDListPtr     sSHGetKnownFolderIDListStub;
static  SHGetKnownFolderPathPtr       sSHGetKnownFolderPathStub;

typedef void (*pointer_to_handler)();
typedef struct _dyn_link_desc
{
    const char* name;
    void* hook;
    pointer_to_handler* original;
}dyn_link_desc;

/* AVX memset with non-temporal instructions */
TETE_EXT_CLASS void * __cdecl
memset_nontemporal_tt (void *dest, int c, size_t count)
{
    if (!cpu_has_avx())
    {
        return memset(dest, c, count);
    }
    return memset_avx(dest, c, count);
}

/* Get the second level cache size */
TETE_EXT_CLASS uint32_t
GetNonTemporalDataSizeMin_tt(void)
{
    return get_level_size();
}

/* Never used,to be compatible with tete's patch */
TETE_EXT_CLASS int
GetCpuFeature_tt(void)
{
    return 0;
}

TETE_EXT_CLASS intptr_t
GetAppDirHash_tt(void)
{
    return 0;
}

bool
creator_hook(void *target, void *func, void **original)
{
    if ( NULL != target && MH_CreateHook(target, func, original) == MH_OK )
    {
        return ( MH_EnableHook(target) == MH_OK );
    }
    return false;
}

bool
remove_hook(void **target)
{
    if ( NULL != target && MH_RemoveHook(*target) == MH_OK )
    {
        *target = NULL;
        return true;
    }
    return false;
}

HRESULT WINAPI
HookSHGetSpecialFolderLocation(HWND hwndOwner, int nFolder, LPITEMIDLIST *ppidl)
{
    int folder = nFolder & 0xff;
    if ( CSIDL_APPDATA == folder || CSIDL_LOCAL_APPDATA == folder )
    {
        LPITEMIDLIST pidlnew = NULL;
        HRESULT result = E_FAIL;
        switch (folder)
        {
            case CSIDL_APPDATA:
            {
                WCHAR appdata_path[MAX_PATH+1] = {0};
                if (!get_appdt_path(appdata_path, MAX_PATH))
                {
                    return result;
                }
                if ( appdata_path[0] != L'\0' )
                {
                    result = SHILCreateFromPath(appdata_path, &pidlnew, NULL);
                }
                break;
            }
            case CSIDL_LOCAL_APPDATA:
            {
                WCHAR localdt_path[MAX_PATH+1] = {0};
                if (!get_localdt_path(localdt_path, MAX_PATH))
                {
                    break;
                }
                if ( localdt_path[0] != L'\0' && wcreate_dir(localdt_path) )
                {
                    result = SHILCreateFromPath(localdt_path, &pidlnew, NULL);
                }
                break;
            }
            default:
                break;
        }
        if (result == S_OK)
        {
            *ppidl = pidlnew;
            return result;
        }
    }
    return sSHGetSpecialFolderLocationStub(hwndOwner, nFolder, ppidl);
}

HRESULT WINAPI
HookSHGetFolderPathW(HWND hwndOwner,int nFolder,HANDLE hToken,
                     DWORD dwFlags,LPWSTR pszPath)
{
    uintptr_t dwCaller;
    bool      dwFf = false;
    int       folder = nFolder & 0xff;
    HRESULT   ret = E_FAIL;
#ifndef LIBPORTABLE_STATIC
    WCHAR	  dllname[VALUE_LEN+1];
    GetModuleFileNameW(dll_module, dllname, VALUE_LEN);
#endif
    dwCaller = (uintptr_t)_ReturnAddress();
    dwFf = is_specialdll(dwCaller, L"*\\xul.dll")                 ||
        #ifndef LIBPORTABLE_STATIC
           is_specialdll(dwCaller, dllname)                       ||
        #endif
           is_flash_plugins(dwCaller);
    if ( !dwFf )
    {
        return sSHGetFolderPathWStub(hwndOwner, nFolder, hToken, dwFlags, pszPath);
    }
    switch (folder)
    {
        case CSIDL_APPDATA:
        {
            WCHAR appdata_path[MAX_PATH+1] = {0};
            if (!get_appdt_path(appdata_path, MAX_PATH))
            {
                break;
            }
            if ( appdata_path[0] != L'\0' )
            {
                int	 num = wnsprintfW(pszPath,MAX_PATH,L"%ls",appdata_path);
                if ( num>0 && num<MAX_PATH ) ret = S_OK;
            }
            break;
        }
        case CSIDL_LOCAL_APPDATA:
        {
            WCHAR localdt_path[MAX_PATH+1] = {0};
            if (!get_localdt_path(localdt_path, MAX_PATH))
            {
                break;
            }
            if (localdt_path[0] != L'\0' && wcreate_dir(localdt_path))
            {
                int	 num = wnsprintfW(pszPath,MAX_PATH,L"%ls",localdt_path);
                if (num>0 && num<MAX_PATH )
                    ret = S_OK;
            }
            break;
        }
        default:
            break;
    }

    if (S_OK != ret)
    {
        ret = sSHGetFolderPathWStub(hwndOwner, nFolder, hToken, dwFlags, pszPath);
    }
    return ret;
}

bool WINAPI
HookSHGetSpecialFolderPathW(HWND hwndOwner,LPWSTR lpszPath,int csidl,bool fCreate)
{
    bool       internal;
    uintptr_t  dwCaller = (uintptr_t)_ReturnAddress();
    internal = is_specialdll(dwCaller, L"*\\xul.dll") || is_flash_plugins(dwCaller);
    if ( !internal )
    {
        return sSHGetSpecialFolderPathWStub(hwndOwner,lpszPath,csidl,fCreate);
    }
    return (HookSHGetFolderPathW(
            hwndOwner,
            csidl + (fCreate ? CSIDL_FLAG_CREATE : 0),
            NULL,
            0,
            lpszPath)) == S_OK ? true : false;
}

HRESULT WINAPI
HookSHGetKnownFolderIDList(REFKNOWNFOLDERID rfid,DWORD dwFlags,HANDLE hToken,PIDLIST_ABSOLUTE *ppidl)
{
    if ( IsEqualGUID(rfid, &FOLDERID_RoamingAppData) )
    {
        return HookSHGetSpecialFolderLocation(NULL, CSIDL_APPDATA, ppidl);
    }
    else if ( IsEqualGUID(rfid, &FOLDERID_LocalAppData) )
    {
        return HookSHGetSpecialFolderLocation(NULL, CSIDL_LOCAL_APPDATA, ppidl);
    }
    return sSHGetKnownFolderIDListStub(rfid,dwFlags,hToken,ppidl);
}

HRESULT WINAPI
HookSHGetKnownFolderPath(REFKNOWNFOLDERID rfid,DWORD dwFlags,HANDLE hToken,PWSTR *ppszPath)
{
    *ppszPath = NULL;
    if ( IsEqualGUID(rfid, &FOLDERID_RoamingAppData) )
    {
        WCHAR appdata_path[MAX_PATH+1] = {0};
        if (!get_appdt_path(appdata_path, MAX_PATH))
        {
            return S_FALSE;
        }
        *ppszPath = CoTaskMemAlloc(sizeof(appdata_path));
        if (!*ppszPath)
        {
            return E_OUTOFMEMORY;
        #ifdef _LOGDEBUG
            logmsg("return E_OUTOFMEMORY\n");
        #endif
        }
        wcscpy(*ppszPath, appdata_path);
        PathRemoveBackslashW(*ppszPath);
        return S_OK;
    }
    else if ( IsEqualGUID(rfid, &FOLDERID_LocalAppData) )
    {
        WCHAR localdt_path[MAX_PATH+1] = {0};
        if (!get_localdt_path(localdt_path, MAX_PATH))
        {
            return S_FALSE;
        }
        *ppszPath = CoTaskMemAlloc(sizeof(localdt_path));
        if (!*ppszPath)
        {
            return E_OUTOFMEMORY;
        }
        wcscpy(*ppszPath, localdt_path);
        PathRemoveBackslashW(*ppszPath);
        return S_OK;
    }
    return sSHGetKnownFolderPathStub(rfid,dwFlags,hToken,ppszPath);
}

static void
init_portable(void)
{
#define DLD(s,h) {#s, (void*)(Hook##s), (pointer_to_handler*)(void*)(&h)}
    int i;
    HMODULE h_shell32;
    const dyn_link_desc api_tables[] =
    {
          DLD(SHGetSpecialFolderLocation, sSHGetSpecialFolderLocationStub)
        , DLD(SHGetFolderPathW, sSHGetFolderPathWStub)
        , DLD(SHGetSpecialFolderPathW, sSHGetSpecialFolderPathWStub)
        , DLD(SHGetKnownFolderIDList, sSHGetKnownFolderIDListStub)
        , DLD(SHGetKnownFolderPath, sSHGetKnownFolderPathStub)
    };
    int func_num = sizeof(api_tables)/sizeof(api_tables[0]);
    if ((h_shell32 = GetModuleHandleW(L"shell32.dll")) == NULL)
    {
        return;
    }
    if (!m_target[0])
    {
        for (i = 0 ; i<func_num; i++)
        {
            m_target[i] = (uintptr_t)GetProcAddress(h_shell32, api_tables[i].name);
        }
        for (i = 0 ; m_target[i]!=0&&i<func_num; i++)
        {
            creator_hook((void*)m_target[i], api_tables[i].hook, (void **)api_tables[i].original);
        }
    }
#undef DLD
}

static bool
diff_days(void)
{
    bool res = false;
    const uint64_t diff = 3600*24;
    uint64_t cur_time = (uint64_t)time(NULL);
    uint64_t last_time = ini_read_uint64("update", "last_check", ini_portable_path, true);
    if (cur_time - last_time > diff)
    {
        res = true;
    }
    return res;
}

/* uninstall hook and clean up */
void WINAPI
undo_it(void)
{
    if (g_mutex)
    {
    #ifdef _LOGDEBUG
        logmsg("clean LIBPORTABLE_LAUNCHER_PROCESS\n");
    #endif
        CloseHandle(g_mutex);
    }
    /* 反注册uia */
    un_uia();
    /* 解除快捷键 */
    uninstall_bosskey();
    /* 清理启动过的进程树 */
    kill_trees();
    jmp_end();
    MH_Uninitialize();
#ifdef _LOGDEBUG
    logmsg("all clean!\n");
#endif
}

unsigned WINAPI
update_thread(void *lparam)
{
    WCHAR *pos = NULL;
    WCHAR temp[MAX_PATH+1] = {0};
    WCHAR path[MAX_PATH+1] = {0};
    WCHAR wcmd[MAX_BUFF+1] = {0};
    if (!get_localdt_path(temp, MAX_PATH))
    {
    #ifdef _LOGDEBUG
        logmsg("get_localdt_path return false!\n");
    #endif
        return (0);
    }
    if (!GetModuleFileNameW(NULL, path, MAX_PATH))
    {
        return (0);
    }
    if ((pos = wcsrchr(path, L'\\')) == NULL)
    {
        return (0);
    }
    if (true)
    {
        wcsncpy(wcmd, path, ++pos-path);
        wcsncat(temp, L"\\Mozilla\\updates", MAX_PATH);
        _wputenv(L"LIBPORTABLE_UPCHECK_DEFINED=1");
    }
    if (ini_read_int("update", "be_ready", ini_portable_path, true) > 0)
    {
        wnsprintfW(wcmd, MAX_BUFF, L"%ls"_UPDATE L"-k %lu -e \"%ls\" -s \"%ls\" -u 1", wcmd, GetCurrentProcessId(), temp, path);
        CloseHandle(create_new(wcmd, NULL, NULL, 0, NULL));
    #ifdef _LOGDEBUG
        logmsg("update_thread will install!\n");
    #endif
    }
    else if (diff_days())
    {
        Sleep(8000);
        /* Use single threading during updates */
        wnsprintfW(wcmd, MAX_BUFF, L"%ls"_UPDATE L"-i auto -t 1 -k %lu -e \"%ls\"", wcmd, GetCurrentProcessId(), temp);
        CloseHandle(create_new(wcmd, NULL, NULL, 0, NULL));
    #ifdef _LOGDEBUG
        logmsg("update_thread will update!\n");
    #endif
    }
    return (1);
}

static bool
init_hook_data(void)
{
    HANDLE mutex = NULL;
    WCHAR appdt[MAX_PATH] = {0};
    if (!get_appdt_path(appdt, MAX_PATH))
    {
    #ifdef _LOGDEBUG
        logmsg("get_appdt_path(%ls) return false!\n", appdt);
    #endif
        return false;
    }
    if (MH_Initialize() != MH_OK)
    {
    #ifdef _LOGDEBUG
        logmsg("MH_Initialize false!!!!\n");
    #endif
        return false;
    }
    if (!_wgetenv(L"LIBPORTABLE_FILEIO_DEFINED"))
    {
        write_file(appdt);
    }
    else
    {
    #ifdef _LOGDEBUG
        logmsg("LIBPORTABLE_FILEIO_DEFINED!\n");
    #endif
    }
    if (!_wgetenv(L"LIBPORTABLE_SETENV_DEFINED"))
    {
        setenv_tt();
    }
    else
    {
    #ifdef _LOGDEBUG
        logmsg("LIBPORTABLE_SETENV_DEFINED!\n");
    #endif
    }
    if (ini_read_int("General", "Portable", ini_portable_path, true) > 0 && wcreate_dir(appdt))
    {
        init_portable();
        init_safed();
    }
    if (_wgetenv(L"LIBPORTABLE_UPCHECK_LAUNCHER_PROCESS") ||
       (mutex = OpenFileMappingW(PAGE_READONLY, false, L"LIBPORTABLE_LAUNCHER_PROCESS=1")) != NULL)
    {
    #ifdef _LOGDEBUG
        logmsg("LIBPORTABLE_LAUNCHER_PROCESS_DEFINED!\n");
    #endif
        _wputenv(L"LIBPORTABLE_UPCHECK_DEFINED=");
        _wputenv(L"LIBPORTABLE_ONTABS_DEFINED=");
        _wputenv(L"LIBPORTABLE_NEWPROCESS_DEFINED=");
    }
    if (mutex)
    {
        CloseHandle(mutex);
    }
    return true;
}

static void
window_hooks(void)
{
    int up = 0;
    ini_cache plist = NULL;
    DWORD ver = get_os_version();
    plist = iniparser_create_cache(ini_portable_path, false, true);
    if (!plist)
    {
        return;
    }
    up = inicache_read_int("General", "Update", &plist);
    if (_wgetenv(L"LIBPORTABLE_UPCHECK_DEFINED"))
    {
    #ifdef _LOGDEBUG
        logmsg("LIBPORTABLE_UPCHECK_DEFINED!\n");
    #endif
    }
    else if (is_ff_official() > MOZ_ICEWEASEL)
    {   // 支持官方版本更新开关的禁止与启用.
        CloseHandle((HANDLE) _beginthreadex(NULL, 0, &fn_update, (void *)(uintptr_t)up, 0, NULL));
        _wputenv(L"LIBPORTABLE_UPCHECK_DEFINED=1");
    }
    else if (ver > 503 && up && inicache_read_int("General", "Portable", &plist) > 0)
    {
        // 调用Iceweasel的自动更新进程.
        CloseHandle((HANDLE)_beginthreadex(NULL,0,&update_thread,NULL,0,NULL));
    }
    if (inicache_read_int("General", "CreateCrashDump", &plist) != 0)
    {
        init_exeception(NULL);
    }
    if (_wgetenv(L"LIBPORTABLE_ONTABS_DEFINED"))
    {
    #ifdef _LOGDEBUG
        logmsg("LIBPORTABLE_ONTABS_DEFINED!\n");
    #endif
    }
    else if (inicache_read_int("General", "OnTabs", &plist) > 0)
    {
        _wputenv(L"LIBPORTABLE_ONTABS_DEFINED=1");
        threads_on_tabs();
    }
    if (inicache_read_int("General", "DisableScan", &plist) > 0)
    {
        init_winreg(NULL);
    }
    if (inicache_read_int("General", "ProcessAffinityMask", &plist) > 0)
    {
        CloseHandle((HANDLE)_beginthreadex(NULL,0,&set_cpu_balance,NULL,0,NULL));
    }
    if (inicache_read_int("General", "Bosskey", &plist) > 0)
    {
        CloseHandle((HANDLE)_beginthreadex(NULL,0,&bosskey_thread,NULL,0,NULL));
    }
    if (_wgetenv(L"LIBPORTABLE_NEWPROCESS_DEFINED"))
    {
    #ifdef _LOGDEBUG
        logmsg("LIBPORTABLE_NEWPROCESS_DEFINED!\n");
    #endif
    }
    else if (inicache_read_int("General", "ProxyExe", &plist) > 0)
    {
        _wputenv(L"LIBPORTABLE_NEWPROCESS_DEFINED=1");
        CloseHandle((HANDLE)_beginthreadex(NULL,0,&run_process,NULL,0,NULL));
    }
    iniparser_destroy_cache(&plist);
}

static bool
child_proces_if(void)
{
    LPWSTR  *args = NULL;
    int     count = 0;
    bool    ret = false;
    if (is_ff_official() > 0)
    {
        args = CommandLineToArgvW(GetCommandLineW(), &count);
        if ( NULL != args )
        {
            int i;
            for (i = 0; i < count; ++i)
            {
                if ( (_wcsicmp(args[i], L"-greomni") == 0) )
                {
                    ret = true;
                    break;
                }
            }
            LocalFree(args);
        }
    }
    return ret;
}

void WINAPI
do_it(void)
{
    if (!child_proces_if())
    {
        if (!init_hook_data())
        {
            return;
        }
        if (is_ff_official() > 0 && !no_gui_boot())
        {
            window_hooks();
        }
    }
}

/* This is standard DllMain function. */
#ifdef __cplusplus
extern "C" {
#endif

#if defined(LIBPORTABLE_EXPORTS) || !defined(LIBPORTABLE_STATIC)
int CALLBACK
_DllMainCRTStartup(HINSTANCE hModule, DWORD dwReason, LPVOID lpvReserved)
{
    switch(dwReason)
    {
    case DLL_PROCESS_ATTACH:
        dll_module = (HMODULE)hModule;
        DisableThreadLibraryCalls(hModule);
    #ifdef _LOGDEBUG
        init_logs();
    #endif
    // 链接msvcrt时,先初始化几个函数,因为新版编译器不支持旧的io调用
    #if defined(_MSC_VER) && !defined(VC12_CRT)
        if (!init_crt_funcs())
        {
            break;
        }
    #endif
        do_it();
        break;
    case DLL_PROCESS_DETACH:
        undo_it();
    #if defined(ENABLE_TCMALLOC)
        FreeLibraryAndExitThread(dll_module, 0);
    #endif
        break;
    case DLL_THREAD_ATTACH:
        break;
    case DLL_THREAD_DETACH:
        break;
    default:
        return false;
    }
    return true;
}
#endif  /* LIBPORTABLE_EXPORTS */

#ifdef __cplusplus
}
#endif
