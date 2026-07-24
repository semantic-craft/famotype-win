#include "stdafx.h"
#include "Globals.h"

// ════════════════════════════════════════════════════════════════════════
// 法墨 fork overlay —— 替换上游 WeaselTSF/Globals.cpp 的 TSF identity GUID。
// 上游：rime/weasel @ 93eec2dc（GPL-3.0）。仅替换法墨自有的 4+1 个 GUID；
// 系统/共享 GUID（GUID_TFCAT_*、GUID_LBI_INPUTMODE）保持不变。
// GUID 值来自 famo-identity.json，由本任务新生成，与日用 Weasel 完全不相交。
// 由 apply-famo-identity.ps1 拷贝覆盖到上游 checkout。
// ════════════════════════════════════════════════════════════════════════

HINSTANCE g_hInst;

LONG g_cRefDll = -1;

CRITICAL_SECTION g_cs;

// 法墨 TSF text service CLSID {54EAD76A-B864-4A6D-9C82-148E3352BEE7}
static const GUID c_clsidTextService = {
    0x54ead76a,
    0xb864,
    0x4a6d,
    {0x9c, 0x82, 0x14, 0x8e, 0x33, 0x52, 0xbe, 0xe7}};

// 法墨 profile {0158C2BA-4E96-4BA8-B505-E1BBEBB3FA33}
static const GUID c_guidProfile = {
    0x0158c2ba,
    0x4e96,
    0x4ba8,
    {0xb5, 0x05, 0xe1, 0xbb, 0xeb, 0xb3, 0xfa, 0x33}};

// 法墨 language bar item button {903A6C32-FD79-4DE5-AACC-3740594D312A}
static const GUID c_guidLangBarItemButton = {
    0x903a6c32,
    0xfd79,
    0x4de5,
    {0xaa, 0xcc, 0x37, 0x40, 0x59, 0x4d, 0x31, 0x2a}};

// 法墨 display attribute (input) {AC217550-D35E-49C1-97DD-D5ED1415BAC1}
static const GUID c_guidDisplayAttributeInput = {
    0xac217550,
    0xd35e,
    0x49c1,
    {0x97, 0xdd, 0xd5, 0xed, 0x14, 0x15, 0xba, 0xc1}};

#ifdef WEASEL_USING_OLDER_TSF_SDK

/* For Windows 8 —— 系统分类 GUID，保持不变 */
const GUID GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT = {
    0x13A016DF,
    0x560B,
    0x46CD,
    {0x94, 0x7A, 0x4C, 0x3A, 0xF1, 0xE0, 0xE3, 0x5D}};

const GUID GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT = {
    0x25504FB4,
    0x7BAB,
    0x4BC1,
    {0x9C, 0x69, 0xCF, 0x81, 0x89, 0x0F, 0x0E, 0xF5}};

#endif

// 系统 langbar input-mode GUID，保持不变
const GUID GUID_LBI_INPUTMODE = {
    0x2C77A81E,
    0x41CC,
    0x4178,
    {0xA3, 0xA7, 0x5F, 0x8A, 0x98, 0x75, 0x68, 0xE6}};

// 法墨 preserved key（模式切换热键）—— 独立 GUID，避免与 Weasel 撞键
// {E0C4D79C-1D37-4929-87F9-D1476A3FBF03}
const GUID GUID_IME_MODE_PRESERVED_KEY = {
    0xe0c4d79c,
    0x1d37,
    0x4929,
    {0x87, 0xf9, 0xd1, 0x47, 0x6a, 0x3f, 0xbf, 0x03}};
