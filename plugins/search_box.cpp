// Search Box -- a search field on the garage part list, the decal picker and the vendor list, as
// plugins\mt_plugin_SearchBox.dll. With hundreds of engines and attachments those lists
// have no other way to find anything.
//
// All of them are a ListView of plain item objects, in a VerticalBox. The game's own
// W_StringSearchBox goes into that box. Each frame-ish tick, its text filters the list
// through ListView.SetListItems. When the game rebuilds a list (category change, part
// installed), the items no longer match what was shown, so the new set is taken and
// filtered again.
//
// A ListView is the only thing keeping its items alive, so a filtered-out item would be
// garbage collected. All of them stay in a second, collapsed ListView in the same box.
//
// Matching: every word typed must appear in the row key or the display name, in any order.
// Decal rows have no name, so those match on the key alone (GeomShape_01_003-star).
//
// shortcut: the key offsets below come from the code that fills the items, and the box
// lands under the list because AddChild only appends. An update moving the keys only
// matches less (an item with no key is always shown); check here after a game update.

#include <cwctype>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include "../include/mt_plugin.hpp"

using namespace mt;

struct Kind { const char* label, *cls, *listProp; int itemKey; bool ownWidget; uintptr_t c; int oList; };  // itemKey: FName of the row
static Kind kinds[] = {
    {"garage", "GaragePartListWidget", "PartsListView", 0x48, true},  // the part list is its own widget inside the column
    {"vendor", "VendorWidget", "ItemsListView", 0x28, false},
    {"decal", "GarageSelectDecalWidget", "DecalsTileView", 0x28, false},
};

struct Ref { int index; uintptr_t ptr; };
static bool alive(const Ref& r) { return r.ptr && api->object(r.index) == r.ptr; }

struct Screen {
    Ref owner; Kind* kind;
    uintptr_t list = 0, box = 0, field = 0, hold = 0, textData = 0;
    std::vector<uintptr_t> shown;
    std::wstring query;
    bool gaveUp = false;
};

static uintptr_t cListView, cVBox, cSearch, cText;
static uintptr_t gameplayCDO, widgetCDO, textCDO;
static uintptr_t fSpawn, fCreate, fAddChild, fSetVis, fSetItems, fIsVisible, fToStr;
static int oItems, oEntryClass, oSlot, oParent, oField, oText, oInText, oRet;

static std::mutex lock;
static std::vector<Ref> found;      // worker -> game thread
static std::vector<Screen> screens;  // game thread only
static std::map<std::string, std::wstring> names;  // row key -> lower-case display names

static std::wstring lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

static std::wstring toStr(uintptr_t ftext) {
    if (!get<uintptr_t>(ftext)) return L"";
    unsigned char parms[0x40] = {0};
    rd(ftext, parms + oInText, oRet - oInText);
    api->process_event(textCDO, fToStr, parms);
    uintptr_t s = 0; int len = 0;
    memcpy(&s, parms + oRet, 8); memcpy(&len, parms + oRet + 8, 4);
    if (!s || len <= 1 || len > 512) return L"";
    std::wstring out(len - 1, 0);
    return rd(s, out.data(), (size_t)(len - 1) * 2) ? out : L"";
}

// Display names of every loaded part and item row. Runs once, on the first search.
static void loadNames() {
    uintptr_t sPart = api->find_object("ScriptStruct", "VehiclePartRow", "/Script/MotorTown");
    uintptr_t sItem = api->find_object("ScriptStruct", "MTItemRow", "/Script/MotorTown");
    uintptr_t sTexts = api->find_object("ScriptStruct", "MTTextByTexts", "/Script/MotorTown");
    int oNames = off(sTexts, "Texts");
    struct { uintptr_t st; int plain, texts; } rowKinds[] = {
        {sPart, off(sPart, "Name"), off(sPart, "Name2")},
        {sItem, off(sItem, "ItemName"), off(sItem, "ItemNameTexts")},
    };
    for (int i = 0, n = api->num_objects(); i < n; i++) {
        uintptr_t t = api->object(i);
        if (!t || isCDO(t)) continue;
        std::string cn = objName(classOf(t));
        if (cn != "DataTable" && cn != "CompositeDataTable") continue;
        int rs = -1; uintptr_t st = 0;
        for (int x = 0x28; x <= 0x48 && rs < 0; x += 8) {
            uintptr_t q = get<uintptr_t>(t + x);
            if (q && objName(classOf(q)) == "ScriptStruct") { rs = x; st = q; }
        }
        for (auto& k : rowKinds) {
            if (rs < 0 || st != k.st) continue;
            uintptr_t el = get<uintptr_t>(t + rs + 8); int num = get<int>(t + rs + 16);
            for (int j = 0; j < num; j++) {  // TSetElement: FName, uint8*, hash links = 24 bytes
                uintptr_t row = get<uintptr_t>(el + 24ull * j + 8);
                std::string key = fname(el + 24ull * j);
                if (!row || key.empty()) continue;
                std::wstring text = toStr(row + k.plain);
                uintptr_t d = get<uintptr_t>(row + k.texts + oNames); int m = get<int>(row + k.texts + oNames + 8);
                for (int q = 0; q < m && q < 4; q++) text += L" " + toStr(d + q * (uintptr_t)(oRet - oInText));
                names[key] += L" " + lower(text);
            }
        }
    }
    MT_LOG("names: %d rows", (int)names.size());
}

static bool matches(const Screen& s, uintptr_t item) {
    char b[300];
    if (api->fname(item + s.kind->itemKey, b, sizeof b) == 0) return true;
    std::wstring key(b, b + strlen(b));
    auto it = names.find(b);
    std::wstring hay = lower(key) + (it != names.end() ? it->second : L"");
    std::wistringstream words(s.query);
    for (std::wstring w; words >> w;)
        if (hay.find(w) == std::wstring::npos) return false;
    return true;
}

static std::vector<uintptr_t> itemsOf(uintptr_t list) {
    uintptr_t d = get<uintptr_t>(list + oItems); int n = get<int>(list + oItems + 8);
    std::vector<uintptr_t> v(n > 0 ? n : 0);
    if (n > 0) rd(d, v.data(), (size_t)n * 8);
    return v;
}

static void setItems(uintptr_t list, std::vector<uintptr_t>& v) {
    struct { uintptr_t* data; int num, max; } arr = {v.data(), (int)v.size(), (int)v.size()};
    api->process_event(list, fSetItems, &arr);
}

static bool callBool(uintptr_t obj, uintptr_t fn) {
    unsigned char parms[16] = {0};
    api->process_event(obj, fn, parms);
    return parms[0] != 0;
}

static uintptr_t addTo(uintptr_t panel, uintptr_t child) {
    struct { uintptr_t content, slot; } p = {child, 0};
    api->process_event(panel, fAddChild, &p);
    return p.slot;
}

static void addBox(Screen& s) {
    if (!cSearch) cSearch = api->find_object("WidgetBlueprintGeneratedClass", "W_StringSearchBox_C", "/Game/UI/Component/W_StringSearchBox");
    uintptr_t from = s.kind->ownWidget ? s.owner.ptr : s.list;
    uintptr_t panel = get<uintptr_t>(get<uintptr_t>(from + oSlot) + oParent);
    if (!cSearch || !panel || !api->is_a(classOf(panel), cVBox)) {
        MT_LOG("%s: no search box -- class %p, panel %p is not a VerticalBox", s.kind->label, (void*)cSearch, (void*)panel);
        s.gaveUp = true;
        return;
    }
    struct { uintptr_t cls, outer, ret; } spawn = {cListView, s.owner.ptr, 0};
    api->process_event(gameplayCDO, fSpawn, &spawn);
    struct { uintptr_t context, cls, player, ret; } create = {s.owner.ptr, cSearch, 0, 0};
    api->process_event(widgetCDO, fCreate, &create);
    if (!spawn.ret || !create.ret) { MT_LOG("could not create the widgets"); s.gaveUp = true; return; }

    uintptr_t entry = get<uintptr_t>(s.list + oEntryClass);
    wr(spawn.ret + oEntryClass, &entry, 8);
    addTo(panel, spawn.ret);
    uint8_t collapsed = 1;
    api->process_event(spawn.ret, fSetVis, &collapsed);
    addTo(panel, create.ret);

    s.hold = spawn.ret; s.box = create.ret; s.field = get<uintptr_t>(s.box + oField);
    MT_LOG("%s: search box added, %d items", s.kind->label, (int)itemsOf(s.list).size());
}

static void update(Screen& s) {
    if (!s.box) { if (callBool(s.list, fIsVisible)) addBox(s); if (!s.box) return; }

    uintptr_t td = get<uintptr_t>(s.field + oText);
    bool changed = td != s.textData;
    if (changed) { s.textData = td; s.query = lower(toStr(s.field + oText)); }

    auto items = itemsOf(s.list);
    bool rebuilt = items != s.shown;
    if (rebuilt) setItems(s.hold, items);
    if (!rebuilt && !changed) return;

    if (!s.query.empty() && names.empty()) loadNames();
    auto all = itemsOf(s.hold);
    std::vector<uintptr_t> keep;
    for (uintptr_t it : all) if (matches(s, it)) keep.push_back(it);
    if (keep != items) setItems(s.list, keep);
    s.shown = keep;
}

static void tick(void*) {
    std::vector<Ref> add;
    { std::lock_guard<std::mutex> g(lock); add.swap(found); }
    for (auto& r : add) {
        bool known = false;
        for (auto& s : screens) known = known || s.owner.ptr == r.ptr;
        if (known) continue;
        for (auto& k : kinds) {
            if (!api->is_a(classOf(r.ptr), k.c)) continue;
            Screen s{r, &k};
            s.list = get<uintptr_t>(r.ptr + k.oList);
            if (s.list) screens.push_back(s);
        }
    }
    for (size_t i = 0; i < screens.size();) {
        if (!alive(screens[i].owner)) { screens.erase(screens.begin() + i); continue; }
        if (!screens[i].gaveUp) update(screens[i]);
        i++;
    }
}

static DWORD WINAPI run(LPVOID) {
    MT_LOG("Search Box loaded");
    if (!api->wait_engine(INFINITE)) { MT_LOG("FAILED: no engine"); return 0; }
    cListView = api->find_object("Class", "ListView", "/Script/UMG");
    cVBox = api->find_object("Class", "VerticalBox", "/Script/UMG");
    cText = api->find_object("Class", "EditableTextBox", "/Script/UMG");
    uintptr_t cWidget = api->find_object("Class", "Widget", "/Script/UMG");
    uintptr_t cSlot = api->find_object("Class", "PanelSlot", "/Script/UMG");
    uintptr_t cBoxNative = api->find_object("Class", "StringSearchBoxWidget", "/Script/MotorTown");
    gameplayCDO = api->find_object("GameplayStatics", "Default__GameplayStatics", "/Script/Engine");
    widgetCDO = api->find_object("WidgetBlueprintLibrary", "Default__WidgetBlueprintLibrary", "/Script/UMG");
    textCDO = api->find_object("KismetTextLibrary", "Default__KismetTextLibrary", "/Script/Engine");
    fSpawn = api->find_object("Function", "SpawnObject", "GameplayStatics");
    fCreate = api->find_object("Function", "Create", "WidgetBlueprintLibrary");
    fAddChild = api->find_object("Function", "AddChild", "PanelWidget");
    fSetVis = api->find_object("Function", "SetVisibility", "Widget");
    fSetItems = api->find_object("Function", "BP_SetListItems", "ListView");
    fIsVisible = api->find_object("Function", "IsVisible", "Widget");
    fToStr = api->find_object("Function", "Conv_TextToString", "KismetTextLibrary");
    oItems = off(cListView, "ListItems"); oEntryClass = off(cListView, "EntryWidgetClass");
    oSlot = off(cWidget, "Slot"); oParent = off(cSlot, "Parent");
    oField = off(cBoxNative, "SearchTextBox"); oText = off(cText, "Text");
    oInText = off(fToStr, "InText"); oRet = off(fToStr, "ReturnValue");

    uintptr_t need[] = {cVBox, gameplayCDO, widgetCDO, textCDO, fSpawn, fCreate, fAddChild, fSetVis, fSetItems, fIsVisible, fToStr};
    int offs[] = {oItems, oEntryClass, oSlot, oParent, oField, oText, oInText, oRet};
    bool ok = true;
    for (auto& k : kinds) {
        k.c = api->find_object("Class", k.cls, "/Script/MotorTown");
        k.oList = off(k.c, k.listProp);
        ok = ok && k.c && k.oList >= 0;
    }
    for (uintptr_t p : need) ok = ok && p;
    for (int o : offs) ok = ok && o >= 0;
    if (!ok) { MT_LOG("FAILED: widget classes not found -- no search box"); return 0; }

    for (int beat = 0;; beat++) {
        if (beat % 7 == 0) {  // about a second
            std::vector<Ref> v;
            for (int i = 0, n = api->num_objects(); i < n; i++) {
                uintptr_t o = api->object(i);
                if (!o || (get<uint32_t>(o + L().obj_flags) & 0x30)) continue;  // CDO or archetype
                uintptr_t c = classOf(o);
                for (auto& k : kinds)
                    if (api->is_a(c, k.c)) v.push_back({i, o});
            }
            std::lock_guard<std::mutex> g(lock);
            found.swap(v);
        }
        if (api->game_thread_ready()) api->run_on_game_thread(tick, nullptr);
        Sleep(150);
    }
}

extern "C" __declspec(dllexport) int PluginInit(const MtApi* a) {
    if (!attach(a)) return 1;
    CloseHandle(CreateThread(nullptr, 0, run, nullptr, 0, nullptr));
    return 0;
}
