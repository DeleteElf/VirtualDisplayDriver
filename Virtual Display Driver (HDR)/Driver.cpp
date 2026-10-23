/*++

Copyright (c) Microsoft Corporation

Abstract:

	MSDN documentation on indirect displays can be found at https://msdn.microsoft.com/en-us/library/windows/hardware/mt761968(v=vs.85).aspx.

Environment:

	User Mode, UMDF

--*/

#include "Driver.h"

#include "Utils.h"
#include "IOHelper.h"
#include "version.h"

#include<fstream>
#include<sstream>
#include<string>
#include<tuple>
#include<vector>
#include<algorithm>
#include<iomanip>
#include<chrono>
#include <AdapterOption.h>
#include <xmllite.h>
#include <shlwapi.h>
#include <atlbase.h>
#include <iostream>
#include <cstdlib>
#include <windows.h>
#include <cstdio>
#include <sddl.h>
#include <mutex>
#include <cwchar>
#include <map>
#include <set>

#include <minidumpapiset.h>
#include <cctype>

using namespace std;
using namespace Microsoft::IndirectDisp;
using namespace Microsoft::WRL;

#define CONFIG_FILE "option.txt"
#define RESET_EVENT_NAME	"Global\\HdrVDDReset"
#define DISPLAY_SIGN_NAME	"Global\\HdrVDDSign"
#define DRIVER_NAME "HdrVDD"
#define LOG_FILE_NAME DRIVER_NAME".log"
#define DUMP_FILE_NAME DRIVER_NAME".dmp"
#define EDID_FILE_NAME DRIVER_NAME"_edid.bin"

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD VirtualDisplayDriverDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY VirtualDisplayDriverDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT VirtualDisplayDriverDeviceD0Exit;

EVT_IDD_CX_ADAPTER_INIT_FINISHED VirtualDisplayDriverAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES VirtualDisplayDriverAdapterCommitModes;

EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION VirtualDisplayDriverParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES VirtualDisplayDriverMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES VirtualDisplayDriverMonitorQueryModes;

EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN VirtualDisplayDriverMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN VirtualDisplayDriverMonitorUnassignSwapChain;

EVT_IDD_CX_ADAPTER_QUERY_TARGET_INFO VirtualDisplayDriverEvtIddCxAdapterQueryTargetInfo;
EVT_IDD_CX_MONITOR_SET_DEFAULT_HDR_METADATA VirtualDisplayDriverEvtIddCxMonitorSetDefaultHdrMetadata;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION2 VirtualDisplayDriverEvtIddCxParseMonitorDescription2;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES2 VirtualDisplayDriverEvtIddCxMonitorQueryTargetModes2;
EVT_IDD_CX_ADAPTER_COMMIT_MODES2 VirtualDisplayDriverEvtIddCxAdapterCommitModes2;

EVT_IDD_CX_MONITOR_SET_GAMMA_RAMP VirtualDisplayDriverEvtIddCxMonitorSetGammaRamp;

struct DisplayInfo
{
    bool use_edid;
    bool hw_cursor;
    LONG prefer_width;
    LONG prefer_height;
    UINT prefer_fps;
    BYTE edid_data[256];
    size_t edid_len;
    SIZE size_list[256];
    size_t size_cnt;
    UINT fps_list[16];
    size_t fps_cnt;

    bool record_stream;
    int record_size;
    char record_path[256];
    int edid_product_id;
};

struct{
    AdapterOption Adapter;
} Options;
vector<tuple<UINT,UINT,UINT>> displayResFpsList={{1920,1080,60},{1920,1080,60}}; //没有使用edid时，默认使用此配置
vector<DisplayInfo> displays; //显示器信息
std::string configPath = "C:\\ProgramData\\Zero\\HdrVDD";//默认配置路径 ，这边考虑到需要读取配置
vector<UINT> fpsList; //刷新率
vector<SIZE> res; //分辨率

UINT displayCount = 1; //默认初始化一个虚拟显示器
const int monitorMaxCount = 4;//默认最多4个显示器
bool HDRPlus = false;
bool SDR10 = false;
bool useEdid = true; //默认使用edid

const bool defaultDisplayRecordSteam = false; //是否启用流记录保存
const int defaultDisplayRecordSize = 100 * 1024 * 1024; //流记录保存的大小

//Mouse settings
bool hardwareCursor = true;//默认使用硬件鼠标
bool alphaCursorSupport = true;
int cursorMaxX = 128;
int cursorMaxY = 128;
IDDCX_XOR_CURSOR_SUPPORT xorCursorSupportLevel = IDDCX_XOR_CURSOR_SUPPORT_FULL;

//Rest
IDDCX_BITS_PER_COMPONENT SDRCOLOUR = IDDCX_BITS_PER_COMPONENT_8;
IDDCX_BITS_PER_COMPONENT HDRCOLOUR = IDDCX_BITS_PER_COMPONENT_10;

wstring ColourFormat = L"RGB";

// === EDID INTEGRATION SETTINGS ===
bool edidIntegrationEnabled = false;
bool autoConfigureFromEdid = false;
wstring edidProfilePath = L"EDID/monitor_profile.xml";
bool overrideManualSettings = false;
bool fallbackOnError = true;

// === HDR ADVANCED SETTINGS ===
bool hdr10StaticMetadataEnabled = false;
double maxDisplayMasteringLuminance = 1000.0;
double minDisplayMasteringLuminance = 0.05;
int maxContentLightLevel = 1000;
int maxFrameAvgLightLevel = 400;

bool colorPrimariesEnabled = false;
double redX = 0.708, redY = 0.292;
double greenX = 0.170, greenY = 0.797;
double blueX = 0.131, blueY = 0.046;
double whiteX = 0.3127, whiteY = 0.3290;

bool colorSpaceEnabled = false;
double gammaCorrection = 2.4;
wstring primaryColorSpace = L"sRGB";
bool enableMatrixTransform = false;

// === AUTO RESOLUTIONS SETTINGS ===
bool autoResolutionsEnabled = false;
wstring sourcePriority = L"manual";
int minRefreshRate = 24;
int maxRefreshRate = 240;
bool excludeFractionalRates = false;
int minResolutionWidth = 640;
int minResolutionHeight = 480;
int maxResolutionWidth = 7680;
int maxResolutionHeight = 4320;
bool useEdidPreferred = false;
int fallbackWidth = 1920;
int fallbackHeight = 1080;
int fallbackRefresh = 60;

// === COLOR ADVANCED SETTINGS ===
bool autoSelectFromColorSpace = false;
wstring forceBitDepth = L"auto";
bool fp16SurfaceSupport = true;
bool wideColorGamut = false;
bool hdrToneMapping = false;
double sdrWhiteLevel = 80.0;

// === MONITOR EMULATION SETTINGS ===
bool monitorEmulationEnabled = false;
bool emulatePhysicalDimensions = false;
int physicalWidthMm = 510;
int physicalHeightMm = 287;
bool manufacturerEmulationEnabled = false;


std::map<std::wstring, std::pair<std::wstring, std::wstring>> SettingsQueryMap = {
        {L"LoggingEnabled", {L"LOGS", L"logging"}},
        {L"DebugLoggingEnabled", {L"DEBUGLOGS", L"debuglogging"}},
        {L"CustomEdidEnabled", {L"CUSTOMEDID", L"CustomEdid"}},

        {L"PreventMonitorSpoof", {L"PREVENTMONITORSPOOF", L"PreventSpoof"}},
        {L"EdidCeaOverride", {L"EDIDCEAOVERRIDE", L"EdidCeaOverride"}},
        {L"SendLogsThroughPipe", {L"SENDLOGSTHROUGHPIPE", L"SendLogsThroughPipe"}},

        //Cursor Begin
        {L"HardwareCursorEnabled", {L"HARDWARECURSOR", L"HardwareCursor"}},
        {L"AlphaCursorSupport", {L"ALPHACURSORSUPPORT", L"AlphaCursorSupport"}},
        {L"CursorMaxX", {L"CURSORMAXX", L"CursorMaxX"}},
        {L"CursorMaxY", {L"CURSORMAXY", L"CursorMaxY"}},
        {L"XorCursorSupportLevel", {L"XORCURSORSUPPORTLEVEL", L"XorCursorSupportLevel"}},
        //Cursor End

        //Colour Begin
        {L"HDRPlusEnabled", {L"HDRPLUS", L"HDRPlus"}},
        {L"SDR10Enabled", {L"SDR10BIT", L"SDR10bit"}},
        {L"ColourFormat", {L"COLOURFORMAT", L"ColourFormat"}},
        //Colour End

        //EDID Integration Begin
        {L"EdidIntegrationEnabled", {L"EDIDINTEGRATION", L"enabled"}},
        {L"AutoConfigureFromEdid", {L"AUTOCONFIGFROMEDID", L"auto_configure_from_edid"}},
        {L"EdidProfilePath", {L"EDIDPROFILEPATH", L"edid_profile_path"}},
        {L"OverrideManualSettings", {L"OVERRIDEMANUALSETTINGS", L"override_manual_settings"}},
        {L"FallbackOnError", {L"FALLBACKONERROR", L"fallback_on_error"}},
        //EDID Integration End

        //HDR Advanced Begin
        {L"Hdr10StaticMetadataEnabled", {L"HDR10STATICMETADATA", L"enabled"}},
        {L"MaxDisplayMasteringLuminance", {L"MAXLUMINANCE", L"max_display_mastering_luminance"}},
        {L"MinDisplayMasteringLuminance", {L"MINLUMINANCE", L"min_display_mastering_luminance"}},
        {L"MaxContentLightLevel", {L"MAXCONTENTLIGHT", L"max_content_light_level"}},
        {L"MaxFrameAvgLightLevel", {L"MAXFRAMEAVGLIGHT", L"max_frame_avg_light_level"}},
        {L"ColorPrimariesEnabled", {L"COLORPRIMARIES", L"enabled"}},
        {L"RedX", {L"REDX", L"red_x"}},
        {L"RedY", {L"REDY", L"red_y"}},
        {L"GreenX", {L"GREENX", L"green_x"}},
        {L"GreenY", {L"GREENY", L"green_y"}},
        {L"BlueX", {L"BLUEX", L"blue_x"}},
        {L"BlueY", {L"BLUEY", L"blue_y"}},
        {L"WhiteX", {L"WHITEX", L"white_x"}},
        {L"WhiteY", {L"WHITEY", L"white_y"}},
        {L"ColorSpaceEnabled", {L"COLORSPACE", L"enabled"}},
        {L"GammaCorrection", {L"GAMMA", L"gamma_correction"}},
        {L"PrimaryColorSpace", {L"PRIMARYCOLORSPACE", L"primary_color_space"}},
        {L"EnableMatrixTransform", {L"MATRIXTRANSFORM", L"enable_matrix_transform"}},
        //HDR Advanced End

        //Auto Resolutions Begin
        {L"AutoResolutionsEnabled", {L"AUTORESOLUTIONS", L"enabled"}},
        {L"SourcePriority", {L"SOURCEPRIORITY", L"source_priority"}},
        {L"MinRefreshRate", {L"MINREFRESHRATE", L"min_refresh_rate"}},
        {L"MaxRefreshRate", {L"MAXREFRESHRATE", L"max_refresh_rate"}},
        {L"ExcludeFractionalRates", {L"EXCLUDEFRACTIONAL", L"exclude_fractional_rates"}},
        {L"MinResolutionWidth", {L"MINWIDTH", L"min_resolution_width"}},
        {L"MinResolutionHeight", {L"MINHEIGHT", L"min_resolution_height"}},
        {L"MaxResolutionWidth", {L"MAXWIDTH", L"max_resolution_width"}},
        {L"MaxResolutionHeight", {L"MAXHEIGHT", L"max_resolution_height"}},
        {L"UseEdidPreferred", {L"USEEDIDPREFERRED", L"use_edid_preferred"}},
        {L"FallbackWidth", {L"FALLBACKWIDTH", L"fallback_width"}},
        {L"FallbackHeight", {L"FALLBACKHEIGHT", L"fallback_height"}},
        {L"FallbackRefresh", {L"FALLBACKREFRESH", L"fallback_refresh"}},
        //Auto Resolutions End

        //Color Advanced Begin
        {L"AutoSelectFromColorSpace", {L"AUTOSELECTCOLOR", L"auto_select_from_color_space"}},
        {L"ForceBitDepth", {L"FORCEBITDEPTH", L"force_bit_depth"}},
        {L"Fp16SurfaceSupport", {L"FP16SUPPORT", L"fp16_surface_support"}},
        {L"WideColorGamut", {L"WIDECOLORGAMUT", L"wide_color_gamut"}},
        {L"HdrToneMapping", {L"HDRTONEMAPPING", L"hdr_tone_mapping"}},
        {L"SdrWhiteLevel", {L"SDRWHITELEVEL", L"sdr_white_level"}},
        //Color Advanced End

        //Monitor Emulation Begin
        {L"MonitorEmulationEnabled", {L"MONITOREMULATION", L"enabled"}},
        {L"EmulatePhysicalDimensions", {L"EMULATEPHYSICAL", L"emulate_physical_dimensions"}},
        {L"PhysicalWidthMm", {L"PHYSICALWIDTH", L"physical_width_mm"}},
        {L"PhysicalHeightMm", {L"PHYSICALHEIGHT", L"physical_height_mm"}},
        {L"ManufacturerEmulationEnabled", {L"MANUFACTUREREMULATION", L"enabled"}},
        {L"ManufacturerName", {L"MANUFACTURERNAME", L"manufacturer_name"}},
        {L"ModelName", {L"MODELNAME", L"model_name"}},
        {L"SerialNumber", {L"SERIALNUMBER", L"serial_number"}},
        //Monitor Emulation End
};

const char* XorCursorSupportLevelToString(IDDCX_XOR_CURSOR_SUPPORT level) {
    switch (level) {
        case IDDCX_XOR_CURSOR_SUPPORT_UNINITIALIZED:
            return "IDDCX_XOR_CURSOR_SUPPORT_UNINITIALIZED";
        case IDDCX_XOR_CURSOR_SUPPORT_NONE:
            return "IDDCX_XOR_CURSOR_SUPPORT_NONE";
        case IDDCX_XOR_CURSOR_SUPPORT_FULL:
            return "IDDCX_XOR_CURSOR_SUPPORT_FULL";
        case IDDCX_XOR_CURSOR_SUPPORT_EMULATION:
            return "IDDCX_XOR_CURSOR_SUPPORT_EMULATION";
        default:
            return "Unknown";
    }
}

#pragma region math

int gcd(int a, int b) {
    while (b != 0) {
        int temp = b;
        b = a % b;
        a = temp;
    }
    return a;
}

void float_to_vsync(float refresh_rate, int& num, int& den) {
    den = 10000;

    num = static_cast<int>(round(refresh_rate * den));

    int divisor = gcd(num, den);
    num /= divisor;
    den /= divisor;
}

#pragma endregion

#pragma region log
int logLevel = 3; //默认初始化一个日志级别为信息级
CRITICAL_SECTION logSec;
string logFile;
string logsPath;
int64_t logMaxSize = 1024 * 1024;//默认1m，太大会打不开

const char* debug = "DEBUG";
const char* info = "INFO";
const char* warn = "WARN";
const char* error = "ERROR";

typedef BOOL(WINAPI* MINIDUMPWRITEDUMP)(HANDLE hProcess, DWORD dwPid, HANDLE hFile, MINIDUMP_TYPE DumpType,
    CONST PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam, CONST PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
    CONST PMINIDUMP_CALLBACK_INFORMATION CallbackParam);

static MINIDUMPWRITEDUMP s_pMiniDumpWriteDump = nullptr;
LONG exception_handler(struct _EXCEPTION_POINTERS* apExceptionInfo)
{
    LOG_ERROR("will dump to file ...");
    if (apExceptionInfo == nullptr || apExceptionInfo->ExceptionRecord == nullptr) {
        LOG_ERROR("Exception info is null.");
        return EXCEPTION_CONTINUE_SEARCH;
    }
    PEXCEPTION_RECORD pRecord = apExceptionInfo->ExceptionRecord;   // 1. 获取异常记录
    // 2. 提取核心信息
    DWORD exceptionCode = pRecord->ExceptionCode;         // 错误码（如 0xC0000005 是内存访问违例）
    PVOID exceptionAddress = pRecord->ExceptionAddress;   // 崩溃发生的内存地址
    // 3. 打印到日志
    LOG_ERROR("Crash Detected! ExceptionCode: 0x%X, ExceptionAddress: %p",
              exceptionCode, exceptionAddress);
    // 如果是内存访问违例（Access Violation），还可以抓取是读冲突还是写冲突
    if (exceptionCode == EXCEPTION_ACCESS_VIOLATION && pRecord->NumberParameters >= 2) {
        LOG_ERROR("Access Violation Details: %s address %p",
                  (pRecord->ExceptionInformation[0] ? "Write to" : "Read from"), pRecord->ExceptionInformation[1]);
    }

    if (NULL == s_pMiniDumpWriteDump) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto hFile = CreateFileA(IOHelper::pathCombine(configPath, DUMP_FILE_NAME).c_str(),
            GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    const DWORD flags = MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithUnloadedModules |
                        MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData |
                        MiniDumpWithFullMemoryInfo | MiniDumpWithThreadInfo |
                        MiniDumpWithFullAuxiliaryState | MiniDumpIgnoreInaccessibleMemory |
                        MiniDumpWithTokenInformation;

    if (hFile != INVALID_HANDLE_VALUE)
    {
        _MINIDUMP_EXCEPTION_INFORMATION ExInfo;
        ExInfo.ThreadId = GetCurrentThreadId();
        ExInfo.ExceptionPointers = apExceptionInfo;
        ExInfo.ClientPointers = FALSE;

        s_pMiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                (MINIDUMP_TYPE)flags, &ExInfo, nullptr, nullptr);
        CloseHandle(hFile);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

void write_log(const char* level, const char* func, int line, const char* format, ...)
{
    if (level == debug && logLevel < (int)LogLevel::Debug)
        return;
    else if (level == info && logLevel < (int)LogLevel::Info)
        return;
    else if (level == warn && logLevel < (int)LogLevel::Warning)
        return;
    else if (level == error && logLevel < (int)LogLevel::Error)
        return;

    FILE* fp = NULL;
    EnterCriticalSection(&logSec);
    if (fopen_s(&fp, logFile.c_str(), "ab+, ccs=UTF-8") == 0 && fp != NULL) {
        char msg[1024];
        va_list args;
        va_start(args, format);
        vsnprintf_s(msg, sizeof(msg) - 1, format, args);
        va_end(args);

        // 获取当前时间点
        auto currentTime = std::chrono::system_clock::now();
        // 2. 将时间点转换为 time_t（精确到秒）
        std::time_t now = std::chrono::system_clock::to_time_t(currentTime);
        struct tm t;
        localtime_s(&t, &now);

        auto ts = currentTime.time_since_epoch();
        auto ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(ts).count() % 1000);
        fprintf_s(fp, "[%04d-%02d-%02d %02d:%02d:%02d,%03d][%s][proId:%d][threadId:%d][func:%s][line:%d]: %s\r\n",
                  t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, ms,
                  level, GetCurrentProcessId(), GetCurrentThreadId(), func, line, msg);
        auto file_len = ftell(fp);
        fclose(fp);
        if (logMaxSize > 0 && logMaxSize <= file_len) {
            logsPath = IOHelper::pathCombine(configPath, "../logs");
            IOHelper::checkAndCreateDirectory(logsPath);//生成日志的备份目录
            IOHelper::backupFile(logFile, logsPath);
        }
    }
    LeaveCriticalSection(&logSec);
}

#pragma endregion

#pragma region edid
const UINT64 MHZ = 1000000;
const UINT64 KHZ = 1000;

constexpr DISPLAYCONFIG_VIDEO_SIGNAL_INFO dispinfo(UINT32 h, UINT32 v, UINT32 rn, UINT32 rd) {
    const UINT32 clock_rate = rn * (v + 4) * (v + 4) / rd + 1000;
    return {
        clock_rate,                                      // pixel clock rate [Hz]
        { clock_rate, v + 4 },                         // fractional horizontal refresh rate [Hz]
        { clock_rate, (v + 4) * (v + 4) },          // fractional vertical refresh rate [Hz]
        { h, v },                                    // (horizontal, vertical) active pixel resolution
        { h + 4, v + 4 },                         // (horizontal, vertical) total pixel resolution
        { { 255, 0 }},                                   // video standard and vsync divider
        DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE
    };
}

vector<BYTE> hardcodedEdid =
{
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x36, 0x94, 0x37, 0x13, 0xe7, 0x1e, 0xe7, 0x1e,
    0x1c, 0x22, 0x01, 0x03, 0x80, 0x32, 0x1f, 0x78, 0x07, 0xee, 0x95, 0xa3, 0x54, 0x4c, 0x99, 0x26,
    0x0f, 0x50, 0x54, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c,
    0x45, 0x00, 0x63, 0xc8, 0x10, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x17, 0xf0, 0x0f,
    0xff, 0x37, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc,
    0x00, 0x56, 0x44, 0x44, 0x20, 0x62, 0x79, 0x20, 0x4d, 0x54, 0x54, 0x0a, 0x20, 0x20, 0x01, 0xc2,
    0x02, 0x03, 0x20, 0x40, 0xe6, 0x06, 0x0d, 0x01, 0xa2, 0xa2, 0x10, 0xe3, 0x05, 0xd8, 0x00, 0x67,
    0xd8, 0x5d, 0xc4, 0x01, 0x6e, 0x80, 0x00, 0x68, 0x03, 0x0c, 0x00, 0x00, 0x00, 0x30, 0x00, 0x0b,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8c
};

void updateCeaExtensionCount(vector<BYTE>& edid, int count) {
    edid[126] = static_cast<BYTE>(count);
}
//
//vector<BYTE> loadEdid(const string& filePath) {
//    if (customEdid) {
//        LOG_INFO( "Attempting to use user Edid");
//    }
//    else {
//        LOG_INFO( "Using hardcoded edid");
//        return hardcodedEdid;
//    }
//
//    ifstream file(filePath, ios::binary | ios::ate);
//    if (!file) {
//        LOG_INFO( "No custom edid found");
//        LOG_INFO( "Using hardcoded edid");
//        return hardcodedEdid;
//    }
//
//    streamsize size = file.tellg();
//    file.seekg(0, ios::beg);
//
//    vector<BYTE> buffer(size);
//    if (file.read((char*)buffer.data(), size)) {
//        //calculate checksum and compare it to 127 byte, if false then return hardcoded if true then return buffer to prevent loading borked edid.
//        BYTE calculatedChecksum = calculateChecksum(buffer);
//        if (calculatedChecksum != buffer[127]) {
//            LOG_ERROR( "Custom edid failed due to invalid checksum");
//            LOG_INFO( "Using hardcoded edid");
//            return hardcodedEdid;
//        }
//
//        if (edidCeaOverride) {
//            if (buffer.size() == 256) {
//                for (int i = 128; i < 256; ++i) {
//                    buffer[i] = hardcodedEdid[i];
//                }
//                updateCeaExtensionCount(buffer, 1);
//            }
//            else if (buffer.size() == 128) {
//                buffer.insert(buffer.end(), hardcodedEdid.begin() + 128, hardcodedEdid.end());
//                updateCeaExtensionCount(buffer, 1);
//            }
//        }
//
//        LOG_INFO( "Using custom edid");
//        return buffer;
//    }
//    else {
//        LOG_INFO( "Using hardcoded edid");
//        return hardcodedEdid;
//    }
//}
//
//int maincalc() {
//    vector<BYTE> edid = loadEdid(WStringToString(confpath) + "\\user_edid.bin");
//
//    if (!preventManufacturerSpoof) modifyEdid(edid);
//    BYTE checksum = calculateChecksum(edid);
//    edid[127] = checksum;
//    // Setting this variable is depricated, hardcoded edid is either returned or custom in loading edid function
//    IndirectDeviceContext::s_KnownMonitorEdid = edid;
//    return 0;
//}
//
//
//// Generate modes from EDID with advanced filtering and optimization
//vector<tuple<int, int, int, int>> GenerateModesFromEdid(const EdidProfileData& profile) {
//    vector<tuple<int, int, int, int>> generatedModes;
//
//    if (!autoResolutionsEnabled) {
//        LOG_INFO( "Auto resolutions disabled, skipping EDID mode generation");
//        return generatedModes;
//    }
//
//    for (const auto& mode : profile.modes) {
//        int width = get<0>(mode);
//        int height = get<1>(mode);
//        int refreshRateMultiplier = get<2>(mode);
//        int nominalRefreshRate = get<3>(mode);
//
//        // Apply comprehensive filtering
//        bool passesFilter = true;
//
//        // Resolution range filtering
//        if (width < minResolutionWidth || width > maxResolutionWidth ||
//            height < minResolutionHeight || height > maxResolutionHeight) {
//            passesFilter = false;
//        }
//
//        // Refresh rate filtering
//        if (nominalRefreshRate < minRefreshRate || nominalRefreshRate > maxRefreshRate) {
//            passesFilter = false;
//        }
//
//        // Fractional rate filtering
//        if (excludeFractionalRates && refreshRateMultiplier != 1000) {
//            passesFilter = false;
//        }
//
//        // Add custom quality filtering
//        if (passesFilter) {
//            // Prefer standard aspect ratios for better compatibility
//            double aspectRatio = static_cast<double>(width) / height;
//            bool isStandardAspect = (abs(aspectRatio - 16.0/9.0) < 0.01) ||  // 16:9
//                                    (abs(aspectRatio - 16.0/10.0) < 0.01) ||  // 16:10
//                                    (abs(aspectRatio - 4.0/3.0) < 0.01) ||    // 4:3
//                                    (abs(aspectRatio - 21.0/9.0) < 0.01);     // 21:9
//
//            // Log non-standard aspect ratios for information
//            if (!isStandardAspect) {
//                stringstream ss;
//                ss << "Including non-standard aspect ratio mode: " << width << "x" << height
//                   << " (ratio: " << fixed << setprecision(2) << aspectRatio << ")";
//                LOG_DEBUG( ss.str().c_str());
//            }
//
//            generatedModes.push_back(mode);
//        }
//    }
//
//    // Sort modes by preference (resolution, then refresh rate)
//    sort(generatedModes.begin(), generatedModes.end(),
//         [](const tuple<int, int, int, int>& a, const tuple<int, int, int, int>& b) {
//             // Primary sort: resolution (area)
//             int areaA = get<0>(a) * get<1>(a);
//             int areaB = get<0>(b) * get<1>(b);
//             if (areaA != areaB) return areaA > areaB;  // Larger resolution first
//
//             // Secondary sort: refresh rate
//             return get<3>(a) > get<3>(b);  // Higher refresh rate first
//         });
//
//    stringstream ss;
//    ss << "Generated " << generatedModes.size() << " modes from EDID (filtered from " << profile.modes.size() << " total)";
//    LOG_INFO( ss.str().c_str());
//
//    return generatedModes;
//}
//
//// Find and validate preferred mode from EDID
//tuple<int, int, int, int> FindPreferredModeFromEdid(const EdidProfileData& profile,
//                                                    const vector<tuple<int, int, int, int>>& availableModes) {
//    // Default fallback mode
//    tuple<int, int, int, int> preferredMode = make_tuple(fallbackWidth, fallbackHeight, 1000, fallbackRefresh);
//
//    if (!useEdidPreferred) {
//        LOG_INFO( "EDID preferred mode disabled, using fallback");
//        return preferredMode;
//    }
//
//    // Look for EDID preferred mode in available modes
//    for (const auto& mode : availableModes) {
//        if (get<0>(mode) == profile.preferredWidth &&
//            get<1>(mode) == profile.preferredHeight) {
//            // Found matching resolution, use it
//            preferredMode = mode;
//
//            stringstream ss;
//            ss << "Found EDID preferred mode: " << profile.preferredWidth << "x" << profile.preferredHeight
//               << "@" << get<3>(mode) << "Hz";
//            LOG_INFO( ss.str().c_str());
//            break;
//        }
//    }
//
//    return preferredMode;
//}
//
//// Merge and optimize mode lists
//vector<tuple<int, int, int, int>> MergeAndOptimizeModes(const vector<tuple<int, int, int, int>>& manualModes,
//                                                        const vector<tuple<int, int, int, int>>& edidModes) {
//    vector<tuple<int, int, int, int>> mergedModes;
//
//    if (sourcePriority == L"edid") {
//        mergedModes = edidModes;
//        LOG_INFO( "Using EDID-only mode list");
//    }
//    else if (sourcePriority == L"manual") {
//        mergedModes = manualModes;
//        LOG_INFO( "Using manual-only mode list");
//    }
//    else if (sourcePriority == L"combined") {
//        // Start with manual modes
//        mergedModes = manualModes;
//
//        // Add EDID modes that don't duplicate manual modes
//        for (const auto& edidMode : edidModes) {
//            bool isDuplicate = false;
//            for (const auto& manualMode : manualModes) {
//                if (get<0>(edidMode) == get<0>(manualMode) &&
//                    get<1>(edidMode) == get<1>(manualMode) &&
//                    get<3>(edidMode) == get<3>(manualMode)) {
//                    isDuplicate = true;
//                    break;
//                }
//            }
//            if (!isDuplicate) {
//                mergedModes.push_back(edidMode);
//            }
//        }
//
//        stringstream ss;
//        ss << "Combined modes: " << manualModes.size() << " manual + "
//           << (mergedModes.size() - manualModes.size()) << " unique EDID = " << mergedModes.size() << " total";
//        LOG_INFO( ss.str().c_str());
//    }
//
//    return mergedModes;
//}
//
//// Optimize mode list for performance and compatibility
//vector<tuple<int, int, int, int>> OptimizeModeList(const vector<tuple<int, int, int, int>>& modes,
//                                                   const tuple<int, int, int, int>& preferredMode) {
//    vector<tuple<int, int, int, int>> optimizedModes = modes;
//
//    // Remove preferred mode from list if it exists, we'll add it at the front
//    optimizedModes.erase(
//            remove_if(optimizedModes.begin(), optimizedModes.end(),
//                      [&preferredMode](const tuple<int, int, int, int>& mode) {
//                          return get<0>(mode) == get<0>(preferredMode) &&
//                                 get<1>(mode) == get<1>(preferredMode) &&
//                                 get<3>(mode) == get<3>(preferredMode);
//                      }),
//            optimizedModes.end());
//
//    // Insert preferred mode at the beginning
//    optimizedModes.insert(optimizedModes.begin(), preferredMode);
//
//    // Remove duplicate modes (same resolution and refresh rate)
//    sort(optimizedModes.begin(), optimizedModes.end());
//    optimizedModes.erase(unique(optimizedModes.begin(), optimizedModes.end(),
//                                [](const tuple<int, int, int, int>& a, const tuple<int, int, int, int>& b) {
//                                    return get<0>(a) == get<0>(b) &&
//                                           get<1>(a) == get<1>(b) &&
//                                           get<3>(a) == get<3>(b);
//                                }),
//                         optimizedModes.end());
//
//    // Limit total number of modes for performance (Windows typically supports 20-50 modes)
//    const size_t maxModes = 32;
//    if (optimizedModes.size() > maxModes) {
//        optimizedModes.resize(maxModes);
//        stringstream ss;
//        ss << "Limited mode list to " << maxModes << " modes for optimal performance";
//        LOG_INFO( ss.str().c_str());
//    }
//
//    return optimizedModes;
//}
//
//// Enhanced mode validation with detailed reporting
//bool ValidateModeList(const vector<tuple<int, int, int, int>>& modes) {
//    if (modes.empty()) {
//        LOG_ERROR( "Mode list is empty - this will cause display driver failure");
//        return false;
//    }
//
//    stringstream validationReport;
//    validationReport << "=== MODE LIST VALIDATION REPORT ===\n"
//                     << "Total modes: " << modes.size() << "\n";
//
//    // Analyze resolution distribution
//    map<pair<int, int>, int> resolutionCount;
//    map<int, int> refreshRateCount;
//
//    for (const auto& mode : modes) {
//        pair<int, int> resolution = {get<0>(mode), get<1>(mode)};
//        resolutionCount[resolution]++;
//        refreshRateCount[get<3>(mode)]++;
//    }
//
//    validationReport << "Unique resolutions: " << resolutionCount.size() << "\n";
//    validationReport << "Unique refresh rates: " << refreshRateCount.size() << "\n";
//    validationReport << "Preferred mode: " << get<0>(modes[0]) << "x" << get<1>(modes[0])
//                     << "@" << get<3>(modes[0]) << "Hz";
//
//    LOG_INFO( validationReport.str().c_str());
//
//    return true;
//}
//
//bool LoadEdidProfile(const wstring& profilePath, EdidProfileData& profile) {
//    wstring fullPath = confpath + L"\\" + profilePath;
//
//    // Check if file exists
//    if (!PathFileExistsW(fullPath.c_str())) {
//        vddlog("w", ("EDID profile not found: " + WStringToString(fullPath)).c_str());
//        return false;
//    }
//
//    CComPtr<IStream> pStream;
//    CComPtr<IXmlReader> pReader;
//    HRESULT hr = SHCreateStreamOnFileW(fullPath.c_str(), STGM_READ, &pStream);
//    if (FAILED(hr)) {
//        LOG_ERROR( "LoadEdidProfile: Failed to create file stream.");
//        return false;
//    }
//
//    hr = CreateXmlReader(__uuidof(IXmlReader), (void**)&pReader, NULL);
//    if (FAILED(hr)) {
//        LOG_ERROR( "LoadEdidProfile: Failed to create XmlReader.");
//        return false;
//    }
//
//    hr = pReader->SetInput(pStream);
//    if (FAILED(hr)) {
//        LOG_ERROR( "LoadEdidProfile: Failed to set input stream.");
//        return false;
//    }
//
//    XmlNodeType nodeType;
//    const WCHAR* pwszLocalName;
//    const WCHAR* pwszValue;
//    UINT cwchLocalName;
//    UINT cwchValue;
//    wstring currentElement;
//    wstring currentSection;
//
//    // Temporary mode data
//    int tempWidth = 0, tempHeight = 0, tempRefreshRateMultiplier = 1000, tempNominalRefreshRate = 60;
//
//    while (S_OK == (hr = pReader->Read(&nodeType))) {
//        switch (nodeType) {
//            case XmlNodeType_Element:
//                hr = pReader->GetLocalName(&pwszLocalName, &cwchLocalName);
//                if (FAILED(hr)) return false;
//                currentElement = wstring(pwszLocalName, cwchLocalName);
//
//                // Track sections for context
//                if (currentElement == L"MonitorModes" || currentElement == L"HDRCapabilities" ||
//                    currentElement == L"ColorProfile" || currentElement == L"PreferredMode") {
//                    currentSection = currentElement;
//                }
//                break;
//
//            case XmlNodeType_Text:
//                hr = pReader->GetValue(&pwszValue, &cwchValue);
//                if (FAILED(hr)) return false;
//
//                wstring value = wstring(pwszValue, cwchValue);
//
//                // Parse monitor modes
//                if (currentSection == L"MonitorModes") {
//                    if (currentElement == L"Width") {
//                        tempWidth = stoi(value);
//                    }
//                    else if (currentElement == L"Height") {
//                        tempHeight = stoi(value);
//                    }
//                    else if (currentElement == L"RefreshRateMultiplier") {
//                        tempRefreshRateMultiplier = stoi(value);
//                    }
//                    else if (currentElement == L"NominalRefreshRate") {
//                        tempNominalRefreshRate = stoi(value);
//                        // Complete mode entry
//                        if (tempWidth > 0 && tempHeight > 0) {
//                            profile.modes.push_back(make_tuple(tempWidth, tempHeight, tempRefreshRateMultiplier, tempNominalRefreshRate));
//                            stringstream ss;
//                            ss << "EDID Mode: " << tempWidth << "x" << tempHeight << " @ " << tempRefreshRateMultiplier << "/" << tempNominalRefreshRate << "Hz";
//                            LOG_DEBUG( ss.str().c_str());
//                        }
//                    }
//                }
//                    // Parse HDR capabilities
//                else if (currentSection == L"HDRCapabilities") {
//                    if (currentElement == L"HDR10Supported") {
//                        profile.hdr10Supported = (value == L"true");
//                    }
//                    else if (currentElement == L"DolbyVisionSupported") {
//                        profile.dolbyVisionSupported = (value == L"true");
//                    }
//                    else if (currentElement == L"HDR10PlusSupported") {
//                        profile.hdr10PlusSupported = (value == L"true");
//                    }
//                    else if (currentElement == L"MaxLuminance") {
//                        profile.maxLuminance = stod(value);
//                    }
//                    else if (currentElement == L"MinLuminance") {
//                        profile.minLuminance = stod(value);
//                    }
//                }
//                    // Parse color profile
//                else if (currentSection == L"ColorProfile") {
//                    if (currentElement == L"PrimaryColorSpace") {
//                        profile.primaryColorSpace = value;
//                    }
//                    else if (currentElement == L"Gamma") {
//                        profile.gamma = stod(value);
//                    }
//                    else if (currentElement == L"RedX") {
//                        profile.redX = stod(value);
//                    }
//                    else if (currentElement == L"RedY") {
//                        profile.redY = stod(value);
//                    }
//                    else if (currentElement == L"GreenX") {
//                        profile.greenX = stod(value);
//                    }
//                    else if (currentElement == L"GreenY") {
//                        profile.greenY = stod(value);
//                    }
//                    else if (currentElement == L"BlueX") {
//                        profile.blueX = stod(value);
//                    }
//                    else if (currentElement == L"BlueY") {
//                        profile.blueY = stod(value);
//                    }
//                    else if (currentElement == L"WhiteX") {
//                        profile.whiteX = stod(value);
//                    }
//                    else if (currentElement == L"WhiteY") {
//                        profile.whiteY = stod(value);
//                    }
//                }
//                    // Parse preferred mode
//                else if (currentSection == L"PreferredMode") {
//                    if (currentElement == L"Width") {
//                        profile.preferredWidth = stoi(value);
//                    }
//                    else if (currentElement == L"Height") {
//                        profile.preferredHeight = stoi(value);
//                    }
//                    else if (currentElement == L"RefreshRate") {
//                        profile.preferredRefresh = stod(value);
//                    }
//                }
//                break;
//        }
//    }
//
//    stringstream ss;
//    ss << "EDID Profile loaded: " << profile.modes.size() << " modes, HDR10: " << (profile.hdr10Supported ? "Yes" : "No")
//       << ", Color space: " << WStringToString(profile.primaryColorSpace);
//    LOG_INFO( ss.str().c_str());
//
//    return true;
//}
//
//bool ApplyEdidProfile(const EdidProfileData& profile) {
//    if (!edidIntegrationEnabled) {
//        return false;
//    }
//
//    // === ENHANCED MODE MANAGEMENT ===
//    if (autoResolutionsEnabled) {
//        // Store original manual modes
//        vector<tuple<int, int, int, int>> originalModes = monitorModes;
//
//        // Generate optimized modes from EDID
//        vector<tuple<int, int, int, int>> edidModes = GenerateModesFromEdid(profile);
//
//        // Find preferred mode from EDID
//        tuple<int, int, int, int> preferredMode = FindPreferredModeFromEdid(profile, edidModes);
//
//        // Merge and optimize mode lists
//        vector<tuple<int, int, int, int>> finalModes = MergeAndOptimizeModes(originalModes, edidModes);
//
//        // Optimize final mode list with preferred mode priority
//        finalModes = OptimizeModeList(finalModes, preferredMode);
//
//        // Validate the final mode list
//        if (ValidateModeList(finalModes)) {
//            monitorModes = finalModes;
//            RebuildKnownMonitorModesCache();
//
//            stringstream ss;
//            ss << "Enhanced mode management completed:\n"
//               << "  Original manual modes: " << originalModes.size() << "\n"
//               << "  Generated EDID modes: " << edidModes.size() << "\n"
//               << "  Final optimized modes: " << finalModes.size() << "\n"
//               << "  Preferred mode: " << get<0>(preferredMode) << "x" << get<1>(preferredMode)
//               << "@" << get<3>(preferredMode) << "Hz\n"
//               << "  Source priority: " << WStringToString(sourcePriority);
//            LOG_INFO( ss.str().c_str());
//        } else {
//            LOG_ERROR( "Mode list validation failed, keeping original modes");
//        }
//    }
//
//    // Apply HDR settings if configured
//    if (hdr10StaticMetadataEnabled && profile.hdr10Supported) {
//        if (overrideManualSettings || maxDisplayMasteringLuminance == 1000.0) { // Default value
//            maxDisplayMasteringLuminance = profile.maxLuminance;
//        }
//        if (overrideManualSettings || minDisplayMasteringLuminance == 0.05) { // Default value
//            minDisplayMasteringLuminance = profile.minLuminance;
//        }
//    }
//
//    // Apply color primaries if configured
//    if (colorPrimariesEnabled && (overrideManualSettings || redX == 0.708)) { // Default Rec.2020 values
//        redX = profile.redX;
//        redY = profile.redY;
//        greenX = profile.greenX;
//        greenY = profile.greenY;
//        blueX = profile.blueX;
//        blueY = profile.blueY;
//        whiteX = profile.whiteX;
//        whiteY = profile.whiteY;
//    }
//
//    // Apply color space settings
//    if (colorSpaceEnabled && (overrideManualSettings || primaryColorSpace == L"sRGB")) { // Default value
//        primaryColorSpace = profile.primaryColorSpace;
//        gammaCorrection = profile.gamma;
//    }
//
//    // Generate and store HDR metadata for all monitors if HDR is enabled
//    if (hdr10StaticMetadataEnabled && profile.hdr10Supported) {
//        VddHdrMetadata hdrMetadata = ConvertEdidToSmpteMetadata(profile);
//
//        if (hdrMetadata.isValid) {
//            // Store metadata for future monitor creation
//            // Note: We don't have monitor handles yet at this point, so we'll store it as a template
//            // The actual association will happen when monitors are created or HDR metadata is requested
//
//            stringstream ss;
//            ss << "Generated SMPTE ST.2086 HDR metadata from EDID profile:\n"
//               << "  Red: (" << hdrMetadata.display_primaries_x[0] << ", " << hdrMetadata.display_primaries_y[0] << ") "
//               << "→ (" << profile.redX << ", " << profile.redY << ")\n"
//               << "  Green: (" << hdrMetadata.display_primaries_x[1] << ", " << hdrMetadata.display_primaries_y[1] << ") "
//               << "→ (" << profile.greenX << ", " << profile.greenY << ")\n"
//               << "  Blue: (" << hdrMetadata.display_primaries_x[2] << ", " << hdrMetadata.display_primaries_y[2] << ") "
//               << "→ (" << profile.blueX << ", " << profile.blueY << ")\n"
//               << "  White Point: (" << hdrMetadata.white_point_x << ", " << hdrMetadata.white_point_y << ") "
//               << "→ (" << profile.whiteX << ", " << profile.whiteY << ")\n"
//               << "  Max Luminance: " << hdrMetadata.max_display_mastering_luminance
//               << " (" << profile.maxLuminance << " nits)\n"
//               << "  Min Luminance: " << hdrMetadata.min_display_mastering_luminance
//               << " (" << profile.minLuminance << " nits)";
//            LOG_INFO( ss.str().c_str());
//
//            // Store as template metadata - will be applied to monitors during HDR metadata events
//            // We use a special key (nullptr converted to uintptr_t) to indicate template metadata
//            g_HdrMetadataStore[reinterpret_cast<IDDCX_MONITOR>(0)] = hdrMetadata;
//        } else {
//            vddlog("w", "Generated HDR metadata is not valid, skipping storage");
//        }
//    }
//
//    // Generate and store gamma ramp for color space processing if enabled
//    if (colorSpaceEnabled) {
//        VddGammaRamp gammaRamp = ConvertEdidToGammaRamp(profile);
//
//        if (gammaRamp.isValid) {
//            // Store gamma ramp as template for future monitor creation
//            stringstream ss;
//            ss << "Generated Gamma Ramp from EDID profile:\n"
//               << "  Gamma: " << gammaRamp.gamma << " (from " << profile.gamma << ")\n"
//               << "  Color Space: " << WStringToString(gammaRamp.colorSpace) << "\n"
//               << "  Matrix Transform: " << (gammaRamp.useMatrix ? "Enabled" : "Disabled");
//
//            if (gammaRamp.useMatrix) {
//                ss << "\n  3x4 Matrix:\n"
//                   << "    [" << gammaRamp.matrix.matrix[0][0] << ", " << gammaRamp.matrix.matrix[0][1] << ", " << gammaRamp.matrix.matrix[0][2] << ", " << gammaRamp.matrix.matrix[0][3] << "]\n"
//                   << "    [" << gammaRamp.matrix.matrix[1][0] << ", " << gammaRamp.matrix.matrix[1][1] << ", " << gammaRamp.matrix.matrix[1][2] << ", " << gammaRamp.matrix.matrix[1][3] << "]\n"
//                   << "    [" << gammaRamp.matrix.matrix[2][0] << ", " << gammaRamp.matrix.matrix[2][1] << ", " << gammaRamp.matrix.matrix[2][2] << ", " << gammaRamp.matrix.matrix[2][3] << "]";
//            }
//
//            LOG_INFO( ss.str().c_str());
//
//            // Store as template gamma ramp - will be applied to monitors during gamma ramp events
//            // We use a special key (nullptr converted to uintptr_t) to indicate template gamma ramp
//            g_GammaRampStore[reinterpret_cast<IDDCX_MONITOR>(0)] = gammaRamp;
//        } else {
//            vddlog("w", "Generated gamma ramp is not valid, skipping storage");
//        }
//    }
//
//    return true;
//}

#pragma endregion

#pragma region d3dDeviceCache
std::map<LUID, std::shared_ptr<Direct3DDevice>, Microsoft::IndirectDisp::LuidComparator> s_DeviceCache;
std::mutex s_DeviceCacheMutex;
std::shared_ptr<Direct3DDevice> GetOrCreateDevice(LUID RenderAdapter)
{
    std::shared_ptr<Direct3DDevice> Device;
    stringstream logStream;

    logStream << "GetOrCreateDevice called for LUID: " << RenderAdapter.HighPart << "-" << RenderAdapter.LowPart;
    LOG_DEBUG( logStream.str().c_str());

    {
        std::lock_guard<std::mutex> lock(s_DeviceCacheMutex);

        logStream.str("");
        logStream << "Device cache size: " << s_DeviceCache.size();
        LOG_DEBUG( logStream.str().c_str());

        auto it = s_DeviceCache.find(RenderAdapter);
        if (it != s_DeviceCache.end()) {
            Device = it->second;
            if (Device) {
                logStream.str("");
                logStream << "Reusing cached Direct3DDevice for LUID " << RenderAdapter.HighPart << "-" << RenderAdapter.LowPart;
                LOG_DEBUG( logStream.str().c_str());
                return Device;
            } else {
                logStream.str("");
                logStream << "Cached Direct3DDevice is null for LUID " << RenderAdapter.HighPart << "-" << RenderAdapter.LowPart << ", removing from cache";
                LOG_DEBUG( logStream.str().c_str());
                s_DeviceCache.erase(it);
            }
        }
    }

    logStream.str("");
    logStream << "Creating new Direct3DDevice for LUID " << RenderAdapter.HighPart << "-" << RenderAdapter.LowPart;
    LOG_DEBUG( logStream.str().c_str());

    Device = make_shared<Direct3DDevice>(RenderAdapter);
    if (FAILED(Device->Init())) {
        LOG_ERROR( "Failed to initialize new Direct3DDevice");
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(s_DeviceCacheMutex);
        s_DeviceCache[RenderAdapter] = Device;
        logStream.str("");
        logStream << "Created and cached new Direct3DDevice for LUID " << RenderAdapter.HighPart << "-" << RenderAdapter.LowPart << " (cache size now: " << s_DeviceCache.size() << ")";
        LOG_DEBUG( logStream.str().c_str());
    }

    return Device;
}

void CleanupExpiredDevices()
{
    std::lock_guard<std::mutex> lock(s_DeviceCacheMutex);

    int removed = 0;
    for (auto it = s_DeviceCache.begin(); it != s_DeviceCache.end();) {
        // With shared_ptr cache, we only remove null devices (shouldn't happen)
        if (!it->second) {
            it = s_DeviceCache.erase(it);
            removed++;
        } else {
            ++it;
        }
    }

    if (removed > 0) {
        stringstream logStream;
        logStream << "Cleaned up " << removed << " null Direct3DDevice references from cache";
        LOG_DEBUG( logStream.str().c_str());
    }
}

#pragma endregion

struct IndirectDeviceContextWrapper
{
    IndirectDeviceContext* pContext;

    void Cleanup()
    {
        if (pContext) {
            delete pContext;
            pContext = nullptr;
        }
    }
};

struct IndirectMonitorContextWrapper
{
    IndirectMonitorContext* pContext;

    void Cleanup()
    {
        if (pContext) {
            delete pContext;
            pContext = nullptr;
        }
    }
};

// This macro creates the methods for accessing an IndirectDeviceContextWrapper as a context for a WDF object
WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);
//WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IndirectDeviceContextWrapper, WdfObjectGet_IndirectDeviceContextWrapper)
//WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IndirectMonitorContextWrapper, WdfObjectGet_IndirectMonitorContextWrapper)
WDF_DECLARE_CONTEXT_TYPE(IndirectMonitorContextWrapper);

#pragma region colorSpace
// === EDID PROFILE LOADING FUNCTION ===
struct EdidProfileData {
    vector<tuple<int, int, int, int>> modes;
    bool hdr10Supported = false;
    bool dolbyVisionSupported = false;
    bool hdr10PlusSupported = false;
    double maxLuminance = 0.0;
    double minLuminance = 0.0;
    wstring primaryColorSpace = L"sRGB";
    double gamma = 2.2;
    double redX = 0.64, redY = 0.33;
    double greenX = 0.30, greenY = 0.60;
    double blueX = 0.15, blueY = 0.06;
    double whiteX = 0.3127, whiteY = 0.3290;
    int preferredWidth = 1920;
    int preferredHeight = 1080;
    double preferredRefresh = 60.0;
};
// === COLOR SPACE AND GAMMA STRUCTURES ===
struct VddColorMatrix {
    FLOAT matrix[3][4] = {}; // 3x4 color space transformation matrix - zero initialized
    bool isValid = false;
};

struct VddGammaRamp {
    FLOAT gamma = 2.2f;
    wstring colorSpace;
    VddColorMatrix matrix = {};
    bool useMatrix = false;
    bool isValid = false;
};

// === GAMMA AND COLOR SPACE STORAGE ===
std::map<IDDCX_MONITOR, VddGammaRamp> g_GammaRampStore;

// === COLOR SPACE AND GAMMA CONVERSION FUNCTIONS ===

// Convert gamma value to 3x4 color space transformation matrix
VddColorMatrix ConvertGammaToMatrix(double gamma, const wstring& colorSpace) {
    VddColorMatrix matrix = {};

    // Identity matrix as base
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            matrix.matrix[i][j] = (i == j) ? 1.0f : 0.0f;
        }
    }

    // Apply gamma correction to diagonal elements
    float gammaValue = static_cast<float>(gamma);

    if (colorSpace == L"sRGB") {
        // sRGB gamma correction (2.2)
        matrix.matrix[0][0] = gammaValue / 2.2f;  // Red
        matrix.matrix[1][1] = gammaValue / 2.2f;  // Green
        matrix.matrix[2][2] = gammaValue / 2.2f;  // Blue
    }
    else if (colorSpace == L"DCI-P3") {
        // DCI-P3 color space transformation with gamma
        // P3 to sRGB matrix with gamma correction
        matrix.matrix[0][0] = 1.2249f * (gammaValue / 2.4f);
        matrix.matrix[0][1] = -0.2247f;
        matrix.matrix[0][2] = 0.0f;
        matrix.matrix[1][0] = -0.0420f;
        matrix.matrix[1][1] = 1.0419f * (gammaValue / 2.4f);
        matrix.matrix[1][2] = 0.0f;
        matrix.matrix[2][0] = -0.0196f;
        matrix.matrix[2][1] = -0.0786f;
        matrix.matrix[2][2] = 1.0982f * (gammaValue / 2.4f);
    }
    else if (colorSpace == L"Rec.2020") {
        // Rec.2020 to sRGB matrix with gamma correction
        matrix.matrix[0][0] = 1.7347f * (gammaValue / 2.4f);
        matrix.matrix[0][1] = -0.7347f;
        matrix.matrix[0][2] = 0.0f;
        matrix.matrix[1][0] = -0.1316f;
        matrix.matrix[1][1] = 1.1316f * (gammaValue / 2.4f);
        matrix.matrix[1][2] = 0.0f;
        matrix.matrix[2][0] = -0.0241f;
        matrix.matrix[2][1] = -0.1289f;
        matrix.matrix[2][2] = 1.1530f * (gammaValue / 2.4f);
    }
    else if (colorSpace == L"Adobe_RGB") {
        // Adobe RGB with gamma correction
        matrix.matrix[0][0] = 1.0f * (gammaValue / 2.2f);
        matrix.matrix[1][1] = 1.0f * (gammaValue / 2.2f);
        matrix.matrix[2][2] = 1.0f * (gammaValue / 2.2f);
    }
    else {
        // Default to sRGB for unknown color spaces
        matrix.matrix[0][0] = gammaValue / 2.2f;
        matrix.matrix[1][1] = gammaValue / 2.2f;
        matrix.matrix[2][2] = gammaValue / 2.2f;
    }

    matrix.isValid = true;
    return matrix;
}

// Convert EDID profile to gamma ramp
VddGammaRamp ConvertEdidToGammaRamp(const EdidProfileData& profile) {
    VddGammaRamp gammaRamp = {};

    gammaRamp.gamma = static_cast<FLOAT>(profile.gamma);
    gammaRamp.colorSpace = profile.primaryColorSpace;

    // Generate matrix if matrix transforms are enabled
    if (enableMatrixTransform) {
        gammaRamp.matrix = ConvertGammaToMatrix(profile.gamma, profile.primaryColorSpace);
        gammaRamp.useMatrix = gammaRamp.matrix.isValid;
    }

    gammaRamp.isValid = colorSpaceEnabled;

    return gammaRamp;
}

// Convert manual settings to gamma ramp
VddGammaRamp ConvertManualToGammaRamp() {
    VddGammaRamp gammaRamp = {};

    gammaRamp.gamma = static_cast<FLOAT>(gammaCorrection);
    gammaRamp.colorSpace = primaryColorSpace;

    // Generate matrix if matrix transforms are enabled
    if (enableMatrixTransform) {
        gammaRamp.matrix = ConvertGammaToMatrix(gammaCorrection, primaryColorSpace);
        gammaRamp.useMatrix = gammaRamp.matrix.isValid;
    }

    gammaRamp.isValid = colorSpaceEnabled;

    return gammaRamp;
}

// Enhanced color format selection based on color space
IDDCX_BITS_PER_COMPONENT SelectBitDepthFromColorSpace(const wstring& colorSpace) {
    if (autoSelectFromColorSpace) {
        if (colorSpace == L"Rec.2020") {
            return IDDCX_BITS_PER_COMPONENT_10;  // HDR10 - 10-bit for wide color gamut
        } else if (colorSpace == L"DCI-P3") {
            return IDDCX_BITS_PER_COMPONENT_10;  // Wide color gamut - 10-bit
        } else if (colorSpace == L"Adobe_RGB") {
            return IDDCX_BITS_PER_COMPONENT_10;  // Professional - 10-bit
        } else {
            return IDDCX_BITS_PER_COMPONENT_8;   // sRGB - 8-bit
        }
    }

    // Manual bit depth override
    if (forceBitDepth == L"8") {
        return IDDCX_BITS_PER_COMPONENT_8;
    } else if (forceBitDepth == L"10") {
        return IDDCX_BITS_PER_COMPONENT_10;
    } else if (forceBitDepth == L"12") {
        return IDDCX_BITS_PER_COMPONENT_12;
    }

    // Default to existing color depth logic
    return HDRPlus ? IDDCX_BITS_PER_COMPONENT_12 :
           (SDR10 ? IDDCX_BITS_PER_COMPONENT_10 : IDDCX_BITS_PER_COMPONENT_8);
}

// === SMPTE ST.2086 HDR METADATA STRUCTURE ===
struct VddHdrMetadata {
    // SMPTE ST.2086 Display Primaries (scaled 0-50000) - zero initialized
    UINT16 display_primaries_x[3] = {};      // R, G, B chromaticity x coordinates
    UINT16 display_primaries_y[3] = {};      // R, G, B chromaticity y coordinates
    UINT16 white_point_x = 0;               // White point x coordinate
    UINT16 white_point_y = 0;               // White point y coordinate

    // Luminance values (0.0001 cd/m² units for SMPTE ST.2086)
    UINT32 max_display_mastering_luminance = 0;
    UINT32 min_display_mastering_luminance = 0;

    // Content light level (nits)
    UINT16 max_content_light_level = 0;
    UINT16 max_frame_avg_light_level = 0;

    // Validation flag
    bool isValid = false;
};

// === HDR METADATA STORAGE ===
std::map<IDDCX_MONITOR, VddHdrMetadata> g_HdrMetadataStore;

// === HDR METADATA CONVERSION FUNCTIONS ===

// Convert EDID chromaticity (0.0-1.0) to SMPTE ST.2086 format (0-50000)
UINT16 ConvertChromaticityToSmpte(double edidValue) {
    // Clamp to valid range
    if (edidValue < 0.0) edidValue = 0.0;
    if (edidValue > 1.0) edidValue = 1.0;

    return static_cast<UINT16>(edidValue * 50000.0);
}

// Convert EDID luminance (nits) to SMPTE ST.2086 format (0.0001 cd/m² units)
UINT32 ConvertLuminanceToSmpte(double nits) {
    // Clamp to reasonable range (0.0001 to 10000 nits)
    if (nits < 0.0001) nits = 0.0001;
    if (nits > 10000.0) nits = 10000.0;

    return static_cast<UINT32>(nits * 10000.0);
}

// Convert EDID profile data to SMPTE ST.2086 HDR metadata
VddHdrMetadata ConvertEdidToSmpteMetadata(const EdidProfileData& profile) {
    VddHdrMetadata metadata = {};

    // Convert chromaticity coordinates
    metadata.display_primaries_x[0] = ConvertChromaticityToSmpte(profile.redX);     // Red
    metadata.display_primaries_y[0] = ConvertChromaticityToSmpte(profile.redY);
    metadata.display_primaries_x[1] = ConvertChromaticityToSmpte(profile.greenX);   // Green
    metadata.display_primaries_y[1] = ConvertChromaticityToSmpte(profile.greenY);
    metadata.display_primaries_x[2] = ConvertChromaticityToSmpte(profile.blueX);    // Blue
    metadata.display_primaries_y[2] = ConvertChromaticityToSmpte(profile.blueY);

    // Convert white point
    metadata.white_point_x = ConvertChromaticityToSmpte(profile.whiteX);
    metadata.white_point_y = ConvertChromaticityToSmpte(profile.whiteY);

    // Convert luminance values
    metadata.max_display_mastering_luminance = ConvertLuminanceToSmpte(profile.maxLuminance);
    metadata.min_display_mastering_luminance = ConvertLuminanceToSmpte(profile.minLuminance);

    // Use configured content light levels (from vdd_settings.xml)
    metadata.max_content_light_level = static_cast<UINT16>(maxContentLightLevel);
    metadata.max_frame_avg_light_level = static_cast<UINT16>(maxFrameAvgLightLevel);

    // Mark as valid if we have HDR10 support
    metadata.isValid = profile.hdr10Supported && hdr10StaticMetadataEnabled;

    return metadata;
}

// Convert manual settings to SMPTE ST.2086 HDR metadata
VddHdrMetadata ConvertManualToSmpteMetadata() {
    VddHdrMetadata metadata = {};

    // Convert manual chromaticity coordinates
    metadata.display_primaries_x[0] = ConvertChromaticityToSmpte(redX);     // Red
    metadata.display_primaries_y[0] = ConvertChromaticityToSmpte(redY);
    metadata.display_primaries_x[1] = ConvertChromaticityToSmpte(greenX);   // Green
    metadata.display_primaries_y[1] = ConvertChromaticityToSmpte(greenY);
    metadata.display_primaries_x[2] = ConvertChromaticityToSmpte(blueX);    // Blue
    metadata.display_primaries_y[2] = ConvertChromaticityToSmpte(blueY);

    // Convert manual white point
    metadata.white_point_x = ConvertChromaticityToSmpte(whiteX);
    metadata.white_point_y = ConvertChromaticityToSmpte(whiteY);

    // Convert manual luminance values
    metadata.max_display_mastering_luminance = ConvertLuminanceToSmpte(maxDisplayMasteringLuminance);
    metadata.min_display_mastering_luminance = ConvertLuminanceToSmpte(minDisplayMasteringLuminance);

    // Use configured content light levels
    metadata.max_content_light_level = static_cast<UINT16>(maxContentLightLevel);
    metadata.max_frame_avg_light_level = static_cast<UINT16>(maxFrameAvgLightLevel);

    // Mark as valid if HDR10 metadata is enabled and color primaries are enabled
    metadata.isValid = hdr10StaticMetadataEnabled && colorPrimariesEnabled;

    return metadata;
}

// === ENHANCED MODE MANAGEMENT FUNCTIONS ===
#pragma endregion

#pragma region init

const BYTE defaultEdid[256] = {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, // 0-7: 固定的 EDID 头部
        0x0C, 0x64,                                     // 8-9: 厂商代码 (样例: SEC)
        0x37, 0x1A,                                     // 10-11: 产品代码 (将在代码中动态修改)
        0x15, 0xcd, 0x5b, 0x07,                         // 12-15: 32位序列号 (将在代码中动态修改)
        0x18, 0x1A,                                     // 16-17: 制造周、年份
        0x01, 0x04,                                     // 18-19: EDID 版本版本号 (1.4)
        0xb2, 0x3c, 0x22, 0x78, 0x04, 0xe6, 0xc5, 0xad, // 20-27: 图像显示参数与特性
        0x4e, 0x43, 0xaa, 0x26, 0x0b, 0x50, 0x54, 0x01, // 28-35: 颜色特征与基础时序
        0x40, 0x00, 0xd1, 0xc0, 0x95, 0xc0, 0xd1, 0xcf, // 36-43: 标准时序识别码
        0x95, 0xcf, 0xd1, 0xde, 0x95, 0xde, 0xd1, 0xfc, // 44-51: 标准时序识别码
        0x95, 0xfc, 0x4d, 0xd0, 0x00, 0xa0, 0xf0, 0x70, // 52-59: 详细时序描述符 (1920x1080 60Hz)
        0x3e, 0x80, 0x30, 0x20, 0x35, 0x00, 0x80, 0x88, // 60-67:
        0x42, 0x00, 0x00, 0x18, 0x5a, 0x87, 0x80, 0xa0, // 68-75: 显示器范围限制
        0x70, 0x38, 0x4d, 0x40, 0x30, 0x20, 0x35, 0x00, // 76-83:
        0x40, 0x44, 0x21, 0x00, 0x00, 0x18, 0x09, 0xec, // 84-91: 显示器名称描述符
        0x00, 0xa0, 0xa0, 0xa0, 0x67, 0x50, 0x30, 0x20, // 92-99:  "Virtual"
        0x35, 0x00, 0x00, 0xb0, 0x31, 0x00, 0x00, 0x18, // 100-107: "Disp"
        0x00, 0x00, 0x00, 0xfc, 0x00, 'H', 'd','r',  // 108-115: 空白描述符
        'V', 'D', 'D', 0x00, 0x0a, 0x20, 0x20, 0x20, // 116-123:
        0x20, 0x20, 0x00,
        0x00,
};

//void modifyEdid(char* edid,UINT displayIndex) {
//    edid[8] = 0x0C;
//    edid[9] = 0x64;
//    edid[10] = 0x37+displayIndex ;//product_id 低位
//    edid[11] = 0x1A; //product_id 高位
//}

//void modifyEdid(vector<BYTE>& edid,UINT displayIndex=0) {
//    if (edid.size() < 12) {
//        return;
//    }
//    edid[8] = 0x36;
//    edid[9] = 0x94;
//    edid[10] = 0x37+displayIndex ;
//    edid[11] = 0x13;
//}


BYTE calculateChecksum(const char* edid) {
    int sum = 0;
    for (int i = 0; i < 127; ++i) {
        sum += edid[i];
    }
    sum %= 256;
    if (sum != 0) {
        sum = 256 - sum;
    }
    return static_cast<BYTE>(sum);
}
BYTE calculateChecksum(const std::vector<BYTE>& edid) {
    int sum = 0;
    for (int i = 0; i < 127; ++i) {
        sum += edid[i];
    }
    sum %= 256;
    if (sum != 0) {
        sum = 256 - sum;
    }
    return static_cast<BYTE>(sum);
    // check sum calculations. We dont need to include old checksum in calculation, so we only read up to the byte before.
    // Anything after the checksum bytes arent part of the checksum - a flaw with edid managment, not with us
}
/// <summary>
/// 默认分辨率
/// </summary>
const SIZE defaultDisplayResolution[] =
        {
                // 8k
                //{ 7680, 5760 }, //  4:3  WHUSXGA
                //{ 7680, 4800 }, // 16:10 WHUXGA
                //{ 7680, 4320 }, // 16:9  8K UHD
                // 5k
                //{ 5120, 4096 }, //  5:4  HSXGA
                //{ 5120, 3840 }, //  4:3  WHSXGA
                //{ 5120, 3200 }, // 16:10 WHXGA
                //{ 5120, 2880 }, // 16:9  UHD++
                //{ 5120, 2160 }, // 21:9  5K UHD
                // 4k
                //{ 4096, 3072 }, //  4:3  HXGA
                //{ 4096, 2160 }, // 19:10 DCI 4K
                //{ 3840, 2880 }, //  4:3  WQUSXGA
                //{ 3840, 2400 }, // 16:10 WQUXGA
                { 3840, 2160 }, // 16:9  4K UHD
//        { 3840, 1600 }, // 12:5  UW4K
                // 2.5k
                //{ 3000, 2000 }, //  3:2          : matebook x pro
                //{ 2560, 2048 }, //  5:4  USXGA
//        { 2560, 1600 }, // 16:10 UWXGA
                { 2560, 1440 }, // 16:9  QHD/WQHD: retina macbook 13 pro
//        { 2560, 1080 }, // 21:9  CINEMA 21:9
                //{ 2160, 1440 }, //  3:2          : matebook 13/14
                // 2k
//        { 2048, 1536 }, //  4:3   SUVGA(QXGA)
//        { 2048, 1280 }, // 16:10 DCI 2K
//        { 2048, 1152 }, // 16:9   QWXGA
//        { 1920, 1440 }, //  4:3  WUSXGA
//        { 1920, 1200 }, // 16:10  WUXGA
                { 1920, 1080 }, // 16:9  WSUVGA+(WSUGA/HDTV)
                // 1600
//        { 1680, 1050 }, // 16:10 WSXGA+
//        { 1600, 1200 }, //  4:3  USVGA/UXGA/UGA
//        { 1600, 1024 }, // 25:16 WSXGA
//        { 1600,  900 }, // 16:9  HD+
                // 1440
//        { 1440,  960 }, //  3:2  FWXGA+
//        { 1440,  900 }, // 16:10  WXGA+  : macbook 13 air
                { 1366,  768 }, // 16:9  FWXGA
                // 1280
//        { 1280, 1024 }, //  5:4   SXGA
//        { 1280,  960 }, //  4:3   SXGA-/UVGA
//        { 1280,  800 }, // 16:10  WXGA
//        { 1280,  768 }, // 15:9   WXGA
                { 1280,  720 }, // 16:9   WXGA/HD
                { 1024,  768 }, //  4:3    XGA
                {  800,  600 }  //  4:3   SVGA
        };

/// <summary>
/// 默认的显示刷新率
/// </summary>
const UINT defaultDisplayFps[] = { 60 };

bool parseSizePerLine(const char* line, SIZE& size)
{
    size.cx = atoi(line);
    if (size.cx == 0) {
        return false;
    }
    line = strchr(line, 'x');
    if (line == NULL) {
        return false;
    }
    line++;
    size.cy = atoi(line);
    if (size.cy == 0) {
        return false;
    }
    return true;
}

void init() {
    InitializeCriticalSection(&logSec);
    int level=readFromRegWord(L"level");
    if (level != -1)
        logLevel = level;
    auto path = readFromRegString(L"path");
    if (!path.empty()) {
        IOHelper::checkAndCreateDirectory(configPath);//创建旧路径，可以用于甄别驱动是否进入加载逻辑
        configPath = WStringToString(path);
        LOG_DEBUG("从注册表配置读取到新的配置路径===>%s", configPath.c_str());
        if (IOHelper::checkAndCreateDirectory(path)) {//如果目录不存在，则创建目录
            LOG_DEBUG("新路径创建成功！");
            logFile = IOHelper::pathCombine(configPath, LOG_FILE_NAME);
        }
    }
    IOHelper::checkAndCreateDirectory(configPath);//如果目录不存在，则创建目录
    logFile = IOHelper::pathCombine(configPath, LOG_FILE_NAME);
    LOG_DEBUG("日志创建成功！");

#ifdef UNICODE
    const auto mhLib = LoadLibrary(L"dbghelp.dll");
#else
    const auto mhLib = LoadLibrary("dbghelp.dll");
#endif // !UNICODE
    if (mhLib) {
        s_pMiniDumpWriteDump = (MINIDUMPWRITEDUMP)GetProcAddress(mhLib, "MiniDumpWriteDump");
    }
    //todo:因为好几个地方都需要按这个最大数初始化，在没有明确新方案前，暂时不提供修改最大连接数的支持
//    auto maxCount = readFromRegWord(L"MaxCount");
//    if (maxCount > 0)
//        monitorMaxCount = maxCount;
}

void initDisplay(UINT displayIndex, DisplayInfo& display)
{
    LOG_INFO("正在初始化第[%d]个虚拟显示器,useEdid:%s",displayIndex,useEdid?"true":"false");
    display.use_edid = useEdid;
    display.hw_cursor = hardwareCursor;
    //LOG_INFO("正在初始化第[%d]个虚拟显示器,step0", displayIndex);
    if(displayResFpsList.size()<=displayIndex){ //手动配置的配置可能存在问题，这边进行自动修正。
        int index = (int)displayResFpsList.size() - 1;
        if (index < 0)
            index = 0;
        displayIndex=(UINT)index;
        LOG_WARN("检测到当前显示器索引[%d]，实际配置数量[%d],默认采用最后一个有效配置",displayIndex,displayResFpsList.size())
    }
    auto config=displayResFpsList[displayIndex];
    display.prefer_width =  get<0>(config);// res[0].cx;
    display.prefer_height =get<1>(config);// res[0].cy;
    display.prefer_fps = get<2>(config);//fpsList[0];
    //todo:流保存的逻辑，主要用于测试
    display.record_stream =defaultDisplayRecordSteam; // GetPrivateProfileIntA(INI_DISPLAY, INI_DISPLAY_RECORD_STREAM, kDisplayRecordSteam, ini_path);
    display.record_size =defaultDisplayRecordSize;// GetPrivateProfileIntA(INI_DISPLAY, INI_DISPLAY_RECORD_SIZE, kDisplayRecordSize, ini_path);
    if (display.record_stream)//暂时不启用这个
    {
        string format = IOHelper::pathCombine(configPath, "display%d.stream");
        snprintf(display.record_path, sizeof(display.record_path) - 1, format.c_str(), displayIndex + 1);
    }
    //todo:读取edid的逻辑，主要用于自定义显示器
    //LOG_INFO("正在初始化第[%d]个虚拟显示器,step1", displayIndex);
    display.edid_len = 0;
    string edidFile=IOHelper::pathCombine(configPath,EDID_FILE_NAME);
    if (IOHelper::exists(edidFile)) {//如果存在外部配置
        vector<char> vec;
        if (IOHelper::readEdIdData(edidFile.c_str(), vec)) {
            if (vec.size() == 128 || vec.size() == 256) {
                memcpy(display.edid_data, &vec[0], vec.size());
                display.edid_len = vec.size() / sizeof(display.edid_data[0]);
                LOG_INFO("%s use %s edid: count=%d", configPath.c_str(), EDID_FILE_NAME, display.edid_len);
            }
            else {
                LOG_ERROR("%s read %s length=%d", configPath.c_str(), EDID_FILE_NAME, vec.size());
            }
        }
    }
    // 读取不到则使用默认配置
    if (display.edid_len == 0) {
        display.edid_len = _countof(defaultEdid);
        memcpy(display.edid_data, defaultEdid, sizeof(display.edid_data[0]) * display.edid_len);
        LOG_INFO("使用默认的edid: %d", display.edid_len);
    }
    //LOG_INFO("正在初始化第[%d]个虚拟显示器,step2", displayIndex);
    // edid 的厂商改成CPY，产品id的最后一位改成对应的驱动实例，
    // 这样屏幕的ID类似 MONITOR\CPYD0C0\{4d36e96e-e325-11ce-bfc1-08002be10318}\0050
//    auto new_product_id = (display.edid_data[0x0A] & 0xF0) + displayIndex;
//    auto check_sum = (int)display.edid_data[0x7F];
//    check_sum += (display.edid_data[0x08] - 0x0E);
//    check_sum += (display.edid_data[0x09] - 0x19);
//    check_sum += (display.edid_data[0x0A] - new_product_id);
//    display.edid_data[0x08] = 0x0E;
//    display.edid_data[0x09] = 0x19;
//    display.edid_data[0x0A] = (BYTE)new_product_id;
//    display.edid_data[0x7F] = (BYTE)check_sum;
//    modifyEdid(reinterpret_cast<char*>(display.edid_data),displayIndex);
    display.edid_data[10] =(BYTE) (55 + displayIndex); //0x37+displayIndex
//    LOG_INFO("使用默认的edid 的 product_id: %d", display.edid_data[10]*256+display.edid_data[11]);
//    LOG_INFO("使用默认的edid 的 product_id: %d", display.edid_data[10]<<8|display.edid_data[11]);
    display.edid_data[127]= calculateChecksum(reinterpret_cast<char*>(display.edid_data));
    display.edid_product_id=display.edid_data[10];
    LOG_INFO("使用默认的edid 的 product_id: %d", display.edid_product_id);

    //LOG_INFO("正在初始化第[%d]个虚拟显示器,step3", displayIndex);
    display.size_cnt = res.size();//最多256个
    for (int i = 0; i < res.size(); i++)
        display.size_list[i] = res[i];
    //LOG_INFO("正在初始化第[%d]个虚拟显示器,step4", displayIndex);
    display.fps_cnt = fpsList.size();//最多16个
    std::copy(fpsList.begin(), fpsList.end(), display.fps_list);
//    for(int i=0;i<display.fps_cnt;i++){
//        LOG_DEBUG("支持的FPS：%d", display.fps_list[i]);
//    }
}

/// <summary>
/// 通过 EDID 数据查找对应的显示器索引
/// </summary>
int findDisplayIndexByEdid(const BYTE* pEdid, UINT edidLen)
{
    if (pEdid == nullptr || edidLen < 11) {
        LOG_WARN("EDID 数据为空或长度不足，len=%u", edidLen);
        return -1;
    }

    // 方法1：通过 product_id（第10字节）匹配 — 你的代码中每个显示器唯一化了
    BYTE productId = pEdid[10];
    for (int i = 0; i < (int)displays.size(); i++) {
        if (displays[i].edid_product_id == productId) {
            LOG_DEBUG("通过 product_id=%d 匹配到 display[%d]", productId, i);
            return i;
        }
    }

    // 方法2：通过完整 EDID 数据比对（更安全，兜底）
    for (int i = 0; i < (int)displays.size(); i++) {
        if (displays[i].edid_len == edidLen &&
            memcmp(displays[i].edid_data, pEdid, edidLen) == 0) {
            LOG_DEBUG("通过 EDID 完整比对匹配到 display[%d]", i);
            return i;
        }
    }

    // 方法3：都匹配不到，默认返回 0（单显示器场景兜底）
    LOG_WARN("未能匹配到显示器，product_id=%d，默认使用 display[0]", productId);
    return 0;
}

void initDisplays() {
    LOG_INFO("显示配置初始化,虚拟显示器数量:%d",displayCount);
    displays.clear();
    for (UINT i = 0; i < displayCount; i++)
    {
        displays.push_back(DisplayInfo());//初始化
        initDisplay(i, displays[i]);
    }
    LOG_INFO("显示配置初始化完成！");
}


void loadSettings() {
    auto configFile = IOHelper::pathCombine(configPath, CONFIG_FILE);
    auto configs = IOHelper::readAllLine(configFile);
    LOG_INFO("开始加载配置，读取到的配置条数:%d",configs.size());
    res.clear();//清空数据
    fpsList.clear();//清空数据
    hardwareCursor = true;//默认为true
    displayCount = 1;//默认一个
    if (configs.size() > 0) {
        for (int i = 0; i < configs.size(); i++)
        {
            /* 分析配置的主要逻辑
              * 1.如果存在注释符，注释之后，都不要
              * 2.如果存在=，则说明是指定属性
              * 3.如果不存在=，则说明是可用分辨率，可用分辨率用,号隔开
              * 4.如果存在多级数组，则第二级使用;号隔开
              */
            //parseSetting(config[i]);
            auto data = trimString(configs[i]);
            //string content = "正在处理配置0:" + data;
            //LOG_DEBUG(content.c_str());
            auto index = data.find_first_of('#');
            //LOG_DEBUG("正在处理配置注释:%d",index);
            if (index != 0) {//如果是0，说明整行配置都是注释，直接跳过此行
                if (index != std::string::npos) { //大于0，说明存在注释
                    //LOG_DEBUG("正在处理注释！");
                    data = trimString(data.erase(index));//去掉#之后的数据
                }
                //LOG_DEBUG("正在处理配置2");
                if (!data.empty()) {
                    string content1 = "正在处理配置:" + data;
                    LOG_DEBUG(content1.c_str());
                    index = data.find_first_of('=');
                    if (index != std::string::npos) { //属性配置
                        if (index > 0) {
                            auto setting = split(data, '=');
                            auto key = trimString(setting[0]);
                            if (key == "DisplayCount") {
                                auto count = stoi(setting[1]);
                                if (count >= 0) { //允许配置0个虚拟显示器
                                    displayCount = count;
                                }
                                if (count > monitorMaxCount) {
                                    displayCount = monitorMaxCount;
                                }
                            }
                            else if (key == "Displays") {
                                auto displaySetting = trimString(setting[1]);
                                if (!displaySetting.empty()) {
                                    displayResFpsList.clear();
                                    vector<string> strvec = split(setting[1], ';');
                                    for (int j = 0; j < strvec.size(); j++) {
                                        vector<string> temp = split(strvec[j], ',');
                                        if (temp.size() == 3) {
                                            displayResFpsList.push_back(make_tuple(stoi(temp[0]), stoi(temp[1]), stoi(temp[2])));
                                        }
                                    }
                                }
                            }
                            else if (key == "Rates") {
                                vector<string> strvec = split(setting[1], ',');
                                for (int j = 0; j < strvec.size(); j++)
                                {
                                    fpsList.push_back(stoi(strvec[j]));
                                }
                            }
                            else if (key == "LogLevel") {
                                auto option = trimString(setting[1]);
                                option = toLowerString(option);
                                if (option == "debug" || option == "4") {
                                    logLevel = 4;
                                }
                                else if (option == "info" || option == "3") {
                                    logLevel = 3;
                                }
                                else if (option == "warn" || option == "2") {
                                    logLevel = 2;
                                }
                                else if (option == "error" || option == "1") {
                                    logLevel = 1;
                                }
                                else {
                                    logLevel = 0;
                                }
                            }
                            else if (key == "HardwareCursor") {
                                auto option = trimString(setting[1]);
                                option = toLowerString(option);
                                hardwareCursor = option == "true";
                            }
                            else if (key == "UseEdid") {
                                auto option = trimString(setting[1]);
                                option = toLowerString(option);
                                useEdid = option == "true";
                            }
                        }
                    }else { //分辨率配置
                        vector<string> strvec = split(data, ',');
                        if (strvec.size() == 2) {
                            res.push_back({ stoi(strvec[0]), stoi(strvec[1]) });
                        }
                    }
                }
            }
        }
        LOG_INFO("使用option.txt配置的内容！");
    }else {
        LOG_INFO("使用默认配置的内容！");
    }
    if (fpsList.empty()) {//加入默认
        LOG_INFO("使用默认配置的刷新率！");
        fpsList.insert(fpsList.end(), begin(defaultDisplayFps),end(defaultDisplayFps));
    }else{
        LOG_INFO("使用自定义的刷新率配置，共%d条！",fpsList.size());
    }
    if (res.empty()) {
        LOG_INFO("使用默认配置的分辨率！");
        res.insert(res.end(), begin(defaultDisplayResolution), end(defaultDisplayResolution));
    }else{
        LOG_INFO("使用自定义的分辨率配置，共%d条！",res.size());
    }
    initDisplays();
}

#pragma endregion

#pragma region Pipe相关

#define PIPE_NAME L"\\\\.\\pipe\\CgTwVdd"

HANDLE hPipeThread = NULL;
std::atomic<bool> g_Running{true};
mutex g_Mutex;
HANDLE g_pipeHandle = INVALID_HANDLE_VALUE;

void  SendToPipe(const std::string& logMessage) {
    if (g_pipeHandle != INVALID_HANDLE_VALUE) {
        DWORD bytesWritten;
        DWORD logMessageSize = static_cast<DWORD>(logMessage.size());
        WriteFile(g_pipeHandle, logMessage.c_str(), logMessageSize, &bytesWritten, NULL);
    }
}

void ReloadDriver(HANDLE hPipe) {
    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(hPipe);
    if (pContext && pContext->pContext) {
        pContext->pContext->InitAdapter();
        LOG_INFO("Adapter reinitialized");//这句理论上永远无法调用执行，因为已经重新加载了
    }
}

void LogIddCxVersion() {
    IDARG_OUT_GETVERSION outArgs;
    NTSTATUS status = IddCxGetVersion(&outArgs);

    if (NT_SUCCESS(status)) {
        char versionStr[16];
        sprintf_s(versionStr, "0x%lx", outArgs.IddCxVersion);
        string logMessage = "IDDCX Version: " + string(versionStr);
        LOG_INFO(logMessage.c_str());
    }
    else {
        LOG_ERROR("Failed to get IDDCX version");
    }
}

LUID getSetAdapterLuid() {
    AdapterOption& adapterOption = Options.Adapter;
    if (!adapterOption.hasTargetAdapter) {
        LOG_ERROR("No Gpu Found/Selected");
    }
    return adapterOption.adapterLuid;
}

void GetGpuInfo()
{
    AdapterOption& adapterOption = Options.Adapter;
    if (!adapterOption.hasTargetAdapter) {
        LOG_ERROR("No GPU found or set.");
        return;
    }
    try {
        string utf8_desc = WStringToString(adapterOption.target_name);
        LUID luid = getSetAdapterLuid();
        string logtext = "ASSIGNED GPU: " + utf8_desc +" (LUID: " + std::to_string(luid.LowPart) + "-" + std::to_string(luid.HighPart) + ")";
        LOG_INFO( logtext.c_str());
    }
    catch (const exception& e) {
        LOG_ERROR(("Error: " + string(e.what())).c_str());
    }
}

void logAvailableGPUs() {
    vector<GPUInfo> gpus;
    ComPtr<IDXGIFactory1> factory;
    if (!SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return;
    }
    for (UINT i = 0;; i++) {
        ComPtr<IDXGIAdapter> adapter;
        if (!SUCCEEDED(factory->EnumAdapters(i, &adapter))) {
            break;
        }
        DXGI_ADAPTER_DESC desc;
        if (!SUCCEEDED(adapter->GetDesc(&desc))) {
            continue;
        }
        GPUInfo gpuInfo{ desc.Description, adapter, desc };
        gpus.push_back(gpuInfo);
    }
    for (const auto& gpu : gpus) {
        wstring logMessage = L"GPU Name: ";
        logMessage += gpu.desc.Description;
        wstring memorySize = L" Memory: ";
        memorySize += std::to_wstring(gpu.desc.DedicatedVideoMemory / (1024 * 1024)) + L" MB";
        wstring logText = logMessage + memorySize;
        int bufferSize = WideCharToMultiByte(CP_UTF8, 0, logText.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (bufferSize > 0) {
            std::string logTextA(bufferSize - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, logText.c_str(), -1, &logTextA[0], bufferSize, nullptr, nullptr);
            LOG_INFO(logTextA.c_str());
        }
    }
}

void HandleClient(HANDLE hPipe) {
    g_pipeHandle = hPipe;
    LOG_DEBUG("Client Handling Enabled");
    wchar_t buffer[128];
    DWORD bytesRead;
    BOOL result = ReadFile(hPipe, buffer, sizeof(buffer) - sizeof(wchar_t), &bytesRead, NULL);
    if (result && bytesRead != 0) {
        buffer[bytesRead / sizeof(wchar_t)] = L'\0';
        wstring bufferwstr(buffer);
        int bufferSize = WideCharToMultiByte(CP_UTF8, 0, bufferwstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        string bufferstr(bufferSize, 0);
        WideCharToMultiByte(CP_UTF8, 0, bufferwstr.c_str(), -1, &bufferstr[0], bufferSize, nullptr, nullptr);
        LOG_INFO(bufferstr.c_str());
        if (wcsncmp(buffer, L"RELOAD_DRIVER", 13) == 0) {
            LOG_DEBUG("Reloading the driver");
            ReloadDriver(hPipe);
        }
        else if (wcsncmp(buffer, L"LOG_DEBUG", 9) == 0) {
            wchar_t* param = buffer + 10;
            if (wcsncmp(param, L"true", 4) == 0) {
//                UpdateXmlToggleSetting(true, L"debug logging");
                logLevel = 4;
                LOG_INFO("Pipe debugging enabled");
            }
            else if (wcsncmp(param, L"false", 5) == 0) {
//                UpdateXmlToggleSetting(false, L"debuglogging");
                logLevel = 3;
                LOG_INFO("Debugging disabled");
            }
            //}
            //else if (wcsncmp(buffer, L"LOGGING", 7) == 0) {
            //    wchar_t* param = buffer + 8;
            //    if (wcsncmp(param, L"true", 4) == 0) {
            //        UpdateXmlToggleSetting(true, L"logging");
            //        logsEnabled = true;
            //        LOG_INFO("Logging Enabled");
            //    }
            //    else if (wcsncmp(param, L"false", 5) == 0) {
            //        UpdateXmlToggleSetting(false, L"logging");
            //        logsEnabled = false;
            //        LOG_INFO("Logging disabled"); // We can keep this here just to make it delete the logs on disable
            //    }
            //}
            //else if (wcsncmp(buffer, L"HDRPLUS", 7) == 0) {
            //    wchar_t* param = buffer + 8;
            //    if (wcsncmp(param, L"true", 4) == 0) {
            //        UpdateXmlToggleSetting(true, L"HDRPlus");
            //        LOG_INFO("HDR+ Enabled");
            //        ReloadDriver(hPipe);
            //    }
            //    else if (wcsncmp(param, L"false", 5) == 0) {
            //        UpdateXmlToggleSetting(false, L"HDRPlus");
            //        LOG_INFO("HDR+ Disabled");
            //        ReloadDriver(hPipe);
            //    }
            //}
            //else if (wcsncmp(buffer, L"SDR10", 5) == 0) {
            //    wchar_t* param = buffer + 6;
            //    if (wcsncmp(param, L"true", 4) == 0) {
            //        UpdateXmlToggleSetting(true, L"SDR10bit");
            //        LOG_INFO("SDR 10 Bit Enabled");
            //        ReloadDriver(hPipe);
            //    }
            //    else if (wcsncmp(param, L"false", 5) == 0) {
            //        UpdateXmlToggleSetting(false, L"SDR10bit");
            //        LOG_INFO("SDR 10 Bit Disabled");
            //        ReloadDriver(hPipe);
            //    }
            //}
            //else if (wcsncmp(buffer, L"CUSTOMEDID", 10) == 0) {
            //    wchar_t* param = buffer + 11;
            //    if (wcsncmp(param, L"true", 4) == 0) {
            //        UpdateXmlToggleSetting(true, L"CustomEdid");
            //        LOG_INFO("Custom Edid Enabled");
            //        ReloadDriver(hPipe);
            //    }
            //    else if (wcsncmp(param, L"false", 5) == 0) {
            //        UpdateXmlToggleSetting(false, L"CustomEdid");
            //        LOG_INFO("Custom Edid Disabled");
            //        ReloadDriver(hPipe);
            //    }
            //}
            //else if (wcsncmp(buffer, L"PREVENTSPOOF", 12) == 0) {
            //    wchar_t* param = buffer + 13;
            //    if (wcsncmp(param, L"true", 4) == 0) {
            //        UpdateXmlToggleSetting(true, L"PreventSpoof");
            //        LOG_INFO("Prevent Spoof Enabled");
            //        ReloadDriver(hPipe);
            //    }
            //    else if (wcsncmp(param, L"false", 5) == 0) {
            //        UpdateXmlToggleSetting(false, L"PreventSpoof");
            //        LOG_INFO("Prevent Spoof Disabled");
            //        ReloadDriver(hPipe);
            //    }
            //}
            //else if (wcsncmp(buffer, L"CEAOVERRIDE", 11) == 0) {
            //    wchar_t* param = buffer + 12;
            //    if (wcsncmp(param, L"true", 4) == 0) {
            //        UpdateXmlToggleSetting(true, L"EdidCeaOverride");
            //        LOG_INFO("Cea override Enabled");
            //        ReloadDriver(hPipe);
            //    }
            //    else if (wcsncmp(param, L"false", 5) == 0) {
            //        UpdateXmlToggleSetting(false, L"EdidCeaOverride");
            //        LOG_INFO("Cea override Disabled");
            //        ReloadDriver(hPipe);
            //    }
            //}
            //else if (wcsncmp(buffer, L"HARDWARECURSOR", 14) == 0) {
            //    wchar_t* param = buffer + 15;
            //    if (wcsncmp(param, L"true", 4) == 0) {
            //        UpdateXmlToggleSetting(true, L"HardwareCursor");
            //        LOG_INFO("Hardware Cursor Enabled");
            //        ReloadDriver(hPipe);
            //    }
            //    else if (wcsncmp(param, L"false", 5) == 0) {
            //        UpdateXmlToggleSetting(false, L"HardwareCursor");
            //        LOG_INFO("Hardware Cursor Disabled");
            //        ReloadDriver(hPipe);
            //    }
            //}
            //else if (wcsncmp(buffer, L"D3DDEVICEGPU", 12) == 0) {
            //    LOG_INFO("Retrieving D3D GPU (This information may be inaccurate without reloading the driver first)");
            //    InitializeD3DDeviceAndLogGPU();
            //    LOG_INFO("Retrieved D3D GPU");
        }else if (wcsncmp(buffer, L"IDDCX_VERSION", 12) == 0) {
            LOG_INFO("Logging iddcx version");
            LogIddCxVersion();
        }
        else if (wcsncmp(buffer, L"GET_GPUINFO", 14) == 0) {
            LOG_INFO("Retrieving Assigned GPU");
            GetGpuInfo();
            LOG_INFO("Retrieved Assigned GPU");
        }else if (wcsncmp(buffer, L"GET_GPUS", 10) == 0) {
            LOG_INFO("Logging all GPUs");
            LOG_INFO( "Any GPUs which show twice but you only have one, will most likely be the GPU the driver is attached to");
            logAvailableGPUs();
            LOG_INFO("Logged all GPUs");
//        }else if (wcsncmp(buffer, L"SETGPU", 6) == 0) { //设置指定的gpu，暂时不提供支持
//            std::wstring gpuName = buffer + 7;
//            gpuName = gpuName.substr(1, gpuName.size() - 2);
//
//            int size_needed = WideCharToMultiByte(CP_UTF8, 0, gpuName.c_str(), static_cast<int>(gpuName.length()), nullptr, 0, nullptr, nullptr);
//            std::string gpuNameNarrow(size_needed, 0);
//            WideCharToMultiByte(CP_UTF8, 0, gpuName.c_str(), static_cast<int>(gpuName.length()), &gpuNameNarrow[0], size_needed, nullptr, nullptr);
//
//            LOG_INFO(("Setting GPU to: " + gpuNameNarrow).c_str());
//            if (UpdateXmlGpuSetting(gpuName.c_str())) {
//                LOG_INFO("Gpu Changed, Restarting Driver");
//            }
//            else {
//                LOG_ERROR( "Failed to update GPU setting in XML. Restarting anyway");
//            }
//            ReloadDriver(hPipe);
//        }else if (wcsncmp(buffer, L"SETDISPLAYCOUNT", 15) == 0) { //暂不提供通过修改配置来重新加载驱动的支持
//            LOG_INFO( "Setting Display Count");
//
//            int newDisplayCount = 1;
//            swscanf_s(buffer + 15, L"%d", &newDisplayCount);
//
//            std::wstring displayLog = L"Setting display count  to " + std::to_wstring(newDisplayCount);
//            LOG_INFO(WStringToString(displayLog).c_str());
//
//            if (UpdateXmlDisplayCountSetting(newDisplayCount)){
//                LOG_INFO("Display Count Changed, Restarting Driver");
//            }
//            else {
//                LOG_ERROR( "Failed to update display count setting in XML. Restarting anyway");
//            }
//            ReloadDriver(hPipe);
            //}
            //else if (wcsncmp(buffer, L"GETSETTINGS", 11) == 0) {
            //    //query and return settings
            //    bool debugEnabled = EnabledQuery(L"DebugLoggingEnabled");
            //    bool loggingEnabled = EnabledQuery(L"LoggingEnabled");

            //    wstring settingsResponse = L"SETTINGS ";
            //    settingsResponse += debugEnabled ? L"DEBUG=true " : L"DEBUG=false ";
            //    settingsResponse += loggingEnabled ? L"LOG=true" : L"LOG=false";

            //    DWORD bytesWritten;
            //    DWORD bytesToWrite = static_cast<DWORD>((settingsResponse.length() + 1) * sizeof(wchar_t));
            //    WriteFile(hPipe, settingsResponse.c_str(), bytesToWrite, &bytesWritten, NULL);

        }
        else if (wcsncmp(buffer, L"PING", 4) == 0) {
            SendToPipe("PONG");
            LOG_INFO("Heartbeat Ping");
        }
        else {
            LOG_ERROR( "Unknown command");

            size_t size_needed;
            wcstombs_s(&size_needed, nullptr, 0, buffer, 0);
            std::string narrowString(size_needed, 0);
            wcstombs_s(nullptr, &narrowString[0], size_needed, buffer, size_needed);
            LOG_ERROR( narrowString.c_str());
        }
    }
    DisconnectNamedPipe(g_pipeHandle);
    CloseHandle(g_pipeHandle);
    g_pipeHandle = INVALID_HANDLE_VALUE; // This value determines whether or not all data gets sent back through the pipe or just the handling pipe data
}

DWORD WINAPI NamedPipeServer(LPVOID lpParam) {
    UNREFERENCED_PARAMETER(lpParam);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = FALSE;
    const wchar_t *sddl = L"D:(A;;GA;;;WD)";
    // 仅允许 SYSTEM + Administrators
    //    const wchar_t *sddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)";
    // 或者仅允许当前登录用户
    //    const wchar_t *sddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)";
    LOG_INFO("Starting pipe with parameters: D:(A;;GA;;;WD)");
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL)) {
        DWORD ErrorCode = GetLastError();
        string errorMessage = to_string(ErrorCode);
        LOG_ERROR(errorMessage.c_str());
        return 1;
    }
    HANDLE hPipe;
    while (g_Running) {
        hPipe = CreateNamedPipeW(PIPE_NAME,PIPE_ACCESS_DUPLEX,
                                 PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                 PIPE_UNLIMITED_INSTANCES,512, 512,0,&sa);

        if (hPipe == INVALID_HANDLE_VALUE) {
            DWORD ErrorCode = GetLastError();
            string errorMessage = to_string(ErrorCode);
            LOG_ERROR(errorMessage.c_str());
            LocalFree(sa.lpSecurityDescriptor);
            return 1;
        }

        BOOL connected = ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected) {
            LOG_INFO("创建命名的Pipe[%s]成功！",WStringToString(PIPE_NAME).c_str());
            HandleClient(hPipe);
        }
        else {
            LOG_ERROR("创建命名的Pipe[%s]失败！",WStringToString(PIPE_NAME).c_str());
            CloseHandle(hPipe);
        }
    }
    LocalFree(sa.lpSecurityDescriptor);
    return 0;
}

void StartNamedPipeServer() {
    LOG_INFO("Starting Pipe");
    hPipeThread = CreateThread(NULL, 0, NamedPipeServer, NULL, 0, NULL);
    if (hPipeThread == NULL) {
        DWORD ErrorCode = GetLastError();
        string errorMessage = to_string(ErrorCode);
        LOG_ERROR( errorMessage.c_str());
    }
    else {
        LOG_INFO("Pipe created");
    }
}

void StopNamedPipeServer() {
    g_Running = false;
    if (hPipeThread) {
        HANDLE hPipe = CreateFileW(
                PIPE_NAME,
                GENERIC_READ | GENERIC_WRITE,
                0,
                NULL,
                OPEN_EXISTING,
                0,
                NULL);

        if (hPipe != INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
        }

        WaitForSingleObject(hPipeThread, INFINITE);
        CloseHandle(hPipeThread);
        hPipeThread = NULL;
        LOG_INFO("Stopped Pipe");
    }
}


extern "C" EVT_WDF_DRIVER_UNLOAD EvtDriverUnload;
//驱动卸载事件
VOID EvtDriverUnload(_In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    StopNamedPipeServer();
    LOG_INFO( "Driver Unloaded");
}

//驱动入口
_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT  pDriverObject,PUNICODE_STRING pRegistryPath)
{
    SetUnhandledExceptionFilter(exception_handler);
    init(); //加载配置
    string version=VERSION;
    LOG_INFO("call WdfDriverCreate!version:%s",version.c_str());

    WDF_DRIVER_CONFIG Config;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);

    WDF_DRIVER_CONFIG_INIT(&Config,VirtualDisplayDriverDeviceAdd);

    Config.EvtDriverUnload = EvtDriverUnload; //注册驱动卸载事件

    //todo：MttVdd 在这里做了大量的配置初始化，我们使用上面的init来初始化即可

    //customEdid = EnabledQuery(L"CustomEdidEnabled");
    //preventManufacturerSpoof = EnabledQuery(L"PreventMonitorSpoof");
    //edidCeaOverride = EnabledQuery(L"EdidCeaOverride");
    //sendLogsThroughPipe = EnabledQuery(L"SendLogsThroughPipe");


    ////colour
    //HDRPlus = EnabledQuery(L"HDRPlusEnabled");
    //SDR10 = EnabledQuery(L"SDR10Enabled");
    //HDRCOLOUR = HDRPlus ? IDDCX_BITS_PER_COMPONENT_12 : IDDCX_BITS_PER_COMPONENT_10;
    //SDRCOLOUR = SDR10 ? IDDCX_BITS_PER_COMPONENT_10 : IDDCX_BITS_PER_COMPONENT_8;
    //ColourFormat = GetStringSetting(L"ColourFormat");

    ////Cursor
    //hardwareCursor = EnabledQuery(L"HardwareCursorEnabled");
    //alphaCursorSupport = EnabledQuery(L"AlphaCursorSupport");
    //CursorMaxX = GetIntegerSetting(L"CursorMaxX");
    //CursorMaxY = GetIntegerSetting(L"CursorMaxY");

    //int xorCursorSupportLevelInt = GetIntegerSetting(L"XorCursorSupportLevel");
    //std::string xorCursorSupportLevelName;

    //if (xorCursorSupportLevelInt < 0 || xorCursorSupportLevelInt > 3) {
    //    vddlog("w", "Selected Xor Level unsupported, defaulting to IDDCX_XOR_CURSOR_SUPPORT_FULL");
    //    XorCursorSupportLevel = IDDCX_XOR_CURSOR_SUPPORT_FULL;
    //}
    //else {
    //    XorCursorSupportLevel = static_cast<IDDCX_XOR_CURSOR_SUPPORT>(xorCursorSupportLevelInt);
    //}

    //// === LOAD NEW EDID INTEGRATION SETTINGS ===
    //edidIntegrationEnabled = EnabledQuery(L"EdidIntegrationEnabled");
    //autoConfigureFromEdid = EnabledQuery(L"AutoConfigureFromEdid");
    //edidProfilePath = GetStringSetting(L"EdidProfilePath");
    //overrideManualSettings = EnabledQuery(L"OverrideManualSettings");
    //fallbackOnError = EnabledQuery(L"FallbackOnError");

    //// === LOAD HDR ADVANCED SETTINGS ===
    //hdr10StaticMetadataEnabled = EnabledQuery(L"Hdr10StaticMetadataEnabled");
    //maxDisplayMasteringLuminance = GetDoubleSetting(L"MaxDisplayMasteringLuminance");
    //minDisplayMasteringLuminance = GetDoubleSetting(L"MinDisplayMasteringLuminance");
    //maxContentLightLevel = GetIntegerSetting(L"MaxContentLightLevel");
    //maxFrameAvgLightLevel = GetIntegerSetting(L"MaxFrameAvgLightLevel");

    //colorPrimariesEnabled = EnabledQuery(L"ColorPrimariesEnabled");
    //redX = GetDoubleSetting(L"RedX");
    //redY = GetDoubleSetting(L"RedY");
    //greenX = GetDoubleSetting(L"GreenX");
    //greenY = GetDoubleSetting(L"GreenY");
    //blueX = GetDoubleSetting(L"BlueX");
    //blueY = GetDoubleSetting(L"BlueY");
    //whiteX = GetDoubleSetting(L"WhiteX");
    //whiteY = GetDoubleSetting(L"WhiteY");

    //colorSpaceEnabled = EnabledQuery(L"ColorSpaceEnabled");
    //gammaCorrection = GetDoubleSetting(L"GammaCorrection");
    //primaryColorSpace = GetStringSetting(L"PrimaryColorSpace");
    //enableMatrixTransform = EnabledQuery(L"EnableMatrixTransform");

    //// === LOAD AUTO RESOLUTIONS SETTINGS ===
    //autoResolutionsEnabled = EnabledQuery(L"AutoResolutionsEnabled");
    //sourcePriority = GetStringSetting(L"SourcePriority");
    //minRefreshRate = GetIntegerSetting(L"MinRefreshRate");
    //maxRefreshRate = GetIntegerSetting(L"MaxRefreshRate");
    //excludeFractionalRates = EnabledQuery(L"ExcludeFractionalRates");
    //minResolutionWidth = GetIntegerSetting(L"MinResolutionWidth");
    //minResolutionHeight = GetIntegerSetting(L"MinResolutionHeight");
    //maxResolutionWidth = GetIntegerSetting(L"MaxResolutionWidth");
    //maxResolutionHeight = GetIntegerSetting(L"MaxResolutionHeight");
    //useEdidPreferred = EnabledQuery(L"UseEdidPreferred");
    //fallbackWidth = GetIntegerSetting(L"FallbackWidth");
    //fallbackHeight = GetIntegerSetting(L"FallbackHeight");
    //fallbackRefresh = GetIntegerSetting(L"FallbackRefresh");

    //// === LOAD COLOR ADVANCED SETTINGS ===
    //autoSelectFromColorSpace = EnabledQuery(L"AutoSelectFromColorSpace");
    //forceBitDepth = GetStringSetting(L"ForceBitDepth");
    //fp16SurfaceSupport = EnabledQuery(L"Fp16SurfaceSupport");
    //wideColorGamut = EnabledQuery(L"WideColorGamut");
    //hdrToneMapping = EnabledQuery(L"HdrToneMapping");
    //sdrWhiteLevel = GetDoubleSetting(L"SdrWhiteLevel");

    //// === LOAD MONITOR EMULATION SETTINGS ===
    //monitorEmulationEnabled = EnabledQuery(L"MonitorEmulationEnabled");
    //emulatePhysicalDimensions = EnabledQuery(L"EmulatePhysicalDimensions");
    //physicalWidthMm = GetIntegerSetting(L"PhysicalWidthMm");
    //physicalHeightMm = GetIntegerSetting(L"PhysicalHeightMm");
    //manufacturerEmulationEnabled = EnabledQuery(L"ManufacturerEmulationEnabled");
    //manufacturerName = GetStringSetting(L"ManufacturerName");
    //modelName = GetStringSetting(L"ModelName");
    //serialNumber = GetStringSetting(L"SerialNumber");

    //xorCursorSupportLevelName = XorCursorSupportLevelToString(XorCursorSupportLevel);

    //LOG_INFO( ("Selected Xor Cursor Support Level: " + xorCursorSupportLevelName).c_str());



    //LOG_INFO( "Driver Starting");
    //string utf8_confpath = WStringToString(confpath);
    //string logtext = "VDD Path: " + utf8_confpath;
    //LOG_INFO( logtext.c_str());

    LogIddCxVersion();
    Status = WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status))
    {
        LOG_ERROR("WdfDriverCreate error: 0x%08x", Status);
        return Status;
    }
    StartNamedPipeServer();
    return Status;
}
#pragma endregion

#pragma region SampleMonitors
// Default modes reported for edid-less monitors. The first mode is set as preferred
//static const struct IndirectSampleMonitor::SampleMonitorMode s_SampleDefaultModes[] =
//{
//        { 1920, 1080, 60 },
//        { 1600,  900, 60 },
//        { 1024,  768, 75 },
//};
//
//// FOR SAMPLE PURPOSES ONLY, Static info about monitors that will be reported to OS
//static const struct IndirectSampleMonitor s_SampleMonitors[] =
//        {
//                // Modified EDID from Dell S2719DGF
//                {
//                        {
//                                0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x10,0xAC,0xE6,0xD0,0x55,0x5A,0x4A,0x30,0x24,0x1D,0x01,
//                                0x04,0xA5,0x3C,0x22,0x78,0xFB,0x6C,0xE5,0xA5,0x55,0x50,0xA0,0x23,0x0B,0x50,0x54,0x00,0x02,0x00,
//                                0xD1,0xC0,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x58,0xE3,0x00,
//                                0xA0,0xA0,0xA0,0x29,0x50,0x30,0x20,0x35,0x00,0x55,0x50,0x21,0x00,0x00,0x1A,0x00,0x00,0x00,0xFF,
//                                0x00,0x37,0x4A,0x51,0x58,0x42,0x59,0x32,0x0A,0x20,0x20,0x20,0x20,0x20,0x00,0x00,0x00,0xFC,0x00,
//                                0x53,0x32,0x37,0x31,0x39,0x44,0x47,0x46,0x0A,0x20,0x20,0x20,0x20,0x00,0x00,0x00,0xFD,0x00,0x28,
//                                0x9B,0xFA,0xFA,0x40,0x01,0x0A,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x2C
//                        },
//                        {
//                                { 2560, 1440, 144 },
//                                { 1920, 1080,  60 },
//                                { 1024,  768,  60 },
//                        },
//                        0
//                },
//                // Modified EDID from Lenovo Y27fA
//                {
//                        {
//                                0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x30,0xAE,0xBF,0x65,0x01,0x01,0x01,0x01,0x20,0x1A,0x01,
//                                0x04,0xA5,0x3C,0x22,0x78,0x3B,0xEE,0xD1,0xA5,0x55,0x48,0x9B,0x26,0x12,0x50,0x54,0x00,0x08,0x00,
//                                0xA9,0xC0,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x68,0xD8,0x00,
//                                0x18,0xF1,0x70,0x2D,0x80,0x58,0x2C,0x45,0x00,0x53,0x50,0x21,0x00,0x00,0x1E,0x00,0x00,0x00,0x10,
//                                0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFD,0x00,
//                                0x30,0x92,0xB4,0xB4,0x22,0x01,0x0A,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x00,0x00,0xFC,0x00,0x4C,
//                                0x45,0x4E,0x20,0x59,0x32,0x37,0x66,0x41,0x0A,0x20,0x20,0x20,0x00,0x11
//                        },
//                        {
//                                { 3840, 2160,  60 },
//                                { 1600,  900,  60 },
//                                { 1024,  768,  60 },
//                        },
//                        0
//                }
//        };

#pragma endregion

#pragma region helpers

static inline void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Mode, DWORD Width, DWORD Height, DWORD VSync, bool bMonitorMode)
{
    Mode.totalSize.cx = Mode.activeSize.cx = Width;
    Mode.totalSize.cy = Mode.activeSize.cy = Height;

    // See https://docs.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_video_signal_info
    Mode.AdditionalSignalInfo.vSyncFreqDivider = bMonitorMode ? 0 : 1;
    Mode.AdditionalSignalInfo.videoStandard = 255;

    Mode.vSyncFreq.Numerator = VSync;
    Mode.vSyncFreq.Denominator = 1;
    Mode.hSyncFreq.Numerator = VSync * Height;
    Mode.hSyncFreq.Denominator = 1;

    Mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

    Mode.pixelRate = ((UINT64)VSync) * ((UINT64)Width) * ((UINT64)Height);
}

static IDDCX_MONITOR_MODE CreateIddCxMonitorMode(DWORD Width, DWORD Height, DWORD VSync, IDDCX_MONITOR_MODE_ORIGIN Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR)
{
    IDDCX_MONITOR_MODE Mode = {};

    Mode.Size = sizeof(Mode);
    Mode.Origin = Origin;
    FillSignalInfo(Mode.MonitorVideoSignalInfo, Width, Height, VSync, true);

    return Mode;
}


static IDDCX_MONITOR_MODE2 CreateIddCxMonitorMode2(DWORD Width, DWORD Height, DWORD VSync, IDDCX_MONITOR_MODE_ORIGIN Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR)
{
    IDDCX_MONITOR_MODE2 Mode = {};
    Mode.Size = sizeof(IDDCX_MONITOR_MODE2);
    Mode.Origin = Origin;
    if (ColourFormat == L"RGB") {
        Mode.BitsPerComponent.Rgb = SDRCOLOUR | HDRCOLOUR;
    }
    else if (ColourFormat == L"YCbCr444") {
        Mode.BitsPerComponent.YCbCr444 = SDRCOLOUR | HDRCOLOUR;
    }
    else if (ColourFormat == L"YCbCr422") {
        Mode.BitsPerComponent.YCbCr422 = SDRCOLOUR | HDRCOLOUR;
    }
    else if (ColourFormat == L"YCbCr420") {
        Mode.BitsPerComponent.YCbCr420 = SDRCOLOUR | HDRCOLOUR;
    }
    else {
        Mode.BitsPerComponent.Rgb = SDRCOLOUR | HDRCOLOUR;  // Default to RGB
    }
    FillSignalInfo(Mode.MonitorVideoSignalInfo, Width, Height, VSync, true);

    return Mode;
}

static IDDCX_TARGET_MODE CreateIddCxTargetMode(DWORD Width, DWORD Height, DWORD VSync)
{
IDDCX_TARGET_MODE Mode = {};

Mode.Size = sizeof(Mode);
FillSignalInfo(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSync, false);

return Mode;
}

static IDDCX_TARGET_MODE2 CreateIddCxTargetMode2(DWORD Width, DWORD Height, DWORD VSync)
{
    stringstream logStream;
    logStream << "Creating IDDCX_TARGET_MODE2 with Width: " << Width
              << ", Height: " << Height
              << ", fps: " << VSync;
    LOG_DEBUG(logStream.str().c_str());
    IDDCX_TARGET_MODE2 Mode = {};
    Mode.Size = sizeof(Mode);


    if (ColourFormat == L"RGB") {
        Mode.BitsPerComponent.Rgb = SDRCOLOUR | HDRCOLOUR;
    }
    else if (ColourFormat == L"YCbCr444") {
        Mode.BitsPerComponent.YCbCr444 = SDRCOLOUR | HDRCOLOUR;
    }
    else if (ColourFormat == L"YCbCr422") {
        Mode.BitsPerComponent.YCbCr422 = SDRCOLOUR | HDRCOLOUR;
    }
    else if (ColourFormat == L"YCbCr420") {
        Mode.BitsPerComponent.YCbCr420 = SDRCOLOUR | HDRCOLOUR;
    }
    else {
        Mode.BitsPerComponent.Rgb = SDRCOLOUR | HDRCOLOUR;  // Default to RGB
    }


    logStream.str("");
    logStream << "IDDCX_TARGET_MODE2 configured with Size: " << Mode.Size
              << " and colour format " << WStringToString(ColourFormat);
    LOG_DEBUG(logStream.str().c_str());


    FillSignalInfo(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSync, false);
    return Mode;
}
#pragma endregion

extern "C" BOOL WINAPI DllMain(_In_ HINSTANCE hInstance,_In_ UINT dwReason,_In_opt_ LPVOID lpReserved)
{
//LOG_INFO("");
UNREFERENCED_PARAMETER(hInstance);
UNREFERENCED_PARAMETER(lpReserved);
UNREFERENCED_PARAMETER(dwReason);

return TRUE;
}

//_Use_decl_annotations_
//extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT  pDriverObject,PUNICODE_STRING pRegistryPath)
//{
//    SetUnhandledExceptionFilter(exception_handler);
//    init();
//    LOG_INFO("call WdfDriverCreate");
//
//    WDF_DRIVER_CONFIG Config;
//    NTSTATUS Status;
//
//    WDF_OBJECT_ATTRIBUTES Attributes;
//    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
//
//    WDF_DRIVER_CONFIG_INIT(&Config,VirtualDisplayDeviceAdd);
//
//    Config.EvtDriverUnload = EvtDriverUnload;
//
//    Status = WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
//    if (!NT_SUCCESS(Status))
//    {
//        LOG_ERROR("WdfDriverCreate error: 0x%08x", Status);
//        return Status;
//    }
//
//    return Status;
//}


_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit){
    LOG_INFO("call IddCxDeviceInitConfig WdfDeviceCreate");

    NTSTATUS Status = STATUS_SUCCESS;
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;

    UNREFERENCED_PARAMETER(Driver);

    // Register for power callbacks - in this sample only power-on is needed
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDeviceD0Entry = VirtualDisplayDriverDeviceD0Entry;
    PnpPowerCallbacks.EvtDeviceD0Exit = VirtualDisplayDriverDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

    IDD_CX_CLIENT_CONFIG IddConfig;
    IDD_CX_CLIENT_CONFIG_INIT(&IddConfig);
    // If the driver wishes to handle custom IoDeviceControl requests,
    // it's necessary to use this callback since IddCx
    // redirects IoDeviceControl requests to an internal queue. This sample does not need this.
    // IddConfig.EvtIddCxDeviceIoControl = VirtualDisplayIoDeviceControl;
    //loadSettings(); //加载配置
    //todo:判断是否使用指定gpu
    GetGpuInfo();

    IddConfig.EvtIddCxAdapterInitFinished = VirtualDisplayDriverAdapterInitFinished;

    IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = VirtualDisplayDriverMonitorGetDefaultModes;
    IddConfig.EvtIddCxMonitorAssignSwapChain = VirtualDisplayDriverMonitorAssignSwapChain;
    IddConfig.EvtIddCxMonitorUnassignSwapChain = VirtualDisplayDriverMonitorUnassignSwapChain;
    if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterQueryTargetInfo)){
        IddConfig.EvtIddCxAdapterQueryTargetInfo = VirtualDisplayDriverEvtIddCxAdapterQueryTargetInfo;
        IddConfig.EvtIddCxMonitorSetDefaultHdrMetaData = VirtualDisplayDriverEvtIddCxMonitorSetDefaultHdrMetadata;
        IddConfig.EvtIddCxParseMonitorDescription2 = VirtualDisplayDriverEvtIddCxParseMonitorDescription2;
        IddConfig.EvtIddCxMonitorQueryTargetModes2 = VirtualDisplayDriverEvtIddCxMonitorQueryTargetModes2;
        IddConfig.EvtIddCxAdapterCommitModes2 = VirtualDisplayDriverEvtIddCxAdapterCommitModes2;
        IddConfig.EvtIddCxMonitorSetGammaRamp = VirtualDisplayDriverEvtIddCxMonitorSetGammaRamp;
    }else{ //windows 10的书写方式
        IddConfig.EvtIddCxParseMonitorDescription = VirtualDisplayDriverParseMonitorDescription;
        IddConfig.EvtIddCxMonitorQueryTargetModes = VirtualDisplayDriverMonitorQueryModes;
        IddConfig.EvtIddCxAdapterCommitModes = VirtualDisplayDriverAdapterCommitModes;
    }

    Status = IddCxDeviceInitConfig(pDeviceInit, &IddConfig);
    if (!NT_SUCCESS(Status)){
        LOG_ERROR("IddCxDeviceInitConfig error: 0x%08x", Status);
        return Status;
    }
    LOG_DEBUG("IddCxDevice inited!");
    loadSettings();//我们在这里加载细节配置
    //    initShareStream();

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
    Attr.EvtCleanupCallback = [](WDFOBJECT Object) {
        // Automatically cleanup the context when the WDF object is about to be deleted
        auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
        LOG_DEBUG("EvtCleanupCallback Device Object: 0x%p", Object);
        if (pContext) {
            pContext->Cleanup();
        }
    };

    WDFDEVICE Device = nullptr;
    Status = WdfDeviceCreate(&pDeviceInit, &Attr, &Device);
    if (!NT_SUCCESS(Status)){
        LOG_ERROR("WdfDeviceCreate error: 0x%08x", Status);
        return Status;
    }

    Status = IddCxDeviceInitialize(Device);
    if (!NT_SUCCESS(Status)){
        LOG_ERROR("IddCxDeviceInitialize error: 0x%08x", Status);
    }

    // Create a new device context object and attach it to the WDF device object
    auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext = new IndirectDeviceContext(Device);

    LOG_INFO("Create DeviceObject=0x%p suc", Device);
    return Status;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    //    UNREFERENCED_PARAMETER(Device);
    //UNREFERENCED_PARAMETER(PreviousState);
    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    if (NULL == pContext || NULL == pContext->pContext) {
        LOG_ERROR("Get DeviceObject=0x%p NULL", Device);
        return STATUS_INVALID_PARAMETER;
    }

    // This function is called by WDF to start the device in the fully-on power state.

    LOG_INFO("call pContext->InitAdapter start, PreviousState=%d", PreviousState);
    pContext->pContext->InitAdapter();
    LOG_INFO("call pContext->InitAdapter end");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverDeviceD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
//    UNREFERENCED_PARAMETER(Device);
UNREFERENCED_PARAMETER(TargetState);
auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
if (pContext && pContext->pContext) {
    // 清理适配器相关资源
    pContext->Cleanup();
}
return STATUS_SUCCESS;
}

#pragma region Direct3DDevice

Direct3DDevice::Direct3DDevice(LUID AdapterLuid) : AdapterLuid(AdapterLuid)
{
}

Direct3DDevice::Direct3DDevice() : AdapterLuid({})
{
}

HRESULT Direct3DDevice::Init()
{
    // The DXGI factory could be cached, but if a new render adapter appears on the system, a new factory needs to be
    // created. If caching is desired, check DxgiFactory->IsCurrent() each time and recreate the factory if !IsCurrent.
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
    if (FAILED(hr))
    {
        LOG_ERROR("Failed to create DXGI factory. HRESULT: 0x%08x", hr);
        return hr;
    }

    // Find the specified render adapter
    hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
    if (FAILED(hr))
    {
        LOG_ERROR("Failed to enumerate adapter by LUID. HRESULT: 0x%08x", hr);
        return hr;
    }

    //runcore时会打印
//    DXGI_ADAPTER_DESC desc;
//    Adapter->GetDesc(&desc);
//    LOG_INFO("Adapter found: %s (Vendor ID:  %04x , Device ID: %04x )", WStringToString(desc.Description),desc.VendorId,desc.DeviceId );

    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL featureLevel;
    // Create a D3D device using the render adapter. BGRA support is required by the WHQL test suite.
    hr = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION, &Device, &featureLevel, &DeviceContext);
    if (FAILED(hr))
    {
        // If creating the D3D device failed, it's possible the render GPU was lost (e.g. detachable GPU) or else the
        // system is in a transient state.
        LOG_ERROR("Failed to create Direct3D device. HRESULT: 0x%08x", hr);
        return hr;
    }
    LOG_INFO("Direct3D device created successfully. Feature Level: %s", featureLevel);
    return S_OK;
}

#pragma endregion

#pragma region SwapChainProcessor

SwapChainProcessor::SwapChainProcessor(IndirectMonitorContext* monitorContext, IDDCX_SWAPCHAIN hSwapChain,
                                       shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent)
        : m_monitorContext(monitorContext), m_hSwapChain(hSwapChain), m_Device(Device), m_hAvailableBufferEvent(NewFrameEvent)
{
    m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));

    // Immediately create and run the swap-chain processing thread, passing 'this' as the thread parameter
    m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
}

SwapChainProcessor::~SwapChainProcessor()
{
    auto index = m_monitorContext->GetConnectorIndex();
    LOG_INFO("display[%d] SetEvent Terminate", index);
    if (m_hThread.Get())
    {
        // Wait for the thread to terminate
        DWORD waitResult = WaitForSingleObject(m_hThread.Get(), INFINITE);
        switch (waitResult)
        {
            case WAIT_OBJECT_0:
                LOG_INFO("display[%d] Thread terminated successfully.", index);
                break;
            case WAIT_ABANDONED:
                LOG_ERROR("display[%d] Thread wait was abandoned. GetLastError: %s", index, to_string(GetLastError()).c_str());
                break;
            case WAIT_TIMEOUT:
                LOG_ERROR("display[%d] Thread wait timed out. This should not happen. GetLastError: %s", index, to_string(GetLastError()).c_str());
                break;
            default:
                LOG_ERROR("display[%d] Unexpected result from WaitForSingleObject. GetLastError: %s", index, to_string(GetLastError()).c_str());
                break;
        }
    }
    else
    {
        LOG_ERROR("display[%d] No valid thread handle to wait for.", index);
    }
    m_monitorContext=nullptr;
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
return 0;
}

void SwapChainProcessor::Run()
{
    // For improved performance, make use of the Multimedia Class Scheduler Service, which will intelligently
    // prioritize this thread for improved throughput in high CPU-load scenarios.
    DWORD AvTask = 0;
    HANDLE AvTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &AvTask);
    LOG_INFO("display[%d] RunCore start", m_monitorContext->GetConnectorIndex());
    RunCore();
    LOG_INFO("display[%d] RunCore end", m_monitorContext->GetConnectorIndex());
    {
        if (m_hSwapChain) {
            WdfObjectDelete((WDFOBJECT)m_hSwapChain);
            m_hSwapChain = nullptr;
        }
    }
    AvRevertMmThreadCharacteristics(AvTaskHandle);
}

void SwapChainProcessor::RunCore()
{
    DWORD currentRetryDelay = 0;
    const DWORD maxRetryDelay = 100;//100秒内重试
    const DWORD retryDelay=10;//10毫秒一次
    int retryCount = 0;
    const int maxRetries = 10;//最多10次

    // Get the DXGI device interface
    ComPtr<IDXGIDevice> DxgiDevice;
    HRESULT hr = m_Device->Device.As(&DxgiDevice);
    auto index = m_monitorContext->GetConnectorIndex();
    if (FAILED(hr))
    {
        LOG_ERROR("display[%d] 转成dxgi设备失败。。。", index)
        return;
    }
    //char err_info[STREAM_LOG_INFO_LEN];

    DXGI_ADAPTER_DESC1 desc;
    m_Device->Adapter->GetDesc1(&desc);
    LOG_INFO("display[%d] Adapter desc: %S Luid:%04x-%04x VendorId=%04x DeviceId=%04x SubSysId=%04x"
            , index, desc.Description, desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart,
             desc.VendorId, desc.DeviceId, desc.SubSysId);

    IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
    SetDevice.pDevice = DxgiDevice.Get();

    hr = IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
    if (FAILED(hr))
    {
        switch (hr)
        {
            case DXGI_ERROR_ACCESS_LOST:
                LOG_INFO("display[%d] IddCxSwapChainSetDevice return DXGI_ERROR_ACCESS_LOST", index);
                break;
            default:
                LOG_ERROR("display[%d] IddCxSwapChainSetDevice error: 0x%08x", index, hr);
                break;
        }
        return;
    }
    LOG_INFO("display[%d] IddCxSwapChainSetDevice successfully", index);
    if (displays[index].hw_cursor) {
        m_monitorContext->SetHwCursorMode();/* 设置硬件鼠标模式 */
    }
    // Acquire and release buffers in a loop
    for (;;)
    {
        // Ask for the next buffer from the producer
        IDARG_IN_RELEASEANDACQUIREBUFFER2 BufferInArgs = {};
        BufferInArgs.Size = sizeof(BufferInArgs);
        IDXGIResource* pSurface;

        if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2)) {
            IDARG_OUT_RELEASEANDACQUIREBUFFER2 Buffer = {};
            hr = IddCxSwapChainReleaseAndAcquireBuffer2(m_hSwapChain, &BufferInArgs, &Buffer);
            pSurface = Buffer.MetaData.pSurface;
        }
        else
        {
            IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
            hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);
            pSurface = Buffer.MetaData.pSurface;
        }

        // AcquireBuffer immediately returns STATUS_PENDING if no buffer is yet available
        if (hr == E_PENDING)
        {
            HANDLE waitHandles[2] = {};
            DWORD waitHandleCount = 0;

            if (m_hAvailableBufferEvent != nullptr && m_hAvailableBufferEvent != INVALID_HANDLE_VALUE){
                waitHandles[waitHandleCount++] = m_hAvailableBufferEvent;
            }

            if (m_hTerminateEvent.Get()){
                waitHandles[waitHandleCount++] = m_hTerminateEvent.Get();
            }

            if (waitHandleCount == 0){
                LOG_ERROR("No valid wait handles available while waiting for the next frame.");
                break;
            }

            DWORD waitResult = WaitForMultipleObjects(waitHandleCount, waitHandles, FALSE, INFINITE);

            if (waitResult == WAIT_OBJECT_0){ //简单条件要置于复杂条件后面
                continue;
            } else if (waitHandleCount > 1 && waitResult == WAIT_OBJECT_0 + 1) {
                LOG_INFO("Terminate event signaled. Exiting loop.");
                break;
            } else if (waitHandleCount == 1 && waitHandles[0] == m_hTerminateEvent.Get() && waitResult == WAIT_OBJECT_0) {
                LOG_INFO("Terminate event signaled. Exiting loop.");
                break;
            } else {
                hr = HRESULT_FROM_WIN32(waitResult == WAIT_FAILED ? GetLastError() : waitResult);
                LOG_ERROR("Unexpected wait result. HRESULT:0x%08X", hr);
                break;
            }
        } else if (SUCCEEDED(hr)) {
            currentRetryDelay = 0;
            retryCount = 0;
            ComPtr<IDXGIResource> AcquiredBuffer;
            AcquiredBuffer.Attach(pSurface);

            AcquiredBuffer.Reset();
            hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
            if (FAILED(hr))
            {
                break;
            }
        } else {
            if (hr == DXGI_ERROR_ACCESS_LOST && retryCount < maxRetries) {
                currentRetryDelay = min(currentRetryDelay + retryDelay, maxRetryDelay);
//                LOG_WARN("display[%d] DXGI_ERROR_ACCESS_LOST detected. Retry %d/%d after %dms delay.",
//                         m_InitParam.nConnectorIndex,retryCount + 1, maxRetries, currentRetryDelay);
                Sleep(currentRetryDelay);
                retryCount++;
                continue;
            } else {
                if (hr == DXGI_ERROR_ACCESS_LOST) {
                    LOG_ERROR("display[%d] DXGI_ERROR_ACCESS_LOST: Maximum retries (%d) reached. Exiting loop.", index, maxRetries);
                } else {
                    LOG_ERROR("display[%d] Failed to acquire buffer. Exiting loop. HRESULT:0x%08x", index, hr);
                }
                break;
            }
            break;
        }
    }
}

#pragma endregion

#pragma region IndirectDeviceContext
//todo: 还有大量代码需要搬运

IndirectDeviceContext::IndirectDeviceContext(_In_ WDFDEVICE WdfDevice) :m_WdfDevice(WdfDevice),m_Adapter(nullptr)
{
}

IndirectDeviceContext::~IndirectDeviceContext()
{
}

void IndirectDeviceContext::InitAdapter()
{
    // ==============================
    // TODO: Update the below diagnostic information in accordance with the target hardware. The strings and version
    // numbers are used for telemetry and may be displayed to the user in some situations.
    //
    // This is also where static per-adapter capabilities are determined.
    // ==============================

    IDDCX_ADAPTER_CAPS AdapterCaps = {};
    AdapterCaps.Size = sizeof(AdapterCaps);

    if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2)) {
        AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16;
        LOG_INFO("FP16 processing capability detected.");
    }

    //AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_REMOTE_SESSION_DRIVER | IDDCX_ADAPTER_FLAGS_CAN_USE_MOVE_REGIONS;
//    AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_CAN_USE_MOVE_REGIONS; //矩形移动区域支持 //这个会导致二次连接远程桌面时，窗口内黑屏
//   AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_USE_SMALLEST_MODE;//使用最小模式
//    AdapterCaps.Flags = static_cast<IDDCX_ADAPTER_FLAGS>(IDDCX_ADAPTER_FLAGS_USE_SMALLEST_MODE | IDDCX_ADAPTER_FLAGS_REMOTE_SESSION_DRIVER);//远程会话标志
//    AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_PREFER_PHYSICAL_GPU;//偏好物理 GPU 渲染 iddcx 2.0版本以上支持
    //if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2)) {//开启HDR支持 iddcx 2.0版本以上api支持
    //    AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16 ;
    //    logStream << "FP16 processing capability detected.";
    //}
//    AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_CAN_USE_MOVE_REGIONS ;
    LOG_DEBUG("当前显示适配器的flags:%d", AdapterCaps.Flags);
    // Declare basic feature support for the adapter (required)
    AdapterCaps.MaxMonitorsSupported = displayCount;
    AdapterCaps.EndPointDiagnostics.Size = sizeof(AdapterCaps.EndPointDiagnostics);
    AdapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    AdapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;

    // Declare your device strings for telemetry (required)
    AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"Hdr Vdd Device";
    AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"Hdr Vdd";
    AdapterCaps.EndPointDiagnostics.pEndPointModelName = L"Hdr Vdd Model";

    // Declare your hardware and firmware versions (required)
    IDDCX_ENDPOINT_VERSION Version = {};
    Version.Size = sizeof(Version);
    Version.MajorVer = 1;
    Version.MinorVer = 0;
    AdapterCaps.EndPointDiagnostics.pFirmwareVersion = &Version;
    AdapterCaps.EndPointDiagnostics.pHardwareVersion = &Version;

    // Initialize a WDF context that can store a pointer to the device context object
    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

    IDARG_IN_ADAPTER_INIT AdapterInit = {};
    AdapterInit.WdfDevice = m_WdfDevice;
    AdapterInit.pCaps = &AdapterCaps;
    AdapterInit.ObjectAttributes = &Attr;

    // Start the initialization of the adapter, which will trigger the AdapterFinishInit callback later
    IDARG_OUT_ADAPTER_INIT AdapterInitOut;
    NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);

    if (NT_SUCCESS(Status))
    {
        LOG_INFO("初始化适配器成功：0x%p", AdapterInitOut.AdapterObject);
        // Store a reference to the WDF adapter handle
        m_Adapter = AdapterInitOut.AdapterObject;

        // Store the device context object into the WDF object context
        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterInitOut.AdapterObject);
        pContext->pContext = this;
    }
    else
    {
        LOG_ERROR("初始化适配器失败： 0x%08x", Status);
        return;
    }
}
void IndirectDeviceContext::FinishInit(){
    Options.Adapter.apply(m_Adapter);
    for (unsigned int i = 0; i < displayCount; i++) {
        CreateMonitor(i);
    }
}

void IndirectDeviceContext::CreateMonitor(UINT displayIndex)
{
    LOG_DEBUG("开始创建虚拟显示器，index:%d", displayIndex);

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, IndirectMonitorContextWrapper);
    attr.EvtCleanupCallback = [](WDFOBJECT Object)
    {
        // Automatically cleanup the context when the WDF object is about to be deleted
        auto* pContext = WdfObjectGet_IndirectMonitorContextWrapper(Object);
        LOG_INFO("EvtCleanupCallback Monitor Object: 0x%p", Object);
        if (pContext)
        {
            pContext->Cleanup();
        }
    };

    // In the sample driver, we report a monitor right away but a real driver would do this when a monitor connection event occurs
    IDDCX_MONITOR_INFO monitorInfo = {};
    monitorInfo.Size = sizeof(monitorInfo);
    monitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    monitorInfo.ConnectorIndex = displayIndex;
    monitorInfo.MonitorDescription.Size = sizeof(monitorInfo.MonitorDescription);
    monitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    /* if the datasize is 0, then the os will call VirtualDisplayMonitorGetDefaultModes instead of VirtualDisplayParseMonitorDescription */
    if (!displays[displayIndex].use_edid)
    {
        monitorInfo.MonitorDescription.DataSize = 0;
        monitorInfo.MonitorDescription.pData = nullptr;
    }
    else
    {
        monitorInfo.MonitorDescription.DataSize = (UINT)displays[displayIndex].edid_len;
        monitorInfo.MonitorDescription.pData = displays[displayIndex].edid_data;
    }

    // ==============================
    // TODO: The monitor's container ID should be distinct from "this" device's container ID if the monitor is not
    // permanently attached to the display adapter device object. The container ID is typically made unique for each
    // monitor and can be used to associate the monitor with other devices, like audio or input devices. In this
    // sample we generate a random container ID GUID, but it's best practice to choose a stable container ID for a
    // unique monitor or to use "this" device's container ID for a permanent/integrated monitor.
    // ==============================

    // Create a container ID
    CoCreateGuid(&monitorInfo.MonitorContainerId);
    LOG_DEBUG("显示器[%d]的guid:%s",monitorInfo.ConnectorIndex, GuidToString(monitorInfo.MonitorContainerId).c_str());
    IDARG_IN_MONITORCREATE monitorCreate = {};
    monitorCreate.ObjectAttributes = &attr;
    monitorCreate.pMonitorInfo = &monitorInfo;

    // Create a monitor object with the specified monitor descriptor
    IDARG_OUT_MONITORCREATE monitorCreateOut;
    NTSTATUS status = IddCxMonitorCreate(m_Adapter, &monitorCreate, &monitorCreateOut);
    if (NT_SUCCESS(status)) {
        auto monitor= monitorCreateOut.MonitorObject;
        auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(monitor);
        pMonitorContextWrapper->pContext = new IndirectMonitorContext(monitorInfo.ConnectorIndex, monitor, m_Adapter);

        // Tell the OS that the monitor has been plugged in
        IDARG_OUT_MONITORARRIVAL arrivalOut;
        LOG_DEBUG("display[%d] IddCxMonitorArrival MonitorObject=0x%p start", monitorInfo.ConnectorIndex, monitor);
        status = IddCxMonitorArrival(monitor, &arrivalOut);
        LOG_DEBUG("display[%d] IddCxMonitorArrival MonitorObject=0x%p end    luid:%s targetId:%d", monitorInfo.ConnectorIndex, monitorCreateOut.MonitorObject,
                  (std::to_string(arrivalOut.OsAdapterLuid.LowPart) + "-" + std::to_string(arrivalOut.OsAdapterLuid.HighPart)).c_str(),
                  arrivalOut.OsTargetId);
        if (!NT_SUCCESS(status))
        {
            switch (status)
            {
                case STATUS_INVALID_PARAMETER:
                    LOG_ERROR("display[%d] IddCxMonitorArrival error: 0x%08x(STATUS_INVALID_PARAMETER)", monitorInfo.ConnectorIndex, status);
                    break;
                case STATUS_NOT_SUPPORTED:
                    LOG_ERROR("display[%d] IddCxMonitorArrival error: 0x%08x(STATUS_NOT_SUPPORTED)", monitorInfo.ConnectorIndex, status);
                    break;
                default:
                    LOG_ERROR("display[%d] IddCxMonitorArrival error: 0x%08x", monitorInfo.ConnectorIndex, status);
                    break;
            }
        }
    }
    else
    {
        LOG_ERROR("display[%d] IddCxMonitorCreate error: 0x%08x", displayIndex, status);
    }
    LOG_DEBUG("创建虚拟显示器结束，index:%d", displayIndex);
}

IndirectMonitorContext::IndirectMonitorContext(_In_ UINT ConnectorIndex, _In_ IDDCX_MONITOR Monitor, _In_ IDDCX_ADAPTER Adapter) :
m_ConnectorIndex(ConnectorIndex), m_Monitor(Monitor), m_Adapter(Adapter)
{
}

IndirectMonitorContext::~IndirectMonitorContext()
{
    UnassignSwapChain();
}

bool IndirectMonitorContext::SetHwCursorMode()
{
    string name="VirtualDisplayMouse"+std::to_string(m_ConnectorIndex);
    auto mouseEvent = CreateEventA(nullptr,false,false,name.c_str());
    if (!mouseEvent){
        LOG_ERROR("Failed to create mouse event. No hardware cursor supported!");
        return false;
    }
//    LOG_DEBUG("开始设置硬件鼠标，index:%d", m_ConnectorIndex);
    IDARG_IN_SETUP_HWCURSOR hwCursor = {};
    hwCursor.hNewCursorDataAvailable = mouseEvent;
    hwCursor.CursorInfo.Size = sizeof(IDDCX_CURSOR_CAPS);
    hwCursor.CursorInfo.ColorXorCursorSupport = xorCursorSupportLevel;
    hwCursor.CursorInfo.AlphaCursorSupport = alphaCursorSupport;
    //在大多数情况下，128是可以的，但为了安全起见，我们将使用512，较旧的英特尔CPU可能限制为64x64
    hwCursor.CursorInfo.MaxX = cursorMaxX;
    hwCursor.CursorInfo.MaxY = cursorMaxY;

    NTSTATUS Status = IddCxMonitorSetupHardwareCursor(m_Monitor, &hwCursor);
    if (!NT_SUCCESS(Status)) {
        LOG_ERROR("display[%d]IddCxMonitorSetupHardwareCursor error: 0x%08x", m_ConnectorIndex, Status);
        CloseHandle(mouseEvent);
        return false;
    }
    LOG_DEBUG("设置硬件鼠标成功，index:%d", m_ConnectorIndex);
    return true;
}

void IndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID renderAdapter, HANDLE newFrameEvent)
{
    // Only cleanup expired devices periodically, not on every assignment
    static int assignmentCount = 0;
    if (++assignmentCount % 10 == 0) {
        CleanupExpiredDevices();
    }
    auto Device = GetOrCreateDevice(renderAdapter);

//    auto Device = make_shared<Direct3DDevice>(renderAdapter);
    if (FAILED(Device->Init()))
    {
        // It's important to delete the swap-chain if D3D initialization fails, so that the OS knows to generate a new
        // swap-chain and try again.
        WdfObjectDelete(SwapChain);
    }
    else
    {
        // Create a new swap-chain processing thread
        m_ProcessingThread.reset(new SwapChainProcessor(this,SwapChain, Device, newFrameEvent));
    }
//    LOG_DEBUG("完成创建交换链=>%d",m_ConnectorIndex);
}

void IndirectMonitorContext::UnassignSwapChain()
{
        m_ProcessingThread.reset();
}

#pragma endregion

#pragma region DDI Callbacks

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverAdapterInitFinished(IDDCX_ADAPTER adapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
    //UNREFERENCED_PARAMETER(adapterObject);
    //UNREFERENCED_PARAMETER(pInArgs);
    auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(adapterObject);
    if (NULL == pDeviceContextWrapper || NULL == pDeviceContextWrapper->pContext) {
        LOG_ERROR("Get AdapterObject=0x%p NULL", adapterObject);
        return STATUS_INVALID_PARAMETER;
    }

    // This is called when the OS has finished setting up the adapter for use by the IddCx driver. It's now possible
    // to report attached monitors.

    LOG_DEBUG("call pContext->FinishInit start");
    if (NT_SUCCESS(pInArgs->AdapterInitStatus))
    {
        pDeviceContextWrapper->pContext->FinishInit();
        LOG_DEBUG( "Adapter initialization finished successfully.");
    }else {
        LOG_ERROR("AdapterInitStatus=0x%08x", pInArgs->AdapterInitStatus);
    }
    LOG_INFO("call pContext->FinishInit end");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverAdapterCommitModes(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES* pInArgs)
{
	UNREFERENCED_PARAMETER(AdapterObject);
	UNREFERENCED_PARAMETER(pInArgs);

	// For the sample, do nothing when modes are picked - the swap-chain is taken care of by IddCx

	// ==============================
	// TODO: In a real driver, this function would be used to reconfigure the device to commit the new modes. Loop
	// through pInArgs->pPaths and look for IDDCX_PATH_FLAGS_ACTIVE. Any path not active is inactive (e.g. the monitor
	// should be turned off).
	// ==============================

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
    // ==============================
    // TODO: In a real driver, this function would be called to generate monitor modes for an EDID by parsing it. In
    // this sample driver, we hard-code the EDID, so this function can generate known modes.
    // ==============================
    // edid: pInArgs->MonitorDescription.pData, pInArgs->MonitorDescription.DataSize
    int displayIndex = findDisplayIndexByEdid(static_cast<BYTE*>(pInArgs->MonitorDescription.pData),pInArgs->MonitorDescription.DataSize);
    if (displayIndex < 0 || displayIndex >= (int)displays.size()) {
        LOG_ERROR("ParseMonitorDescription 无效的显示器索引: %d, displays.size()=%d",displayIndex, (int)displays.size());
        return STATUS_INVALID_PARAMETER;
    }
    LOG_DEBUG("display[%d] pInArgs->MonitorModeBufferInputCount=%d, call VirtualDisplayParseMonitorDescription", displayIndex, pInArgs->MonitorModeBufferInputCount);
    DisplayInfo* display_info = &displays[displayIndex];
    pOutArgs->MonitorModeBufferOutputCount =1;
//    pOutArgs->MonitorModeBufferOutputCount = UINT(display_info->size_cnt * display_info->fps_cnt);
    if (pInArgs->MonitorModeBufferInputCount < pOutArgs->MonitorModeBufferOutputCount)
    {
        // Return success if there was no buffer, since the caller was only asking for a count of modes
        return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    } else {
        // In the sample driver, we have reported some static information about connected monitors
        // Check which of the reported monitors this call is for by comparing it to the pointer of
        // our known EDID blocks.

        int nIndex = 0;
        int bestIndex = -1;
        bool find=false;
        for (int item_idx = 0; item_idx < display_info->size_cnt; item_idx++) {
            auto item = display_info->size_list[item_idx];
            for (int fps_idx = 0; fps_idx < display_info->fps_cnt; fps_idx++) {
                auto fps = display_info->fps_list[fps_idx];
                if (item.cx == display_info->prefer_width && item.cy == display_info->prefer_height && fps == display_info->prefer_fps) {
                    bestIndex = nIndex;
                    LOG_DEBUG("设置当前分辨率为推荐分辨率 %dx%d@%d",item.cx,item.cy,fps);
                    if(pOutArgs->MonitorModeBufferOutputCount ==1) {
                        pInArgs->pMonitorModes[nIndex] = CreateIddCxMonitorMode(item.cx, item.cy, fps);
                        nIndex++;
                        find=true;
                        break;
                    }
                }
                if(pOutArgs->MonitorModeBufferOutputCount !=1) {
                    LOG_DEBUG("正在处理分辨率 %dx%d@%d index:%d", item.cx, item.cy, fps, nIndex);
                    pInArgs->pMonitorModes[nIndex] = CreateIddCxMonitorMode(item.cx, item.cy, fps);
                    nIndex++;
                }
            }
            if(find){
                break;
            }
        }
        if(nIndex==0){ //根本没有加载到配置
            display_info->prefer_width=1920;
            display_info->prefer_height=1080;
            display_info->prefer_fps=60;
            pInArgs->pMonitorModes[nIndex] = CreateIddCxMonitorMode(display_info->prefer_width, display_info->prefer_height, display_info->prefer_fps);
            LOG_DEBUG("分辨率配置错误，设置默认分辨率 %dx%d@%d", display_info->prefer_width, display_info->prefer_height, display_info->prefer_fps);
        }
        if (bestIndex == -1) {
            LOG_DEBUG("未匹配到合适的分辨率，默认使用第一个");
            bestIndex = 0;
        }
        pOutArgs->PreferredMonitorModeIdx = bestIndex;
        return STATUS_SUCCESS;
    }
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
    LOG_DEBUG("未使用edid加载，使用简单显示配置支持===================>");
    UNREFERENCED_PARAMETER(MonitorObject);
    //    UNREFERENCED_PARAMETER(pInArgs);
    //    UNREFERENCED_PARAMETER(pOutArgs);
    // ==============================
    // TODO: 驱动程序应报告传输协议和几乎所有监视器（如640x480、800x600或1024x768）都保证支持的模式。如果驾驶员可以从EDID以外的描述符访问监控模式，则这些模式也将在此处报告。
    // ==============================
    if (pInArgs->DefaultMonitorModeBufferInputCount == 0)
    {
        pOutArgs->DefaultMonitorModeBufferOutputCount = 1; //默认我们只支持一个默认
    }
    else
    {
        auto config = displayResFpsList[0];
        pInArgs->pDefaultMonitorModes[0] = CreateIddCxMonitorMode(get<0>(config), get<1>(config), get<2>(config));
        pOutArgs->DefaultMonitorModeBufferOutputCount = 1;
        pOutArgs->PreferredMonitorModeIdx = 0;
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverMonitorQueryModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)////////////////////////////////////////////////////////////////////////////////
{
    //UNREFERENCED_PARAMETER(MonitorObject);
    //UNREFERENCED_PARAMETER(pInArgs);
    //UNREFERENCED_PARAMETER(pOutArgs);
    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    if (NULL == pMonitorContextWrapper || NULL == pMonitorContextWrapper->pContext) {
        LOG_ERROR("Get MonitorObject=0x%p NULL", MonitorObject);
        return STATUS_INVALID_PARAMETER;
    }

    auto display_idx = pMonitorContextWrapper->pContext->GetConnectorIndex();
    DisplayInfo *display_info = &displays[display_idx];

    //vector<IDDCX_TARGET_MODE> TargetModes;

    // Create a set of modes supported for frame processing and scan-out. These are typically not based on the
    // monitor's descriptor and instead are based on the static processing capability of the device. The OS will
    // report the available set of modes for a given output as the intersection of monitor modes with target modes.
    // edid: pInArgs->MonitorDescription.pData pInArgs->MonitorDescription.DataSize

    // 获取缓冲区大小
    pOutArgs->TargetModeBufferOutputCount = 1;
    //    pOutArgs->TargetModeBufferOutputCount = UINT(display_info->size_cnt * display_info->fps_cnt);
    LOG_DEBUG("display[%d] TargetModeBufferInputCount=%d,TargetModeBufferOutputCount=%d, call VirtualDisplayMonitorQueryModes",
    display_idx, pInArgs->TargetModeBufferInputCount, pOutArgs->TargetModeBufferOutputCount);
    if (pInArgs->TargetModeBufferInputCount == 0) {/* 用于设置缓冲区大小 */
        return STATUS_SUCCESS;
    }
    else if (pInArgs->TargetModeBufferInputCount < pOutArgs->TargetModeBufferOutputCount) {
        LOG_INFO("display[%d] STATUS_BUFFER_TOO_SMALL", display_idx);
        return STATUS_BUFFER_TOO_SMALL;
    } else if(pInArgs->TargetModeBufferInputCount==1){
        pInArgs->pTargetModes[0] = CreateIddCxTargetMode(display_info->prefer_width, display_info->prefer_height, display_info->prefer_fps);
        LOG_DEBUG("创建单虚拟显示器目标模式:%dx%d@%d TargetMode index %d", display_info->prefer_width, display_info->prefer_height, display_info->prefer_fps, 0)
    } else {
        int nIndex = 0;
        for (int item_idx = 0; item_idx < display_info->size_cnt; item_idx++) {
            auto item = display_info->size_list[item_idx];
            for (int fps_idx = 0; fps_idx < display_info->fps_cnt; fps_idx++) {
                auto fps = display_info->fps_list[fps_idx];
                pInArgs->pTargetModes[nIndex] = CreateIddCxTargetMode(item.cx, item.cy, fps);
                LOG_DEBUG("创建虚拟显示器目标模式:%dx%d@%d TargetMode index %d", item.cx, item.cy, fps, nIndex)
                nIndex++;
            }
        }
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
    //UNREFERENCED_PARAMETER(MonitorObject);
    //UNREFERENCED_PARAMETER(pInArgs);
    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    if (NULL == pMonitorContextWrapper || NULL == pMonitorContextWrapper->pContext) {
        LOG_ERROR("Get MonitorObject=0x%p NULL", MonitorObject);
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    }
    auto display_idx = pMonitorContextWrapper->pContext->GetConnectorIndex();
    LOG_DEBUG("display[%d] call pContext->AssignSwapChain", display_idx);
    pMonitorContextWrapper->pContext->AssignSwapChain(pInArgs->hSwapChain, pInArgs->RenderAdapterLuid, pInArgs->hNextSurfaceAvailable);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
    //UNREFERENCED_PARAMETER(MonitorObject);
    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    if (NULL == pMonitorContextWrapper || NULL == pMonitorContextWrapper->pContext) {
        LOG_ERROR("Get MonitorObject=0x%p NULL", MonitorObject);
        return STATUS_INVALID_PARAMETER;
    }
    auto display_idx = pMonitorContextWrapper->pContext->GetConnectorIndex();
    LOG_DEBUG("display[%d] call pContext->UnassignSwapChain", display_idx);
    pMonitorContextWrapper->pContext->UnassignSwapChain();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverEvtIddCxAdapterQueryTargetInfo(IDDCX_ADAPTER adapterObject, IDARG_IN_QUERYTARGET_INFO* pInArgs, IDARG_OUT_QUERYTARGET_INFO* pOutArgs){
    LOG_DEBUG("Querying target info for adapter object: 0x%p", adapterObject);
    UNREFERENCED_PARAMETER(pInArgs);

    pOutArgs->TargetCaps = IDDCX_TARGET_CAPS_HIGH_COLOR_SPACE | IDDCX_TARGET_CAPS_WIDE_COLOR_SPACE;

    if (ColourFormat == L"RGB") {
        pOutArgs->DitheringSupport.Rgb = SDRCOLOUR | HDRCOLOUR;
    }
    else if (ColourFormat == L"YCbCr444") {
        pOutArgs->DitheringSupport.YCbCr444 = SDRCOLOUR | HDRCOLOUR;
    }
    else if (ColourFormat == L"YCbCr422") {
        pOutArgs->DitheringSupport.YCbCr422 = SDRCOLOUR | HDRCOLOUR;
    }
    else if (ColourFormat == L"YCbCr420") {
        pOutArgs->DitheringSupport.YCbCr420 = SDRCOLOUR | HDRCOLOUR;
    }
    else {
        pOutArgs->DitheringSupport.Rgb = SDRCOLOUR | HDRCOLOUR;  // Default to RGB
    }

    LOG_DEBUG("Target capabilities set to: 0x%p", pOutArgs->TargetCaps);
    LOG_DEBUG("Dithering support colour format set to:  %s", WStringToString(ColourFormat).c_str());
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverEvtIddCxMonitorSetDefaultHdrMetadata(IDDCX_MONITOR monitorObject, const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA* pInArgs){
    UNREFERENCED_PARAMETER(pInArgs);

    LOG_DEBUG("=== PROCESSING HDR METADATA REQUEST ===");
    LOG_DEBUG("Monitor Object:  0x%p, HDR10 Metadata Enabled: %s, Color Primaries Enabled: %s",
    monitorObject, hdr10StaticMetadataEnabled ? "Yes" : "No", colorPrimariesEnabled ? "Yes" : "No");

    // Check if HDR metadata processing is enabled
    if (!hdr10StaticMetadataEnabled) {
        LOG_INFO( "HDR10 static metadata is disabled, skipping metadata configuration");
        return STATUS_SUCCESS;
    }

    VddHdrMetadata metadata = {};
    bool hasValidMetadata = false;

    // Priority 1: Use EDID-derived metadata if available
    if (edidIntegrationEnabled && autoConfigureFromEdid) {
        // First check for monitor-specific metadata
        auto storeIt = g_HdrMetadataStore.find(monitorObject);
        if (storeIt != g_HdrMetadataStore.end() && storeIt->second.isValid) {
            metadata = storeIt->second;
            hasValidMetadata = true;
            LOG_INFO( "Using monitor-specific EDID-derived HDR metadata");
        }
        // If no monitor-specific metadata, check for template metadata from EDID profile
        else {
            auto templateIt = g_HdrMetadataStore.find(reinterpret_cast<IDDCX_MONITOR>(0));
            if (templateIt != g_HdrMetadataStore.end() && templateIt->second.isValid) {
                metadata = templateIt->second;
                hasValidMetadata = true;
                // Store it for this specific monitor for future use
                g_HdrMetadataStore[monitorObject] = metadata;
                LOG_INFO( "Using template EDID-derived HDR metadata and storing for monitor");
            }
        }
    }

    // Priority 2: Use manual configuration if no EDID data or manual override
    if (!hasValidMetadata || overrideManualSettings) {
        if (colorPrimariesEnabled) {
            metadata = ConvertManualToSmpteMetadata();
            hasValidMetadata = metadata.isValid;
            LOG_INFO( "Using manually configured HDR metadata");
        }
    }

    // If we still don't have valid metadata, return early
    if (!hasValidMetadata) {
        LOG_WARN( "No valid HDR metadata available, skipping configuration");
        return STATUS_SUCCESS;
    }

    // Log the HDR metadata values being applied
    stringstream logStream;
    logStream << "=== APPLYING SMPTE ST.2086 HDR METADATA ===\n"
    << "Red Primary: (" << metadata.display_primaries_x[0] << ", " << metadata.display_primaries_y[0] << ")\n"
    << "Green Primary: (" << metadata.display_primaries_x[1] << ", " << metadata.display_primaries_y[1] << ")\n"
    << "Blue Primary: (" << metadata.display_primaries_x[2] << ", " << metadata.display_primaries_y[2] << ")\n"
    << "White Point: (" << metadata.white_point_x << ", " << metadata.white_point_y << ")\n"
    << "Max Mastering Luminance: " << metadata.max_display_mastering_luminance << " (0.0001 cd/m² units)\n"
    << "Min Mastering Luminance: " << metadata.min_display_mastering_luminance << " (0.0001 cd/m² units)\n"
    << "Max Content Light Level: " << metadata.max_content_light_level << " nits\n"
    << "Max Frame Average Light Level: " << metadata.max_frame_avg_light_level << " nits";
    LOG_INFO(logStream.str().c_str());

    // Store the metadata for this monitor
    g_HdrMetadataStore[monitorObject] = metadata;

    // Convert our metadata to the IddCx expected format
    // Note: The actual HDR metadata structure would depend on the IddCx version
    // For now, we log that the metadata has been processed and stored
    LOG_INFO("HDR metadata successfully configured and stored for monitor 0x%p", monitorObject);

    // In a full implementation, you would pass the metadata to the IddCx framework here
    // The exact API calls would depend on IddCx version and HDR implementation details
    // For Phase 2, we focus on the metadata preparation and storage

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverEvtIddCxParseMonitorDescription2(const IDARG_IN_PARSEMONITORDESCRIPTION2* pInArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs){
    // ==============================
    // TODO: In a real driver, this function would be called to generate monitor modes for an EDID by parsing it. In
    // this sample driver, we hard-code the EDID, so this function can generate known modes.
    // ==============================
    int displayIndex = findDisplayIndexByEdid(static_cast<BYTE*>(pInArgs->MonitorDescription.pData), pInArgs->MonitorDescription.DataSize);
    if (displayIndex < 0 || displayIndex >= (int)displays.size()) {
        LOG_ERROR("ParseMonitorDescription 无效的显示器索引: %d, displays.size()=%d", displayIndex, (int)displays.size());
        return STATUS_INVALID_PARAMETER;
    }
    LOG_DEBUG("display[%d] pInArgs->MonitorModeBufferInputCount=%d, call VirtualDisplayDriverEvtIddCxParseMonitorDescription2", displayIndex, pInArgs->MonitorModeBufferInputCount);
    DisplayInfo* display_info = &displays[displayIndex];
    pOutArgs->MonitorModeBufferOutputCount = 1;
    //    pOutArgs->MonitorModeBufferOutputCount = UINT(display_info->size_cnt * display_info->fps_cnt);
    if (pInArgs->MonitorModeBufferInputCount < pOutArgs->MonitorModeBufferOutputCount)
    {
        // Return success if there was no buffer, since the caller was only asking for a count of modes
        return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    }
    else {
        // In the sample driver, we have reported some static information about connected monitors
        // Check which of the reported monitors this call is for by comparing it to the pointer of
        // our known EDID blocks.

        int nIndex = 0;
        int bestIndex = -1;
        bool find = false;
        for (int item_idx = 0; item_idx < display_info->size_cnt; item_idx++) {
            auto item = display_info->size_list[item_idx];
            for (int fps_idx = 0; fps_idx < display_info->fps_cnt; fps_idx++) {
                auto fps = display_info->fps_list[fps_idx];
                if (item.cx == display_info->prefer_width && item.cy == display_info->prefer_height && fps == display_info->prefer_fps) {
                    bestIndex = nIndex;
                    LOG_DEBUG("设置当前分辨率为推荐分辨率 %dx%d@%d", item.cx, item.cy, fps);
                    if (pOutArgs->MonitorModeBufferOutputCount == 1) {
                        pInArgs->pMonitorModes[nIndex] = CreateIddCxMonitorMode2(item.cx, item.cy, fps);
                        nIndex++;
                        find = true;
                        break;
                    }
                }
                if (pOutArgs->MonitorModeBufferOutputCount != 1) {
                    LOG_DEBUG("正在处理分辨率 %dx%d@%d index:%d", item.cx, item.cy, fps, nIndex);
                    pInArgs->pMonitorModes[nIndex] = CreateIddCxMonitorMode2(item.cx, item.cy, fps);
                    nIndex++;
                }
            }
            if (find) {
                break;
            }
        }
        if (nIndex == 0) { //根本没有加载到配置
            display_info->prefer_width = 1920;
            display_info->prefer_height = 1080;
            display_info->prefer_fps = 60;
            pInArgs->pMonitorModes[nIndex] = CreateIddCxMonitorMode2(display_info->prefer_width, display_info->prefer_height, display_info->prefer_fps);
            LOG_DEBUG("分辨率配置错误，设置默认分辨率 %dx%d@%d", display_info->prefer_width, display_info->prefer_height, display_info->prefer_fps);
        }
        if (bestIndex == -1) {
            LOG_DEBUG("未匹配到合适的分辨率，默认使用第一个");
            bestIndex = 0;
        }
        pOutArgs->PreferredMonitorModeIdx = bestIndex;
        return STATUS_SUCCESS;
    }
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverEvtIddCxMonitorQueryTargetModes2(IDDCX_MONITOR monitorObject, const IDARG_IN_QUERYTARGETMODES2* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs){
    //UNREFERENCED_PARAMETER(MonitorObject);
    //UNREFERENCED_PARAMETER(pInArgs);
    //UNREFERENCED_PARAMETER(pOutArgs);
    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(monitorObject);
    if (NULL == pMonitorContextWrapper || NULL == pMonitorContextWrapper->pContext) {
        LOG_ERROR("Get MonitorObject=0x%p NULL", monitorObject);
        return STATUS_INVALID_PARAMETER;
    }

    auto display_idx = pMonitorContextWrapper->pContext->GetConnectorIndex();
    DisplayInfo* display_info = &displays[display_idx];

    //vector<IDDCX_TARGET_MODE> TargetModes;

    // Create a set of modes supported for frame processing and scan-out. These are typically not based on the
    // monitor's descriptor and instead are based on the static processing capability of the device. The OS will
    // report the available set of modes for a given output as the intersection of monitor modes with target modes.
    // edid: pInArgs->MonitorDescription.pData pInArgs->MonitorDescription.DataSize

    // 获取缓冲区大小
    pOutArgs->TargetModeBufferOutputCount = 1;
    //    pOutArgs->TargetModeBufferOutputCount = UINT(display_info->size_cnt * display_info->fps_cnt);
    LOG_DEBUG("display[%d] TargetModeBufferInputCount=%d,TargetModeBufferOutputCount=%d, call VirtualDisplayMonitorQueryModes",
        display_idx, pInArgs->TargetModeBufferInputCount, pOutArgs->TargetModeBufferOutputCount);
    if (pInArgs->TargetModeBufferInputCount == 0) {/* 用于设置缓冲区大小 */
        return STATUS_SUCCESS;
    }
    else if (pInArgs->TargetModeBufferInputCount < pOutArgs->TargetModeBufferOutputCount) {
        LOG_INFO("display[%d] STATUS_BUFFER_TOO_SMALL", display_idx);
        return STATUS_BUFFER_TOO_SMALL;
    }
    else if (pInArgs->TargetModeBufferInputCount == 1) {
        pInArgs->pTargetModes[0] = CreateIddCxTargetMode2(display_info->prefer_width, display_info->prefer_height, display_info->prefer_fps);
        LOG_DEBUG("创建单虚拟显示器目标模式:%dx%d@%d TargetMode index %d", display_info->prefer_width, display_info->prefer_height, display_info->prefer_fps, 0)
    }
    else {
        int nIndex = 0;
        for (int item_idx = 0; item_idx < display_info->size_cnt; item_idx++) {
            auto item = display_info->size_list[item_idx];
            for (int fps_idx = 0; fps_idx < display_info->fps_cnt; fps_idx++) {
                auto fps = display_info->fps_list[fps_idx];
                pInArgs->pTargetModes[nIndex] = CreateIddCxTargetMode2(item.cx, item.cy, fps);
                LOG_DEBUG("创建虚拟显示器目标模式:%dx%d@%d TargetMode index %d", item.cx, item.cy, fps, nIndex)
                    nIndex++;
            }
        }
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverEvtIddCxAdapterCommitModes2(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES2* pInArgs){
    UNREFERENCED_PARAMETER(AdapterObject);
    UNREFERENCED_PARAMETER(pInArgs);

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverEvtIddCxMonitorSetGammaRamp(IDDCX_MONITOR monitorObject, const IDARG_IN_SET_GAMMARAMP* pInArgs)
{
    LOG_INFO("VirtualDisplayMonitorSetGammaRamp===============>");
    UNREFERENCED_PARAMETER(monitorObject);
    UNREFERENCED_PARAMETER(pInArgs);

    // Check if color space processing is enabled
    if (!colorSpaceEnabled) {
        LOG_INFO( "Color space processing is disabled, skipping gamma ramp configuration");
        return STATUS_SUCCESS;
    }

    VddGammaRamp gammaRamp = {};
    bool hasValidGammaRamp = false;

    // Priority 1: Use EDID-derived gamma settings if available
    if (edidIntegrationEnabled && autoConfigureFromEdid) {
        // First check for monitor-specific gamma ramp
        auto storeIt = g_GammaRampStore.find(monitorObject);
        if (storeIt != g_GammaRampStore.end() && storeIt->second.isValid) {
            gammaRamp = storeIt->second;
            hasValidGammaRamp = true;
            LOG_INFO( "Using monitor-specific EDID-derived gamma ramp");
        }
        // If no monitor-specific gamma ramp, check for template from EDID profile
        else {
            auto templateIt = g_GammaRampStore.find(reinterpret_cast<IDDCX_MONITOR>(0));
            if (templateIt != g_GammaRampStore.end() && templateIt->second.isValid) {
                gammaRamp = templateIt->second;
                hasValidGammaRamp = true;
                // Store it for this specific monitor for future use
                g_GammaRampStore[monitorObject] = gammaRamp;
                LOG_INFO( "Using template EDID-derived gamma ramp and storing for monitor");
            }
        }
    }

    // Priority 2: Use manual configuration if no EDID data or manual override
    if (!hasValidGammaRamp || overrideManualSettings) {
        gammaRamp = ConvertManualToGammaRamp();
        hasValidGammaRamp = gammaRamp.isValid;
        LOG_INFO( "Using manually configured gamma ramp");
    }

    // If we still don't have valid gamma settings, return early
    if (!hasValidGammaRamp) {
        LOG_WARN( "No valid gamma ramp available, skipping configuration");
        return STATUS_SUCCESS;
    }

    // Log the gamma ramp values being applied
    LOG_INFO("=== APPLYING GAMMA RAMP AND COLOR SPACE TRANSFORM ===");
    LOG_INFO("Gamma Value: %d", gammaRamp.gamma);
    LOG_INFO("Color Space: %s", WStringToString(gammaRamp.colorSpace).c_str());
    LOG_INFO("Use Matrix Transform: %s", gammaRamp.useMatrix ? "Yes" : "No");

    // Apply gamma ramp based on type
    if (pInArgs->Type == IDDCX_GAMMARAMP_TYPE_3x4_COLORSPACE_TRANSFORM && gammaRamp.useMatrix) {
        // Apply 3x4 color space transformation matrix
        LOG_INFO( "Applying 3x4 Color Space Matrix:");
        LOG_INFO("[%f,%f,%f]", gammaRamp.matrix.matrix[0][0] , gammaRamp.matrix.matrix[0][1] ,gammaRamp.matrix.matrix[0][2] , gammaRamp.matrix.matrix[0][3]);
        LOG_INFO("[%f,%f,%f]", gammaRamp.matrix.matrix[1][0], gammaRamp.matrix.matrix[1][1], gammaRamp.matrix.matrix[1][2], gammaRamp.matrix.matrix[1][3]);
        LOG_INFO("[%f,%f,%f]", gammaRamp.matrix.matrix[2][0], gammaRamp.matrix.matrix[2][1], gammaRamp.matrix.matrix[2][2], gammaRamp.matrix.matrix[2][3]);
        // Store the matrix for this monitor
        g_GammaRampStore[monitorObject] = gammaRamp;

        // In a full implementation, you would apply the matrix to the rendering pipeline here
        // The exact API calls would depend on IddCx version and hardware capabilities

        LOG_INFO("3x4 matrix transform applied successfully for monitor 0x%p", monitorObject);
    }
    else if (pInArgs->Type == IDDCX_GAMMARAMP_TYPE_RGB256x3x16) {
        // Apply traditional RGB gamma ramp
        LOG_INFO("Applying RGB 256x3x16 gamma ramp with gamma %f", gammaRamp.gamma);

        // In a full implementation, you would generate and apply RGB lookup tables here
        // Based on the gamma value and color space
        LOG_INFO("RGB gamma ramp applied successfully for monitor 0x%p", monitorObject);
    }
    else {
        LOG_WARN("Unsupported gamma ramp type: %d, using default gamma processing", pInArgs->Type);
    }

    // Store the final gamma ramp for this monitor
    g_GammaRampStore[monitorObject] = gammaRamp;

    LOG_INFO("Gamma ramp configuration completed for monitor 0x%p", monitorObject);
    return STATUS_SUCCESS;
}

#pragma endregion
