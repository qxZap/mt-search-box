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
// Engines: when the part list holds engines, two rows of the game's W_CheckBox appear
// under the box, one for fuels and one for Cars / Trucks / Bikes, all ticked; unticking one
// hides those engines. Fuel is on the engine asset, which the row only points at softly, so
// each is loaded on first use. The kind is in the row's description, a line like
// "Tags: Truck": tuning mods empty VehicleTypes so any engine fits anything, and the
// description is theirs to fill. An engine with no tag shows whatever is unticked.
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
static Ref refOf(uintptr_t o) { return {o ? get<int>(o + L().obj_index) : 0, o}; }

// Never crash the game. Every widget the plugin calls into is checked alive first (not only the
// screen's owner: the game rebuilds lists under an open screen), table rows are re-checked against
// the table they came from before they are read (tables are rebuilt on load), and the frame tick runs
// under a guard: a fault is logged, everything is re-checked after a 500 ms pause, and after MAX_FAULTS
// in a session the plugin stops instead of risking the game again.
static const int MAX_FAULTS = 5;
static volatile LONG faults, disabled;
static DWORD pauseUntil;

struct Screen {
    Ref owner; Kind* kind;
    std::vector<Ref> deps;  // every widget this screen calls into, checked alive before each update
    uintptr_t list = 0, box = 0, field = 0, hold = 0, textData = 0;
    std::vector<uintptr_t> shown;
    std::wstring query;
    bool gaveUp = false;
    uintptr_t panel = 0, bars[2] = {}, boxes[6] = {};
    bool on[6] = {true, true, true, true, true, true}, barShown = false, ready = false;
};

// row: 0 fuel (EMTFuelType), 1 vehicle kind. The spaces hold the text off both checkboxes.
static const struct { const wchar_t* label; int row, value; } FILTERS[6] = {
    {L"  Gasoline   ", 0, 1}, {L"  Diesel   ", 0, 2}, {L"  Electric   ", 0, 3},
    {L"  Cars   ", 1, 1}, {L"  Trucks   ", 1, 2}, {L"  Bikes   ", 1, 3}};

static uintptr_t cListView, cVBox, cSearch, cText;
static uintptr_t gameplayCDO, widgetCDO, textCDO;
static uintptr_t fSpawn, fCreate, fAddChild, fSetVis, fSetItems, fIsVisible, fToStr;
static int oItems, oEntryClass, oSlot, oParent, oField, oText, oInText, oRet;

static std::mutex lock;
static std::vector<Ref> found;      // worker -> game thread
static std::vector<Screen> screens;  // game thread only
static std::map<std::string, std::wstring> names;  // row key -> lower-case display names
struct EngineRow { uintptr_t row; int snap; };  // snap: the table it came from (TableSnap), checked before use
static std::map<std::string, EngineRow> engineRows;  // row key -> row, for rows with an engine asset
struct Engine { int fuel, kind; };
static std::map<std::string, Engine> engines;         // row key -> what the asset says, once loaded
static uintptr_t cHBox, cCheck, cEngine, systemCDO, fLoad, fIsChecked, fSetChecked, fSetText, fToText;
static int oEngineAsset, oFuel, oDesc, oLoadIn, oLoadRet, oStrIn, oStrRet;

static std::wstring lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

// An FText (UE 5.5) is a ref-counted pointer to its text data, then flags. A live one points at an object
// whose vtable lies in the game's code. A freed or garbage FText (the 0xffff... read that crashed
// Conv_TextToString) fails this and is skipped instead of handed to the game. Only the first 8 bytes are a
// pointer: an older two-pointer reading rejected every text, typed searches included.
static bool textAlive(uintptr_t ftext) {
    static uintptr_t lo, hi;
    if (!lo) {
        lo = (uintptr_t)GetModuleHandleW(nullptr);
        auto nt = (const IMAGE_NT_HEADERS*)(lo + ((const IMAGE_DOS_HEADER*)lo)->e_lfanew);
        hi = lo + nt->OptionalHeader.SizeOfImage;
    }
    uintptr_t obj = get<uintptr_t>(ftext);
    if (obj < 0x10000) return false;
    uintptr_t vt = get<uintptr_t>(obj);
    return vt >= lo && vt < hi;
}

static std::wstring toStr(uintptr_t ftext) {
    if (!textAlive(ftext)) return L"";
    unsigned char parms[0x40] = {0};
    rd(ftext, parms + oInText, oRet - oInText);
    api->process_event(textCDO, fToStr, parms);
    uintptr_t s = 0; int len = 0;
    memcpy(&s, parms + oRet, 8); memcpy(&len, parms + oRet + 8, 4);
    if (!s || len <= 1 || len > 512) return L"";
    std::wstring out(len - 1, 0);
    return rd(s, out.data(), (size_t)(len - 1) * 2) ? out : L"";
}

// Display names of every loaded part and item row, in two halves so the game never stalls on them:
// collectNames (plugin thread, memory reads only) lists the rows once the first garage list is up;
// nameSlice (game thread, about 3 ms a frame) turns their FTexts into strings, which needs the game.
// A row pointer is only good while its table still holds the same row storage: each job names the
// table it came from (snap), and the game thread checks that before reading the row.
struct TableSnap { Ref table; int rowsAt; uintptr_t el; int num; };
struct NameJob { std::string key; uintptr_t row; int plain, texts, snap, slot; };  // slot: index in the table's row set
static std::vector<NameJob> nameJobs, pendingJobs;  // pending*: worker -> game thread under lock
static std::vector<TableSnap> snaps, pendingSnaps;
static size_t nameAt;
static bool taken;  // game thread has swapped the pending lists in
static volatile LONG wantNames, namesListed;
static volatile bool namesDone;
static std::map<std::string, EngineRow> listedEngines;  // handed to engineRows on the game thread

static bool snapSame(int i) {
    if (i < 0 || i >= (int)snaps.size()) return false;
    const TableSnap& s = snaps[i];
    return alive(s.table) && get<uintptr_t>(s.table.ptr + s.rowsAt + 8) == s.el && get<int>(s.table.ptr + s.rowsAt + 16) == s.num;
}

// Game thread: a table changed under the names, so list them again from scratch.
static void relistNames(const char* why) {
    MT_LOG("names: %s -- listing again", why);
    nameJobs.clear(); snaps.clear(); engineRows.clear(); names.clear();
    nameAt = 0; taken = false; namesDone = false;
    InterlockedExchange(&namesListed, 0);
    InterlockedExchange(&wantNames, 1);
}

static void collectNames() {
    uintptr_t sPart = api->find_object("ScriptStruct", "VehiclePartRow", "/Script/MotorTown");
    uintptr_t sItem = api->find_object("ScriptStruct", "MTItemRow", "/Script/MotorTown");
    uintptr_t cTable = api->find_object("Class", "DataTable", "/Script/Engine");
    struct { uintptr_t st; int plain, texts; } rowKinds[] = {
        {sPart, off(sPart, "Name"), off(sPart, "Name2")},
        {sItem, off(sItem, "ItemName"), off(sItem, "ItemNameTexts")},
    };
    std::vector<NameJob> jobs;
    std::vector<TableSnap> tables;
    std::map<std::string, EngineRow> engines;
    for (int i = 0, n = api->num_objects(); i < n; i++) {
        uintptr_t t = api->object(i);
        if (!t || isCDO(t) || !api->is_a(classOf(t), cTable)) continue;  // CompositeDataTable is one too
        int rs = -1; uintptr_t st = 0;
        for (int x = 0x28; x <= 0x48 && rs < 0; x += 8) {
            uintptr_t q = get<uintptr_t>(t + x);
            if (q && (q == sPart || q == sItem)) { rs = x; st = q; }
        }
        for (auto& k : rowKinds) {
            if (rs < 0 || st != k.st) continue;
            uintptr_t el = get<uintptr_t>(t + rs + 8); int num = get<int>(t + rs + 16);
            if (!el || num <= 0 || num > 200000) continue;
            int snap = (int)tables.size();
            tables.push_back({{i, t}, rs, el, num});
            for (int j = 0; j < num; j++) {  // TSetElement: FName, uint8*, hash links = 24 bytes
                uintptr_t row = get<uintptr_t>(el + 24ull * j + 8);
                std::string key = fname(el + 24ull * j);
                if (!row || key.empty()) continue;
                jobs.push_back({key, row, k.plain, k.texts, snap, j});
                if (k.st == sPart) {
                    uintptr_t soft[5] = {};  // TSoftObjectPtr: weak pointer, then the path
                    rd(row + oEngineAsset, soft, sizeof soft);
                    if (soft[1]) engines[key] = {row, snap};
                }
            }
        }
    }
    std::lock_guard<std::mutex> g(lock);
    pendingJobs.swap(jobs);
    pendingSnaps.swap(tables);
    listedEngines.swap(engines);
    InterlockedExchange(&namesListed, 1);
    MT_LOG("names: %d rows listed from %d tables", (int)pendingJobs.size(), (int)pendingSnaps.size());
}

// Game thread. True once every name is in.
static bool nameSlice() {
    if (!namesListed) { InterlockedExchange(&wantNames, 1); return false; }
    if (namesDone) return true;
    if (!taken) {
        std::lock_guard<std::mutex> g(lock);
        nameJobs.swap(pendingJobs); snaps.swap(pendingSnaps); engineRows.swap(listedEngines);
        pendingJobs.clear(); pendingSnaps.clear(); listedEngines.clear();
        taken = true;
    }
    static int oNames = off(api->find_object("ScriptStruct", "MTTextByTexts", "/Script/MotorTown"), "Texts");
    LARGE_INTEGER hz, from, now;
    QueryPerformanceFrequency(&hz); QueryPerformanceCounter(&from);
    std::vector<signed char> ok(snaps.size(), -1);  // each table checked once a slice
    for (; nameAt < nameJobs.size(); nameAt++) {
        QueryPerformanceCounter(&now);
        if ((now.QuadPart - from.QuadPart) * 1000 > 3 * hz.QuadPart) break;  // 3 ms a frame
        const NameJob& k = nameJobs[nameAt];
        if (k.snap < 0 || k.snap >= (int)ok.size()) continue;
        if (ok[k.snap] < 0) ok[k.snap] = snapSame(k.snap);
        if (!ok[k.snap] || get<uintptr_t>(snaps[k.snap].el + 24ull * k.slot + 8) != k.row) {
            relistNames("a table changed while names were read"); return false;
        }
        std::wstring text = toStr(k.row + k.plain);
        uintptr_t d = get<uintptr_t>(k.row + k.texts + oNames); int m = get<int>(k.row + k.texts + oNames + 8);
        for (int q = 0; q < m && q < 4; q++) text += L" " + toStr(d + q * (uintptr_t)(oRet - oInText));
        names[k.key] += L" " + lower(text);
    }
    if (nameAt < nameJobs.size()) return false;
    namesDone = true;
    MT_LOG("names: %d rows", (int)names.size());
    return true;
}

static Engine engineOf(const std::string& key) {
    auto row = engineRows.find(key);
    if (row == engineRows.end()) return {0, 0};
    auto known = engines.find(key);
    if (known != engines.end()) return known->second;
    if (!snapSame(row->second.snap)) { relistNames("an engine's table changed"); return {0, 0}; }  // never read a freed row
    uintptr_t r = row->second.row;
    unsigned char parms[0x60] = {0};
    rd(r + oEngineAsset, parms + oLoadIn, oLoadRet - oLoadIn);
    api->process_event(systemCDO, fLoad, parms);
    uintptr_t asset = 0;
    memcpy(&asset, parms + oLoadRet, 8);
    std::wstring desc = lower(toStr(r + oDesc));
    size_t tags = desc.find(L"tags:");
    int kind = 0;
    if (tags != std::wstring::npos) {
        auto has = [&](const wchar_t* w) { return desc.find(w, tags) != std::wstring::npos; };
        kind = has(L"bike") ? 3 : has(L"truck") ? 2 : has(L"car") ? 1 : 0;
    }
    return engines[key] = {asset ? get<uint8_t>(asset + oFuel) : 0, kind};
}

static std::string itemKey(const Screen& s, uintptr_t item) {
    char b[300];
    return api->fname(item + s.kind->itemKey, b, sizeof b) ? b : "";
}

static bool matches(const Screen& s, uintptr_t item) {
    char b[300];
    if (api->fname(item + s.kind->itemKey, b, sizeof b) == 0) return true;
    if (s.barShown)
        for (int i = 0; i < 6; i++) {
            Engine e = engineOf(b);
            if (!s.on[i] && (FILTERS[i].row ? e.kind : e.fuel) == FILTERS[i].value) return false;
        }
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

static void setVisible(uintptr_t widget, bool visible) {
    uint8_t v = visible ? 0 : 1;  // ESlateVisibility: Visible, Collapsed
    api->process_event(widget, fSetVis, &v);
}

static void addFuelBar(Screen& s) {
    if (!cCheck) cCheck = api->find_object("WidgetBlueprintGeneratedClass", "W_CheckBox_C", "/Game/UI/Button/W_CheckBox");
    int oInner = cCheck ? off(cCheck, "CheckBox") : -1, oLabel = cCheck ? off(cCheck, "NameTextBlock") : -1;
    if (oInner < 0 || oLabel < 0) { MT_LOG("no fuel filter -- W_CheckBox %p, CheckBox %d, label %d", (void*)cCheck, oInner, oLabel); return; }
    for (int row = 0; row < 2; row++) {
        struct { uintptr_t cls, outer, ret; } spawn = {cHBox, s.owner.ptr, 0};
        api->process_event(gameplayCDO, fSpawn, &spawn);
        if (!spawn.ret) return;
        addTo(s.panel, spawn.ret);
        setVisible(spawn.ret, false);
        s.bars[row] = spawn.ret;
    }
    for (int i = 0; i < 6; i++) {
        struct { uintptr_t context, cls, player, ret; } create = {s.owner.ptr, cCheck, 0, 0};
        api->process_event(widgetCDO, fCreate, &create);
        if (!create.ret) return;
        addTo(s.bars[FILTERS[i].row], create.ret);
        s.boxes[i] = get<uintptr_t>(create.ret + oInner);
        uint8_t state = 1;  // ECheckBoxState::Checked
        api->process_event(s.boxes[i], fSetChecked, &state);
        unsigned char parms[0x40] = {0};
        struct { const wchar_t* data; int num, max; } str = {FILTERS[i].label, (int)wcslen(FILTERS[i].label) + 1, (int)wcslen(FILTERS[i].label) + 1};
        memcpy(parms + oStrIn, &str, sizeof str);
        api->process_event(textCDO, fToText, parms);
        api->process_event(get<uintptr_t>(create.ret + oLabel), fSetText, parms + oStrRet);
    }
    for (uintptr_t w : s.bars) s.deps.push_back(refOf(w));
    for (uintptr_t w : s.boxes) s.deps.push_back(refOf(w));
    s.ready = true;
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

    s.hold = spawn.ret; s.box = create.ret; s.field = get<uintptr_t>(s.box + oField); s.panel = panel;
    for (uintptr_t w : {panel, s.hold, s.box, s.field}) s.deps.push_back(refOf(w));
    if (s.kind->ownWidget) addFuelBar(s);
    MT_LOG("%s: search box added, %d items", s.kind->label, (int)itemsOf(s.list).size());
}

// The game swapped the list, or something this screen calls into is gone: hide what is still there
// (so a fresh box does not sit under a dead one) and start the screen over on the current list.
// Alive by the index recorded when the widget was made: a freed address reused by another object fails.
static bool stillAlive(const Screen& s, uintptr_t p) {
    for (auto& d : s.deps) if (p && d.ptr == p) return alive(d);
    return false;
}

static void restart(Screen& s, uintptr_t list) {
    if (stillAlive(s, s.box)) setVisible(s.box, false);
    for (uintptr_t bar : s.bars) if (stillAlive(s, bar)) setVisible(bar, false);
    MT_LOG("%s: list rebuilt under the screen -- search box set up again", s.kind->label);
    Screen fresh{s.owner, s.kind};
    fresh.list = list;
    if (list) fresh.deps.push_back(refOf(list));
    s = fresh;
}

static void update(Screen& s) {
    uintptr_t list = get<uintptr_t>(s.owner.ptr + s.kind->oList);
    bool dead = list != s.list;
    for (auto& d : s.deps) dead = dead || !alive(d);
    if (dead) { restart(s, list); if (!s.list) return; }
    if (!s.box) { if (callBool(s.list, fIsVisible)) addBox(s); if (!s.box) return; }

    uintptr_t td = get<uintptr_t>(s.field + oText);
    bool changed = td != s.textData;
    if (changed) { s.textData = td; s.query = lower(toStr(s.field + oText)); }

    if (s.barShown)
        for (int i = 0; i < 6; i++) {
            bool on = callBool(s.boxes[i], fIsChecked);
            changed = changed || on != s.on[i];
            s.on[i] = on;
        }

    auto items = itemsOf(s.list);
    bool rebuilt = items != s.shown;
    if (rebuilt) setItems(s.hold, items);
    if (!rebuilt && !changed) return;

    if (s.ready || !s.query.empty()) nameSlice();
    auto all = itemsOf(s.hold);
    if (rebuilt && s.ready) {
        bool has = false;
        for (uintptr_t it : all) has = has || engineRows.count(itemKey(s, it));
        if (has != s.barShown) for (uintptr_t bar : s.bars) setVisible(bar, has);
        s.barShown = has;
    }
    std::vector<uintptr_t> keep;
    for (uintptr_t it : all) if (matches(s, it)) keep.push_back(it);
    if (keep != items) setItems(s.list, keep);
    s.shown = keep;
}

static void tick(void*) {
    // Names come in a slice a frame; when the last one is in, every open list filters again.
    if (wantNames && !namesDone && nameSlice()) for (auto& s : screens) s.shown.clear();
    std::vector<Ref> add;
    { std::lock_guard<std::mutex> g(lock); add.swap(found); }
    for (auto& r : add) {
        if (!alive(r)) continue;  // listed by the worker up to two seconds ago
        bool known = false;
        for (auto& s : screens) known = known || s.owner.ptr == r.ptr;
        if (known) continue;
        for (auto& k : kinds) {
            if (!api->is_a(classOf(r.ptr), k.c)) continue;
            Screen s{r, &k};
            s.list = get<uintptr_t>(r.ptr + k.oList);
            if (s.list) { s.deps.push_back(refOf(s.list)); screens.push_back(s); }
        }
    }
    for (size_t i = 0; i < screens.size();) {
        if (!alive(screens[i].owner)) { screens.erase(screens.begin() + i); continue; }
        if (!screens[i].gaveUp) update(screens[i]);
        i++;
    }
}

// After a fault: every screen re-checks its widgets from scratch and the names are listed again.
static void afterFault() {
    for (auto& s : screens) s.deps.push_back({0, 1});  // never alive (object 0 is not at 1): update() restarts the screen
    relistNames("recovering after a fault");
}

static int faultFilter(unsigned code) {
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR || code == EXCEPTION_DATATYPE_MISALIGNMENT
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

// No C++ objects in here, so it can hold the SEH guard (tick itself has them).
static void guardedTick(void* p) {
    if (disabled || (int)(GetTickCount() - pauseUntil) < 0) return;
    __try {
        tick(p);
    } __except (faultFilter(GetExceptionCode())) {
        LONG n = InterlockedIncrement(&faults);
        pauseUntil = GetTickCount() + 500;
        MT_LOG("FAULT %ld of %d caught in the search box tick -- retrying in 500 ms", n, MAX_FAULTS);
        if (n >= MAX_FAULTS) { InterlockedExchange(&disabled, 1); MT_LOG("too many faults: Search Box stops for this session"); return; }
        __try { afterFault(); } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&disabled, 1); }
    }
}

static DWORD WINAPI run(LPVOID) {
    MT_LOG("Search Box loaded");
    if (!api->wait_engine(INFINITE)) { MT_LOG("FAILED: no engine"); return 0; }
    // The engine can be ready before UMG is in, and one empty lookup would end the plugin.
    for (int i = 0; i < 240 && !(api->find_object("Class", "ListView", "/Script/UMG") && api->find_object("Class", "EditableTextBox", "/Script/UMG")
                                 && api->find_object("WidgetBlueprintLibrary", "Default__WidgetBlueprintLibrary", "/Script/UMG")); i++) Sleep(500);
    cListView = api->find_object("Class", "ListView", "/Script/UMG");
    cVBox = api->find_object("Class", "VerticalBox", "/Script/UMG");
    cHBox = api->find_object("Class", "HorizontalBox", "/Script/UMG");
    cEngine = api->find_object("Class", "MHEngineDataAsset", "/Script/MotorTown");
    uintptr_t sEngine = api->find_object("ScriptStruct", "MTEngineProperty", "/Script/MotorTown");
    uintptr_t sRow = api->find_object("ScriptStruct", "VehiclePartRow", "/Script/MotorTown");
    systemCDO = api->find_object("KismetSystemLibrary", "Default__KismetSystemLibrary", "/Script/Engine");
    fLoad = api->find_object("Function", "LoadAsset_Blocking", "KismetSystemLibrary");
    fIsChecked = api->find_object("Function", "IsChecked", "CheckBox");
    fSetChecked = api->find_object("Function", "SetCheckedState", "CheckBox");
    fSetText = api->find_object("Function", "SetText", "TextBlock");
    fToText = api->find_object("Function", "Conv_StringToText", "KismetTextLibrary");
    oEngineAsset = off(sRow, "EngineAsset");
    oFuel = off(cEngine, "EngineProperty") + off(sEngine, "FuelType");
    oDesc = off(sRow, "Desciption");  // spelled so in the game
    oLoadIn = off(fLoad, "Asset"); oLoadRet = off(fLoad, "ReturnValue");
    oStrIn = off(fToText, "InString"); oStrRet = off(fToText, "ReturnValue");
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

    uintptr_t need[] = {cVBox, cHBox, cEngine, systemCDO, fLoad, fIsChecked, fSetChecked, fSetText, fToText, gameplayCDO, widgetCDO, textCDO, fSpawn, fCreate, fAddChild, fSetVis, fSetItems, fIsVisible, fToStr};
    int offs[] = {oEngineAsset, oFuel, oDesc, oLoadIn, oLoadRet, oStrIn, oStrRet, oItems, oEntryClass, oSlot, oParent, oField, oText, oInText, oRet};
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
        if (beat % 14 == 0) {  // about two seconds
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
        if (disabled) return 0;
        if (wantNames && !namesListed) collectNames();
        if (api->game_thread_ready()) api->run_on_game_thread(guardedTick, nullptr);
        Sleep(namesListed && !namesDone ? 16 : 150);  // a frame at a time while names are coming in
    }
}

extern "C" __declspec(dllexport) int PluginInit(const MtApi* a) {
    if (!attach(a)) return 1;
    if (iniDisabled(L"search_box.ini")) return 0;
    CloseHandle(CreateThread(nullptr, 0, run, nullptr, 0, nullptr));
    return 0;
}
