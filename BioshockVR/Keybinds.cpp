// BioshockVR/Keybinds.cpp
//
// See Keybinds.h. Names are parsed case-insensitively so "num8", "NUM8" and
// "Num8" are all the same key -- users type what they type.

#include "Keybinds.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

extern void LogFile(const char* msg);

static void Log(const char* fmt, ...)
{
    char b[512];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a);
    va_end(a);
    LogFile(b);
}

struct KeyDef
{
    const char* iniName;     // key written in [KEYS]
    const char* defName;     // default binding, as a name
    int         vk;          // resolved at Key_Init
};

// Order MUST match the KeyId enum.
static KeyDef g_keys[KEY_COUNT] = {
    { "GripForwardPlus",   "NUM8",   0 },
    { "GripForwardMinus",  "NUM2",   0 },
    { "GripRightPlus",     "NUM6",   0 },
    { "GripRightMinus",    "NUM4",   0 },
    { "GripUpPlus",        "NUM0",   0 },
    { "GripUpMinus",       "NUM5",   0 },
    { "GripCycleMode",     "NUM9",   0 },
    { "GripCycleStep",     "NUM7",   0 },

    { "HudCycleParam",     "DELETE", 0 },
    { "HudDecrease",       "F11",    0 },
    { "HudIncrease",       "F12",    0 },

    { "Recenter",          "NUMDEC", 0 },
    { "AimCandidate",      "NUMADD", 0 },

    { "HudRedirectToggle", "HOME",   0 },

    { "DrawClear",         "NUMMUL", 0 },
    { "DrawTable",         "NUM3",   0 },
    { "DrawSuppress",      "NUM1",   0 },
    { "DrawIsolateNext",   "NUMSUB", 0 },
    { "DrawIsolateOff",    "NUMDIV", 0 },
    { "DrawFrameDump",     "NONE",   0 },

    { "ProbeSnapshot",     "PGUP",   0 },
    { "ProbeDiff",         "PGDN",   0 },
    { "HandsSnapshot",     "NONE",   0 },
    { "HandsDiff",         "NONE",   0 },
};

struct NameVk { const char* name; int vk; };

// Every name a user might reasonably type. Numpad names come first so the
// common case resolves quickly.
static const NameVk kNames[] = {
    { "NUM0", VK_NUMPAD0 }, { "NUM1", VK_NUMPAD1 }, { "NUM2", VK_NUMPAD2 },
    { "NUM3", VK_NUMPAD3 }, { "NUM4", VK_NUMPAD4 }, { "NUM5", VK_NUMPAD5 },
    { "NUM6", VK_NUMPAD6 }, { "NUM7", VK_NUMPAD7 }, { "NUM8", VK_NUMPAD8 },
    { "NUM9", VK_NUMPAD9 },
    { "NUMADD", VK_ADD }, { "NUMSUB", VK_SUBTRACT },
    { "NUMMUL", VK_MULTIPLY }, { "NUMDIV", VK_DIVIDE },
    { "NUMDEC", VK_DECIMAL },

    { "F1", VK_F1 }, { "F2", VK_F2 }, { "F3", VK_F3 }, { "F4", VK_F4 },
    { "F5", VK_F5 }, { "F6", VK_F6 }, { "F7", VK_F7 }, { "F8", VK_F8 },
    { "F9", VK_F9 }, { "F10", VK_F10 }, { "F11", VK_F11 }, { "F12", VK_F12 },

    { "PGUP", VK_PRIOR }, { "PAGEUP", VK_PRIOR },
    { "PGDN", VK_NEXT },  { "PAGEDOWN", VK_NEXT },
    { "HOME", VK_HOME },  { "END", VK_END },
    { "INS", VK_INSERT }, { "INSERT", VK_INSERT },
    { "DEL", VK_DELETE }, { "DELETE", VK_DELETE },
    { "UP", VK_UP }, { "DOWN", VK_DOWN }, { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT },
    { "TAB", VK_TAB }, { "SPACE", VK_SPACE }, { "BACKSPACE", VK_BACK },
    { "LBRACKET", VK_OEM_4 }, { "RBRACKET", VK_OEM_6 },
    { "SEMICOLON", VK_OEM_1 }, { "QUOTE", VK_OEM_7 },
    { "COMMA", VK_OEM_COMMA }, { "PERIOD", VK_OEM_PERIOD },
    { "SLASH", VK_OEM_2 }, { "BACKSLASH", VK_OEM_5 },
    { "MINUS", VK_OEM_MINUS }, { "EQUALS", VK_OEM_PLUS },
    { "TILDE", VK_OEM_3 }, { "GRAVE", VK_OEM_3 },

    { "NONE", 0 }, { "OFF", 0 },
};

// "NUM8" / "F11" / "A" / "7" / "0x68" -> VK. Returns -1 on a name we do not
// recognise, which the caller reports rather than silently swallowing.
static int NameToVk(const char* s)
{
    if (!s || !s[0]) return -1;

    while (*s == ' ' || *s == '\t') ++s;

    char up[32] = {};
    int n = 0;
    for (; s[n] && n < 31; ++n)
        up[n] = (char)toupper((unsigned char)s[n]);
    while (n > 0 && (up[n - 1] == ' ' || up[n - 1] == '\t')) up[--n] = 0;
    if (!up[0]) return -1;

    // Raw hex, for anything not in the table.
    if (up[0] == '0' && up[1] == 'X')
    {
        int v = (int)strtol(up + 2, nullptr, 16);
        return (v > 0 && v < 256) ? v : -1;
    }

    // Single letter or digit on the main keyboard.
    if (!up[1])
    {
        if (up[0] >= 'A' && up[0] <= 'Z') return up[0];
        if (up[0] >= '0' && up[0] <= '9') return up[0];
        return -1;
    }

    for (int i = 0; i < (int)(sizeof(kNames) / sizeof(kNames[0])); ++i)
        if (_stricmp(up, kNames[i].name) == 0) return kNames[i].vk;

    return -1;
}

static const char* VkToName(int vk)
{
    if (vk == 0) return "none";
    for (int i = 0; i < (int)(sizeof(kNames) / sizeof(kNames[0])); ++i)
        if (kNames[i].vk == vk) return kNames[i].name;

    static char one[2] = {};
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
    {
        one[0] = (char)vk; one[1] = 0;
        return one;
    }
    return "?";
}

void Key_Init(const char* iniPath)
{
    Log("=== BioshockVR keybinds ===");

    for (int i = 0; i < KEY_COUNT; ++i)
    {
        KeyDef& k = g_keys[i];

        char buf[64] = {};
        GetPrivateProfileStringA("KEYS", k.iniName, "", buf, sizeof(buf), iniPath);

        if (!buf[0])
        {
            k.vk = NameToVk(k.defName);          // not set -- use the default
            continue;
        }

        const int vk = NameToVk(buf);
        if (vk < 0)
        {
            k.vk = NameToVk(k.defName);
            Log("!!! keys: %s = '%s' is not a key name I know. Using default %s.",
                k.iniName, buf, k.defName);
            Log("!!! keys: valid examples -- NUM8  F11  PGUP  DELETE  HOME  K  0x68");
            continue;
        }

        k.vk = vk;
    }

    // Duplicate detection. Two functions on one key means one of them silently
    // never fires, which is exactly the class of bug this file exists to kill.
    for (int i = 0; i < KEY_COUNT; ++i)
    {
        if (!g_keys[i].vk) continue;
        for (int j = i + 1; j < KEY_COUNT; ++j)
            if (g_keys[i].vk == g_keys[j].vk)
                Log("!!! keys: %s and %s are BOTH bound to %s -- they will fight.",
                    g_keys[i].iniName, g_keys[j].iniName, VkToName(g_keys[i].vk));
    }

    for (int i = 0; i < KEY_COUNT; ++i)
        Log("  key %-18s = %s", g_keys[i].iniName, VkToName(g_keys[i].vk));
}

bool Key_Down(KeyId id)
{
    if (id < 0 || id >= KEY_COUNT) return false;
    const int vk = g_keys[id].vk;
    if (!vk) return false;                        // deliberately unbound
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool Key_Fired(KeyId id, bool& prev)
{
    const bool down = Key_Down(id);
    const bool fired = down && !prev;
    prev = down;
    return fired;
}

int Key_Vk(KeyId id)
{
    if (id < 0 || id >= KEY_COUNT) return 0;
    return g_keys[id].vk;
}

const char* Key_Name(KeyId id)
{
    if (id < 0 || id >= KEY_COUNT) return "?";
    return VkToName(g_keys[id].vk);
}
