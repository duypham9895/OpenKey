// Standalone harness for the shared OpenKey engine (Sources/OpenKey/engine).
//
// Feeds one keystroke sequence into vKeyHandleEvent() and applies the engine's
// output (backspaces + charData / macroData) to a text buffer the same way the
// macOS host does in OpenKey.mm (Unicode code table):
//   - vWillProcess: delete backspaceCount, append charData[newCharCount-1 .. 0]
//   - vRestore / vRestoreAndStartNewSession: same, then re-send the typed key
//   - vReplaceMaro: delete backspaceCount, append macroData, then re-send the
//     trigger key (OpenKey.mm handleMacro)
//
// One process runs exactly ONE sequence, because startNewSession() does not
// clear every piece of engine state (e.g. hMacroKey); sharing a process between
// rows would leak state from one row into the next.
//
// Usage: engine_harness [-oNAME=VALUE ...] KEYS
//   KEYS: letters, digits and punctuation as typed; ' ' = space, '<' = backspace.
// Output (one line): TEXT<TAB>bs=<synthetic backspaces><TAB>mk=<macro key buffer>
#include "Engine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// Engine configuration globals. The hosts (macOS AppDelegate.m, Windows
// AppDelegate.cpp) define these; the defaults below mirror the macOS defaults
// except vLanguage (always Vietnamese here) and vUseMacro (on, so macro rows run).
int vLanguage = 1, vInputType = 0, vFreeMark = 0, vCodeTable = 0, vSwitchKeyStatus = 0,
    vCheckSpelling = 1, vUseModernOrthography = 1, vQuickTelex = 0, vRestoreIfWrongSpelling = 0,
    vFixRecommendBrowser = 1, vUseMacro = 1, vUseMacroInEnglishMode = 1, vAutoCapsMacro = 0,
    vUseSmartSwitchKey = 1, vUpperCaseFirstChar = 0, vTempOffSpelling = 0, vAllowConsonantZFWJ = 0,
    vQuickStartConsonant = 0, vQuickEndConsonant = 0, vRememberCode = 1, vOtherLanguage = 1,
    vTempOffOpenKey = 0;

extern Uint16 keyCodeToCharacter(const Uint32& keyCode);

static vKeyHookState* pData;
static std::u16string text;
static int backspaces = 0;

static const Uint16 kLetters[26] = {KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
                                    KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
                                    KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z};
static const Uint16 kDigits[10] = {KEY_0, KEY_1, KEY_2, KEY_3, KEY_4,
                                   KEY_5, KEY_6, KEY_7, KEY_8, KEY_9};

static bool mapKey(char c, Uint16& key, bool& shift) {
    shift = false;
    if (c >= 'a' && c <= 'z') { key = kLetters[c - 'a']; return true; }
    if (c >= 'A' && c <= 'Z') { key = kLetters[c - 'A']; shift = true; return true; }
    if (c >= '0' && c <= '9') { key = kDigits[c - '0']; return true; }
    switch (c) {
        case ' ': key = KEY_SPACE; return true;
        case '.': key = KEY_DOT; return true;
        case ',': key = KEY_COMMA; return true;
        case '[': key = KEY_LEFT_BRACKET; return true;
        case ']': key = KEY_RIGHT_BRACKET; return true;
        case '/': key = KEY_SLASH; return true;
        case ';': key = KEY_SEMICOLON; return true;
        case '\'': key = KEY_QUOTE; return true;
        case '-': key = KEY_MINUS; return true;
        case '=': key = KEY_EQUALS; return true;
    }
    return false;
}

// A keycode without CHAR_CODE_MASK is sent by the host as a raw key event.
static char16_t keyToChar(Uint32 data) {
    if (data & CHAR_CODE_MASK) return (char16_t)(data & 0xFFFF);
    Uint16 ch = keyCodeToCharacter(data & 0xFFFF);
    if (!ch) return u'?';
    if ((data & CAPS_MASK) && ch >= 'a' && ch <= 'z') ch -= 32;
    return (char16_t)ch;
}

static void deleteChars(int n) {
    backspaces += n;
    while (n-- > 0 && !text.empty()) text.pop_back();
}

static void appendCharData() {
    for (int i = pData->newCharCount - 1; i >= 0; i--) text += keyToChar(pData->charData[i]);
}

static void typeSequence(const char* keys) {
    for (const char* p = keys; *p; p++) {
        Uint16 key;
        bool shift;
        char16_t typed = 0;
        if (*p == '<') {
            key = KEY_DELETE;
            shift = false;
        } else {
            if (!mapKey(*p, key, shift)) { fprintf(stderr, "unmapped key '%c'\n", *p); exit(2); }
            typed = keyToChar(key | (shift ? CAPS_MASK : 0));
        }
        vKeyHandleEvent(Keyboard, KeyDown, key, shift ? 1 : 0, false);
        switch (pData->code) {
            case vDoNothing:
                if (key == KEY_DELETE) deleteChars(1); else text += typed;
                break;
            case vWillProcess:
            case vRestore:
            case vRestoreAndStartNewSession:
                deleteChars(pData->backspaceCount);
                appendCharData();
                if (pData->code != vWillProcess && key != KEY_DELETE) text += typed;
                if (pData->code == vRestoreAndStartNewSession) startNewSession();
                break;
            case vReplaceMaro:
                deleteChars(pData->backspaceCount);
                for (Uint32 c : pData->macroData) text += keyToChar(c);
                if (key != KEY_DELETE) text += typed;  // host re-sends the trigger key
                break;
            default:
                if (key != KEY_DELETE) text += typed;
        }
    }
}

static std::string toUtf8(const std::u16string& s) {
    std::string out;
    for (char16_t c : s) {
        unsigned v = c;
        if (v < 0x80) out += (char)v;
        else if (v < 0x800) { out += (char)(0xC0 | (v >> 6)); out += (char)(0x80 | (v & 63)); }
        else { out += (char)(0xE0 | (v >> 12)); out += (char)(0x80 | ((v >> 6) & 63)); out += (char)(0x80 | (v & 63)); }
    }
    return out;
}

int main(int argc, char** argv) {
    const char* keys = nullptr;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'o') {
            char name[64];
            int value, used = 0;
            // Reject anything after the number, e.g. two options passed as one argument.
            if (sscanf(argv[i] + 2, "%63[^=]=%d%n", name, &value, &used) != 2 || argv[i][2 + used] != '\0') {
                fprintf(stderr, "bad option '%s' (one -oNAME=VALUE per argument)\n", argv[i]);
                return 2;
            }
#define OPT(x) if (!strcmp(name, #x)) { x = value; continue; }
            OPT(vInputType) OPT(vCheckSpelling) OPT(vUseModernOrthography) OPT(vQuickTelex)
            OPT(vRestoreIfWrongSpelling) OPT(vUseMacro) OPT(vUpperCaseFirstChar) OPT(vAllowConsonantZFWJ)
            OPT(vQuickStartConsonant) OPT(vQuickEndConsonant) OPT(vFreeMark)
#undef OPT
            fprintf(stderr, "unknown option %s\n", name);
            return 2;
        }
        keys = argv[i];
    }
    if (!keys) { fprintf(stderr, "usage: engine_harness [-oNAME=VALUE ...] KEYS\n"); return 2; }

    pData = (vKeyHookState*)vKeyInit();
    // Fixed macro table used by the macro rows in cases.tsv.
    addMacro("r", "rồi");
    addMacro("tv", "tiếng Việt");
    addMacro("hươu", "DEER");
    addMacro("hương", "SCENT");
    addMacro("thương", "LOVE");
    addMacro("ưd", "UD");
    startNewSession();

    typeSequence(keys);

    std::u16string macroKey;
    for (Uint32 c : pData->macroKey) macroKey += keyToChar(c);
    printf("%s\tbs=%d\tmk=%s\n", toUtf8(text).c_str(), backspaces, toUtf8(macroKey).c_str());
    return 0;
}
