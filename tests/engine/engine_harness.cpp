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
// With -ovLanguage=0 (English mode) the harness calls vEnglishMode() instead,
// like OpenKey.mm: the host merges Shift and Caps Lock into one isCaps flag, and
// on vReplaceMaro it deletes, types the macro and re-sends the trigger key.
//
// Usage: engine_harness [-oNAME=VALUE ...] KEYS
//   KEYS: letters, digits and punctuation as typed; ' ' = space, '<' = backspace,
//         '|' = TAB, '^' = Caps Lock toggle (sends capsStatus=2: letters type
//         uppercase, a digit types as the digit). '"' '(' ')' are Shift+quote,
//         Shift+9, Shift+0.
// Output (one line): TEXT<TAB>bs=<synthetic backspaces><TAB>mk=<macro key buffer>
//   A TAB in TEXT or mk= is printed as '|'. A key the engine stores with
//   CAPS_MASK is printed shifted, so in mk= an engine KEY_9|CAPS_MASK shows as
//   "(" even when Caps Lock (not Shift) produced it: the engine stores both alike.
#include "Engine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// Engine configuration globals. The hosts (macOS AppDelegate.m, Windows
// AppDelegate.cpp) define these; the defaults below mirror the macOS defaults
// except vLanguage (Vietnamese unless -ovLanguage=0) and vUseMacro (on, so macro
// rows run).
int vLanguage = 1, vInputType = 0, vFreeMark = 0, vCodeTable = 0, vSwitchKeyStatus = 0,
    vCheckSpelling = 1, vUseModernOrthography = 1, vQuickTelex = 0, vRestoreIfWrongSpelling = 0,
    vFixRecommendBrowser = 1, vUseMacro = 1, vUseMacroInEnglishMode = 1, vAutoCapsMacro = 0,
    vUseSmartSwitchKey = 1, vUpperCaseFirstChar = 0, vTempOffSpelling = 0, vAllowConsonantZFWJ = 0,
    vQuickStartConsonant = 0, vQuickEndConsonant = 0, vRememberCode = 1, vOtherLanguage = 1,
    vTempOffOpenKey = 0;

// Defined by engines that have the "TAB triggers macro" option (upstream issue
// 200). Weak, so the harness still links against an engine without it; then
// &vMacroOnTab is null and -ovMacroOnTab is rejected.
extern int vMacroOnTab __attribute__((weak));

extern Uint16 keyCodeToCharacter(const Uint32& keyCode);

static vKeyHookState* pData;
static std::u16string text;
static int backspaces = 0;
static bool capsLock = false;

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
        case '`': key = KEY_BACKQUOTE; return true;
        case '"': key = KEY_QUOTE; shift = true; return true;
        case '(': key = KEY_9; shift = true; return true;
        case ')': key = KEY_0; shift = true; return true;
        case '|': key = KEY_TAB; return true;
    }
    return false;
}

static void deleteChars(int n) {
    backspaces += n;
    while (n-- > 0 && !text.empty()) text.pop_back();
}

// A keycode without CHAR_CODE_MASK is sent by the host as a raw key event;
// with CAPS_MASK the host sends it with Shift.
static char16_t keyToChar(Uint32 data) {
    if (data & CHAR_CODE_MASK) return (char16_t)(data & 0xFFFF);
    if ((data & 0xFFFF) == KEY_TAB) return u'|';
    Uint16 ch = keyCodeToCharacter(data & 0xFFFF);
    if (!ch) return u'?';
    if (data & CAPS_MASK) {
        const Uint16 shifted = keyCodeToCharacter((data & 0xFFFF) | CAPS_MASK);
        if (shifted) ch = shifted;
    }
    return (char16_t)ch;
}

// What the OS types for a key the host lets through unchanged.
static char16_t typedChar(Uint16 key, bool shift) {
    if (shift) return keyToChar(key | CAPS_MASK);
    const char16_t ch = keyToChar(key);
    if (capsLock && ch >= u'a' && ch <= u'z') return (char16_t)(ch - 32);  // Caps Lock: letters only
    return ch;
}

// English mode (vLanguage == 0), as OpenKey.mm: the engine only tracks the
// macro key; every key is passed through unless a macro replaces the word.
static void typeEnglishKey(Uint16 key, bool shift, char16_t typed) {
    pData->code = vDoNothing;
    if (vUseMacro && vUseMacroInEnglishMode)
        vEnglishMode(KeyDown, key, shift || capsLock, false);
    if (pData->code == vReplaceMaro) {
        deleteChars(pData->backspaceCount);
        for (Uint32 c : pData->macroData) text += keyToChar(c);
        if (key != KEY_DELETE) text += typed;  // host re-sends the trigger key
    } else if (key == KEY_DELETE) {
        deleteChars(1);
    } else {
        text += typed;
    }
}

static void appendCharData() {
    for (int i = pData->newCharCount - 1; i >= 0; i--) text += keyToChar(pData->charData[i]);
}

static void typeSequence(const char* keys) {
    for (const char* p = keys; *p; p++) {
        Uint16 key;
        bool shift;
        char16_t typed = 0;
        if (*p == '^') { capsLock = !capsLock; continue; }
        if (*p == '<') {
            key = KEY_DELETE;
            shift = false;
        } else {
            if (!mapKey(*p, key, shift)) { fprintf(stderr, "unmapped key '%c'\n", *p); exit(2); }
            typed = typedChar(key, shift);
        }
        if (vLanguage == 0) { typeEnglishKey(key, shift, typed); continue; }
        // OpenKey.mm: capsStatus 1 = Shift, 2 = Caps Lock (Shift wins).
        vKeyHandleEvent(Keyboard, KeyDown, key, shift ? 1 : (capsLock ? 2 : 0), false);
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
            OPT(vQuickStartConsonant) OPT(vQuickEndConsonant) OPT(vFreeMark) OPT(vLanguage)
#undef OPT
            if (!strcmp(name, "vMacroOnTab")) {
                if (&vMacroOnTab == nullptr) {
                    fprintf(stderr, "option vMacroOnTab: this engine does not define vMacroOnTab\n");
                    return 2;
                }
                vMacroOnTab = value;
                continue;
            }
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
    addMacro("xd", "xây dựng");
    addMacro("test", "TEST");
    startNewSession();

    typeSequence(keys);

    std::u16string macroKey;
    for (Uint32 c : pData->macroKey) macroKey += keyToChar(c);
    printf("%s\tbs=%d\tmk=%s\n", toUtf8(text).c_str(), backspaces, toUtf8(macroKey).c_str());
    return 0;
}
