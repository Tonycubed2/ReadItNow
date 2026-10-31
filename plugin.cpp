#include "SkyPrompt/API.hpp"
#include "includes/i18n.cpp"
#include "includes/MiniPage.cpp"
#include "includes/AutoTurn.cpp"

// Read It Now - page-turn fix
//
// Original problem: the book was dropped into the world, activated, and then
// immediately Disable()d while the Book Menu was open. The Book Menu was bound
// to that disabled world reference, so it displayed the first spread but could
// not turn pages.
//
// Fix:
//  - Default (iOpenMethod = 0): open the book straight from the inventory, the
//    same way clicking it in the Inventory menu does. No world copy, no
//    disable, no Papyrus give-back needed, and there is no "Take" button.
//  - Fallback (iOpenMethod = 1): the original drop-and-activate method, but the
//    dropped book is no longer disabled while it is being read.
//  - The prompt is removed as soon as it is accepted, and the book is opened
//    on the next game frame (outside SkyPrompt's input callback).
//  - Auto page turning while Voiced Books of Skyrim narrates (includes/AutoTurn.cpp,
//    [AutoTurn] section of ReadItNow.ini).
//  - The prompt text is kept in a persistent string (SkyPrompt stores it as a
//    string_view; the original pointed it at a destroyed temporary).
//
// Requires SkyPrompt/API.hpp from https://github.com/QTR-Modding/SkyPromptAPI
// (API 2.0), placed at <project>/SkyPrompt/API.hpp.

using Flag = RE::OBJ_BOOK::Flag;

SkyPromptAPI::Prompt readPrompt{"", 0, 0, SkyPromptAPI::PromptType::kHold};
std::array<SkyPromptAPI::Prompt, 1> prompts = {readPrompt};
SkyPromptAPI::ClientID clientID;
std::array<std::pair<RE::INPUT_DEVICE, uint32_t>, 2> keys{std::pair{INPUT_DEVICE::kKeyboard, 0},
                                                          std::pair{INPUT_DEVICE::kGamepad, 0}};
PlayerCharacter* player;
FormID playerID;
std::string promptText;
// SkyPrompt's Prompt::text is a std::string_view, so the text it points at must
// stay alive while the prompt is shown. The original code pointed it at a
// temporary string that was destroyed immediately.
std::string promptDisplayText;
TESQuest* readItNow;
BGSKeyword* exclusionKeyword;
std::string_view modName = "Read It Now.esp";
bool bSpellTomes = true;
bool bSkillBooks = true;
int iOpenMethod = 0;  // 0 = inventory read (fixed), 1 = original drop/activate method

TESObjectBOOK* lastBook = nullptr;
TESObjectREFR* tempRef = nullptr;
bool promptShown = false;
bool closeSinkRegistered = false;

void RegisterForBookClose();
void UnregisterForBookClose();

std::string str_replace(std::string_view haystack, std::string_view needle, std::string_view replacement) {
    if (needle.empty()) {
        return std::string(haystack);
    }

    std::string result;
    result.reserve(haystack.size());

    std::size_t pos = 0;
    while (true) {
        std::size_t found = haystack.find(needle, pos);
        if (found == std::string_view::npos) {
            result.append(haystack.substr(pos));
            break;
        }

        result.append(haystack.substr(pos, found - pos));
        result.append(replacement);
        pos = found + needle.size();
    }

    return result;
}

// this is handed via Papy so the RefAlias pointers don't need to be manually updated
// (only used by the fallback method)
void GiveBackTheBook(TESObjectREFR* a_ref) {
    auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
    if (!vm) return;
    auto policy = vm->GetObjectHandlePolicy();
    if (!policy) return;
    RE::VMHandle handle = policy->GetHandleForObject(readItNow->GetFormType(), readItNow);
    if (handle == policy->EmptyHandle()) return;

    RE::BSFixedString scriptName = "ReadItNow_Script";
    RE::BSFixedString functionName = "GiveBackToPlayer";

    RE::BSTSmartPointer<RE::BSScript::Object> papyrusObject;
    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> resultCallback;

    if (!vm->FindBoundObject(handle, scriptName.c_str(), papyrusObject)) {
        return;
    }

    auto args = RE::MakeFunctionArguments(std::move(a_ref));

    vm->DispatchMethodCall1(papyrusObject, functionName, args, resultCallback);
}

class MyPromptSink : public SkyPromptAPI::PromptSink {
public:
    std::span<const SkyPromptAPI::Prompt> GetPrompts() const override { return prompts; };

    void ProcessEvent(SkyPromptAPI::PromptEvent event) const override;
};

static MyPromptSink g_PromptSink;

void HidePrompt() {
    if (promptShown) {
        SkyPromptAPI::RemovePrompt(&g_PromptSink, clientID);
        promptShown = false;
    }
}

bool PlayerHasBook(TESObjectBOOK* a_book) {
    if (!player || !a_book) return false;
    auto counts = player->GetInventoryCounts([a_book](TESBoundObject& a_obj) { return &a_obj == a_book; });
    auto it = counts.find(a_book);
    return it != counts.end() && it->second > 0;
}

void OpenFromInventory(TESObjectBOOK* a_book) {
    // Same path the Inventory menu uses when you click a book: opens the
    // Book Menu bound to the inventory item (no world reference), marks it
    // read and applies skill book / spell tome effects.
    auto equipManager = ActorEquipManager::GetSingleton();
    if (!equipManager) return;
    equipManager->EquipObject(player, a_book, nullptr, 1, nullptr, false, false, true, false);
}

void OpenByDropping(TESObjectBOOK* a_book) {
    auto refhandle = player->DropObject(a_book, nullptr, 1);
    if (!refhandle) return;
    tempRef = refhandle.get().get();
    if (!tempRef) return;
    lastBook = a_book;
    RegisterForBookClose();
    tempRef->ActivateRef(player, 0, a_book, 1, false);
    // NOTE: the original code called tempRef->Disable() here. That is what
    // stopped the pages from turning, so it is intentionally gone. The book
    // is returned to the player by Papyrus when the Book Menu closes.
}

void OpenBook(TESObjectBOOK* a_book) {
    if (!a_book || !player) return;
    if (!PlayerHasBook(a_book)) return;  // sold/dropped/stored before the prompt was accepted

    auto ui = UI::GetSingleton();
    if (ui && ui->IsMenuOpen(BookMenu::MENU_NAME)) return;

    if (a_book->TeachesSpell()) {
        a_book->Read(player);
        return;
    }

    if (iOpenMethod == 1) {
        OpenByDropping(a_book);
    } else {
        OpenFromInventory(a_book);
    }
}

void MyPromptSink::ProcessEvent(SkyPromptAPI::PromptEvent event) const {
    if (event.type == SkyPromptAPI::PromptEventType::kRemovedByMod ||
        event.type == SkyPromptAPI::PromptEventType::kTimeout ||
        event.type == SkyPromptAPI::PromptEventType::kDeclined) {
        promptShown = false;
        return;
    }
    if (event.type != SkyPromptAPI::PromptEventType::kAccepted) return;

    TESObjectBOOK* book = lastBook;
    HidePrompt();
    if (!book) return;

    // Open on the next frame, outside SkyPrompt's input handling, so the held
    // prompt key is not still being processed when the Book Menu takes input.
    SKSE::GetTaskInterface()->AddTask([book]() { OpenBook(book); });
}

class EventHandler : public BSTEventSink<TESContainerChangedEvent>, public BSTEventSink<MenuOpenCloseEvent> {
    BSEventNotifyControl ProcessEvent(const TESContainerChangedEvent* event,
                                      BSTEventSource<TESContainerChangedEvent>*) override {
        if (!event || event->newContainer != playerID) return BSEventNotifyControl::kContinue;
        if (UI::GetSingleton()->IsMenuOpen(MainMenu::MENU_NAME) ||
            UI::GetSingleton()->IsMenuOpen(LoadingMenu::MENU_NAME))
            return BSEventNotifyControl::kContinue;
        // the fallback method hands the dropped book back; don't prompt again for it
        if (closeSinkRegistered) return BSEventNotifyControl::kContinue;
        TESObjectBOOK* book = TESForm::LookupByID<TESObjectBOOK>(event->baseObj);
        if (!book || book->IsRead() || book->HasKeyword(exclusionKeyword)) return BSEventNotifyControl::kContinue;
        if ((!bSkillBooks && book->TeachesSkill()) || (!bSpellTomes && book->TeachesSpell()))
            return BSEventNotifyControl::kContinue;

        lastBook = book;

        HidePrompt();
        promptDisplayText = str_replace(promptText, "<name>", lastBook->GetName());
        prompts[0].text = promptDisplayText;
        keys[0].second =
            ControlMap::GetSingleton()->GetMappedKey(UserEvents::GetSingleton()->readyWeapon, INPUT_DEVICE::kKeyboard);
        keys[1].second =
            ControlMap::GetSingleton()->GetMappedKey(UserEvents::GetSingleton()->readyWeapon, INPUT_DEVICE::kGamepad);
        prompts[0].button_key = std::span{keys};
        promptShown = SkyPromptAPI::SendPrompt(&g_PromptSink, clientID);
        return BSEventNotifyControl::kContinue;
    }

    BSEventNotifyControl ProcessEvent(const MenuOpenCloseEvent* event, BSTEventSource<MenuOpenCloseEvent>*) override {
        if (!event || event->opening || event->menuName != BookMenu::MENU_NAME) return BSEventNotifyControl::kContinue;
        if (tempRef &&
            !tempRef->IsDeleted()  // using the Take prompt in Book menu marks the ref as "deleted"
        ) {
            GiveBackTheBook(tempRef);
        }
        tempRef = nullptr;
        lastBook = nullptr;
        UnregisterForBookClose();
        return BSEventNotifyControl::kContinue;
    }
};

static EventHandler g_sink;

void RegisterForBookClose() {
    if (closeSinkRegistered) return;
    UI::GetSingleton()->AddEventSink<MenuOpenCloseEvent>(&g_sink);
    closeSinkRegistered = true;
}

void UnregisterForBookClose() {
    if (!closeSinkRegistered) return;
    UI::GetSingleton()->RemoveEventSink<MenuOpenCloseEvent>(&g_sink);
    closeSinkRegistered = false;
}

void LoadConfig() {
    CSimpleIniA ini;
    std::string filePath = "Data/SKSE/Plugins/ReadItNow.ini";
    if (ini.LoadFile(filePath.c_str()) == SI_OK) {
        bSpellTomes = ini.GetBoolValue("Main", "bShowForSpellTomes", true);
        bSkillBooks = ini.GetBoolValue("Main", "bShowForSkillBooks", true);
        iOpenMethod = static_cast<int>(ini.GetLongValue("Main", "iOpenMethod", 0));
        AutoTurn::LoadConfig(ini);
    }
}

void Setup() {
    readItNow = TESDataHandler::GetSingleton()->LookupForm<TESQuest>(0x800, modName);
    if (!readItNow) return;
    exclusionKeyword = TESDataHandler::GetSingleton()->LookupForm<BGSKeyword>(0x801, modName);
    if (!exclusionKeyword) return;
    player = PlayerCharacter::GetSingleton();
    playerID = player->GetFormID();
    clientID = SkyPromptAPI::RequestClientID();
    i18n::pluginName = "ReadItNow";
    promptText = i18n::GetTranslation("Main", "sReadPrompt");
    LoadConfig();
    ScriptEventSourceHolder::GetSingleton()->AddEventSink<TESContainerChangedEvent>(&g_sink);
    AutoTurn::Install();
}

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    AutoTurn::InitLog();
    MiniPage::Install();  // must hook the D3D set-up before the game creates its renderer

    SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* message) {
        if (message->type == SKSE::MessagingInterface::kDataLoaded) Setup();
        if (message->type == SKSE::MessagingInterface::kPostLoadGame ||
            message->type == SKSE::MessagingInterface::kNewGame)
            AutoTurn::OnGameLoaded();
    });

    return true;
}
