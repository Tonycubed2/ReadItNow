// AutoTurn.cpp - turns Book Menu pages automatically while
// Voiced Books of Skyrim (VBoS) narrates the book.
//
// How it works
//  - VBoS plays ONE audio file per book. Its player alias script
//    (VoicedBooksPlayerAliasScript) keeps the playing state in Papyrus:
//    bIsPlaying, iSoundPlayInstance (auto properties), bIsPaused and
//    bIsKillcamPaused (variables). We read those every tick and count the
//    narration time while it plays and isn't paused.
//  - Which audio file belongs to the book comes from the same Data\*_VBOS.ini
//    files VBoS reads. Timing data for that file (every spoken word with its
//    time, from a speech-recognition pass over the VBoS audio) is loaded from
//    Data\SKSE\Plugins\ReadItNow\VBoSTimings\<audio name>.txt.
//  - The page movie (book.swf) keeps PageInfoA (pageTop per page) and
//    iLeftPageNumber (open spread). The open spread's text is cut out exactly
//    like book.swf does it, and its last few words are found in the timing
//    data: that is the moment the narrator finishes the spread.
//  - Without timing data (or without a good match) the moment is estimated from
//    the spread's share of the book's words.
//  - fTurnDelay seconds after that moment, Right Arrow is pressed for the player.
//  - If you page back to a spread that was already read, it is left alone.
//
// Everything is logged to Documents\My Games\Skyrim Special Edition\SKSE\ReadItNow.log

#include <ShlObj.h>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>
#include <spdlog/sinks/basic_file_sink.h>

namespace AutoTurn {
    // ---- settings (ReadItNow.ini, [AutoTurn]) ----
    bool bEnabled = true;
    float fTurnDelay = 0.75f;  // seconds after the spread's text should have been read
    int iTurnMethod = 1;       // 1 = Windows key press (default; proven by the test: Right Arrow turns the page), 2 = input dispatcher event, 0 = engine input queue
    std::uint32_t iTurnKey = 0xCD;  // DirectInput code of the key pressed for method 1: 0xCD = Right Arrow (D, the mapped key, does NOT turn pages)
    bool bDebugLog = false;
    bool bResumeOnReopen = true;
    bool bReadWhileWalking = true;   // forward key while a narrated book is open: walk with a mini page
    float fReopenAfterStill = 8.0f;  // standing still this long (out of combat) reopens the full book  // closing a book mid-narration pauses it; reopening the same book continues there
    bool bProbe = false;  // test mode: try every page-turn route once and log which one turns the page

    // ---- runtime state ----
    std::atomic<bool> running{false};
    std::thread worker;
    std::string hbReason;  // what the last tick did (heartbeat log line)

    RE::BSTSmartPointer<RE::BSScript::Object> vbosObject;

    std::int32_t trackedInstance = -1;
    double elapsedSec = 0.0;
    double durationSec = 0.0;
    std::chrono::steady_clock::time_point lastTick;

    int shownLeftPage = -1;
    double elapsedAtShow = 0.0;  // narration time when the open spread was first seen
    bool turnedThisSpread = false;
    int releasePending = 0;  // ticks until we send the key release
    double turnSentAt = 0.0;  // elapsedSec when the key was sent
    int turnRetries = 0;
    std::uint32_t turnKey = 0xCD;
    bool loggedPathFail = false;

    constexpr std::string_view kVBoSPlugin = "Voiced Books of Skyrim.esp";
    constexpr const char* kVBoSScript = "VoicedBooksPlayerAliasScript";

    void Log(const std::string& a_msg) {
        if (bDebugLog) spdlog::info(a_msg);
    }

    // Documents\My Games\Skyrim Special Edition\SKSE\ReadItNow.log
    // (the CommonLib version this project pins puts logs in "My Games\Skyrim.INI\SKSE")
    void InitLog() {
        PWSTR docs = nullptr;
        if (SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &docs) != S_OK || !docs) {
            if (docs) CoTaskMemFree(docs);
            return;
        }
        std::filesystem::path path = docs;
        CoTaskMemFree(docs);
        path /= "My Games";
        path /= "Skyrim Special Edition";
        path /= "SKSE";
        path /= "ReadItNow.log";
        try {
            // appends, so one launch's log survives the next launch
            auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), false);
            auto log = std::make_shared<spdlog::logger>("ReadItNow", std::move(sink));
            log->set_level(spdlog::level::info);
            log->flush_on(spdlog::level::info);
            log->set_pattern("[%H:%M:%S.%e] %v");
            spdlog::set_default_logger(std::move(log));
            spdlog::info("==================== game started ====================");
        } catch (...) {
        }
    }

    void LoadConfig(CSimpleIniA& a_ini) {
        bEnabled = a_ini.GetBoolValue("AutoTurn", "bAutoTurnPages", true);
        fTurnDelay = static_cast<float>(a_ini.GetDoubleValue("AutoTurn", "fTurnDelay", 0.75));
        iTurnMethod = static_cast<int>(a_ini.GetLongValue("AutoTurn", "iTurnMethod", 1));
        iTurnKey = static_cast<std::uint32_t>(a_ini.GetLongValue("AutoTurn", "iTurnKey", 0xCD));
        if (iTurnKey == 0 || iTurnKey >= 0xFF) iTurnKey = 0xCD;
        bDebugLog = a_ini.GetBoolValue("AutoTurn", "bDebugLog", false);
        bProbe = a_ini.GetBoolValue("AutoTurn", "bProbeTurnMethods", false);
        bResumeOnReopen = a_ini.GetBoolValue("AutoTurn", "bResumeOnReopen", true);
        bReadWhileWalking = a_ini.GetBoolValue("AutoTurn", "bReadWhileWalking", true);
        fReopenAfterStill = static_cast<float>(a_ini.GetDoubleValue("AutoTurn", "fReopenAfterStill", 8.0));
        MiniPage::fWidth = std::clamp(static_cast<float>(a_ini.GetDoubleValue("AutoTurn", "fMiniPageWidth", 0.25)), 0.1f, 0.9f);
        MiniPage::fHeight = std::clamp(static_cast<float>(a_ini.GetDoubleValue("AutoTurn", "fMiniPageHeight", 0.5)), 0.1f, 0.9f);
        MiniPage::fOpacity = std::clamp(static_cast<float>(a_ini.GetDoubleValue("AutoTurn", "fMiniPageOpacity", 0.92)), 0.2f, 1.0f);
    }

    // ---------------- VBoS script state ----------------

    bool FindVBoSObject() {
        if (vbosObject) return true;
        auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        auto dh = RE::TESDataHandler::GetSingleton();
        if (!vm || !dh) return false;
        auto policy = vm->GetObjectHandlePolicy();
        if (!policy) return false;
        if (!dh->LookupModByName(kVBoSPlugin)) return false;

        for (auto quest : dh->GetFormArray<RE::TESQuest>()) {
            if (!quest) continue;
            auto file = quest->GetFile(0);
            if (!file || std::string_view(file->fileName) != kVBoSPlugin) continue;
            for (auto alias : quest->aliases) {
                if (!alias) continue;
                auto handle = policy->GetHandleForObject(alias->GetVMTypeID(), alias);
                if (handle == policy->EmptyHandle()) continue;
                RE::BSTSmartPointer<RE::BSScript::Object> obj;
                if (vm->FindBoundObject(handle, kVBoSScript, obj) && obj) {
                    vbosObject = obj;
                    Log(fmt::format("Found VBoS script on quest {:08X}", quest->GetFormID()));
                    return true;
                }
            }
        }
        return false;
    }

    bool ReadBool(const char* a_name, bool a_isProperty) {
        if (!vbosObject) return false;
        const RE::BSScript::Variable* v =
            a_isProperty ? vbosObject->GetProperty(a_name) : vbosObject->GetVariable(a_name);
        return v && v->IsBool() ? v->GetBool() : false;
    }

    std::int32_t ReadInt(const char* a_name) {
        if (!vbosObject) return -1;
        const RE::BSScript::Variable* v = vbosObject->GetProperty(a_name);
        return v && v->IsInt() ? v->GetSInt() : -1;
    }

    void WriteBoolProp(const char* a_name, bool a_value) {
        if (!vbosObject) return;
        if (auto v = vbosObject->GetProperty(a_name)) v->SetBool(a_value);
    }

    void WriteIntProp(const char* a_name, std::int32_t a_value) {
        if (!vbosObject) return;
        if (auto v = vbosObject->GetProperty(a_name)) v->SetSInt(a_value);
    }

    // VoicedBooksPlayerAliasScript.SetPlaybackPaused(bool): pauses/unpauses VBoS's sound category and subtitles and
    // puts the master volume back (VBoS lowers it while narrating)
    void CallSetPlaybackPaused(bool a_pause) {
        auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm || !vbosObject) return;
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
        vm->DispatchMethodCall1(vbosObject, "SetPlaybackPaused", RE::MakeFunctionArguments(std::move(a_pause)), cb);
    }

    // Sound.StopInstance(int) and VBoSPapyrusExtensions.StopSubtitles()
    void CallStopInstance(std::int32_t a_instance) {
        auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm || a_instance == -1) return;
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
        vm->DispatchStaticCall("Sound", "StopInstance", RE::MakeFunctionArguments(std::move(a_instance)), cb);
    }

    // VoicedBooksPlayerAliasScript.StopPlayback(): VBoS's own clean-up (volume, time scale, subtitles)
    void CallStopPlayback() {
        auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm || !vbosObject) return;
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
        vm->DispatchMethodCall1(vbosObject, "StopPlayback", RE::MakeFunctionArguments(), cb);
    }

    void CallStopSubtitles() {
        auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) return;
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
        vm->DispatchStaticCall("VBoSPapyrusExtensions", "StopSubtitles", RE::MakeFunctionArguments(), cb);
    }

    // ---- resume: a book closed mid-narration keeps its narration paused until it is opened again ----
    struct Resume {
        bool valid = false;
        RE::FormID book = 0;
        std::int32_t instance = -1;
        double elapsed = 0.0;
        double duration = 0.0;
        int leftPage = -1;
        bool playing = false;  // the narration kept playing (came from walking), so its clock keeps running meanwhile
    };
    Resume pendingResume;       // set when a book was closed mid-narration
    Resume activeResume;        // the book being reopened: waiting for VBoS's restart to replace it with the paused narration
    double resumeWaitSec = 0.0;
    double resumeGuardSec = 0.0;  // after resuming: seconds left to watch for a late VBoS restart
    RE::FormID openedBookID = 0;
    int navTarget = -1;         // reopened book: page to go back to
    double navClock = 0.0;
    double navSentAt = -10.0;

    // BSSoundHandle::GetDuration (SE 66369 / AE 67630). Called directly because
    // the CommonLibSSE-NG version this project builds against doesn't declare it.
    std::uint64_t SoundHandleGetDuration(RE::BSSoundHandle* a_handle) {
        using func_t = std::uint64_t (*)(RE::BSSoundHandle*);
        static REL::Relocation<func_t> func{RELOCATION_ID(66369, 67630)};
        return func(a_handle);
    }

    double GetSoundDurationSec(std::int32_t a_instance) {
        RE::BSSoundHandle h;
        h.soundID = static_cast<std::uint32_t>(a_instance);
        h.assumeSuccess = false;
        h.state = RE::BSSoundHandle::AssumedState::kPlaying;
        const std::uint64_t ms = SoundHandleGetDuration(&h);
        return static_cast<double>(ms) / 1000.0;
    }

    // ---------------- page movie ----------------

    struct SpreadInfo {
        bool ok = false;
        int leftPage = -1;
        double endFraction = 0.0;  // where the open spread ends, 0..1 of the whole text
        bool isLastSpread = false;
        double pixelFraction = 0.0;  // the same, by text height
        bool byWords = false;        // endFraction came from word counts
        int wordsToEnd = 0;
        int totalWords = 0;
        double endTimeSec = -1.0;  // when the narrator finishes the spread's last word (from the timing data), -1 = unknown
        std::string spokenTail;    // the spoken words matched there (for the log)
        double matchQuality = 0.0;
    };

    std::string wordFail;
    bool loggedWordFail = false;

    std::wstring GFxText(const RE::GFxValue& a_v) {
        if (a_v.IsStringW()) {
            const wchar_t* w = a_v.GetStringW();
            return w ? std::wstring(w) : std::wstring();
        }
        if (!a_v.IsString()) return {};
        const char* u = a_v.GetString();
        if (!u || !*u) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, u, -1, nullptr, 0);
        if (n <= 1) return {};
        std::wstring out(static_cast<std::size_t>(n - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, u, -1, out.data(), n);
        return out;
    }

    // ---------------- words ----------------
    // A word = a run of letters/digits, lowercased; apostrophes are dropped ("Dwemer's" -> "dwemers"), every other
    // character separates words; "[pagebreak]" tags are not words. tools/make_vbos_timings.py normalizes the spoken
    // words exactly the same way, so book words and spoken words compare directly.
    bool IsWordChar(wchar_t a_lower) {
        return (a_lower >= L'a' && a_lower <= L'z') || (a_lower >= L'0' && a_lower <= L'9') || (a_lower >= 0xC0 && a_lower <= 0x24F);
    }
    bool IsApostrophe(wchar_t c) { return c == L'\'' || c == 0x2019; }

    std::string ToUtf8(const std::wstring& a_w) {
        if (a_w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, a_w.data(), static_cast<int>(a_w.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<std::size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, a_w.data(), static_cast<int>(a_w.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    struct BookWord {
        std::string w;
        std::size_t start;  // character offset in the page text
    };

    std::vector<BookWord> Tokenize(const std::wstring& a_text) {
        std::vector<BookWord> out;
        std::wstring cur;
        std::size_t curStart = 0;
        auto flush = [&]() {
            if (!cur.empty()) out.push_back({ToUtf8(cur), curStart});
            cur.clear();
        };
        for (std::size_t i = 0; i < a_text.size(); ++i) {
            if (a_text[i] == L'[' && a_text.compare(i, 11, L"[pagebreak]") == 0) {
                flush();
                i += 10;
                continue;
            }
            const wchar_t c = static_cast<wchar_t>(towlower(a_text[i]));
            if (IsApostrophe(c)) continue;  // dropped, doesn't split the word
            if (IsWordChar(c)) {
                if (cur.empty()) curStart = i;
                cur.push_back(c);
            } else {
                flush();
            }
        }
        flush();
        return out;
    }

    // words whose first character is before a_end
    int WordsBefore(const std::vector<BookWord>& a_words, std::size_t a_end) {
        int n = 0;
        for (auto& w : a_words) {
            if (w.start >= a_end) break;
            ++n;
        }
        return n;
    }

    // ---------------- narration timing data ----------------
    // Data\SKSE\Plugins\ReadItNow\VBoSTimings\<audio file name>.txt, one spoken word per line: "start end word"
    // (seconds), made from a speech-recognition pass over the Voiced Books audio (tools/make_vbos_timings.py).

    struct SpokenWord {
        float start;
        float end;
        std::string w;
    };

    std::unordered_map<RE::FormID, std::string> bookAudio;  // book FormID -> audio file name (no folder, no extension)
    std::vector<SpokenWord> spoken;                          // timing data of the open book
    std::string spokenName;
    double spokenRecordingSec = 0.0;  // length of the recording the timing data was made from ("# duration" line), 0 = unknown

    std::string Lower(std::string a_s) {
        for (auto& c : a_s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return a_s;
    }

    std::string Trim(const std::string& a_s) {
        const auto b = a_s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return {};
        const auto e = a_s.find_last_not_of(" \t\r\n");
        return a_s.substr(b, e - b + 1);
    }

    // Voiced Books reads every Data\*_VBOS.ini; lines look like  0x1AFB3~Skyrim.esm|sound\fx\voicedbooksofskyrim\x.xwm
    void LoadVBoSMappingsImpl() {
        bookAudio.clear();
        auto dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return;
        int files = 0, lines = 0, unresolved = 0;
        std::error_code ec;
        std::filesystem::directory_iterator it("Data", ec), endIt;
        for (; !ec && it != endIt; it.increment(ec)) {
            const auto& entry = *it;
            std::error_code fec;
            if (!entry.is_regular_file(fec)) continue;
            // wide name: Data can hold files whose names don't fit the ANSI code page (e.g. Chinese readme files),
            // and path::string() throws on those
            std::wstring fname = entry.path().filename().wstring();
            for (auto& c : fname) c = static_cast<wchar_t>(towlower(c));
            if (fname.size() < 9 || fname.compare(fname.size() - 9, 9, L"_vbos.ini") != 0) continue;
            ++files;
            std::ifstream in(entry.path());
            std::string line;
            while (std::getline(in, line)) {
                line = Trim(line);
                if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
                const auto tilde = line.find('~');
                const auto bar = line.find('|');
                if (tilde == std::string::npos || bar == std::string::npos || bar < tilde) continue;
                RE::FormID local = 0;
                try {
                    local = static_cast<RE::FormID>(std::stoul(Trim(line.substr(0, tilde)), nullptr, 16));
                } catch (...) {
                    continue;
                }
                const std::string plugin = Trim(line.substr(tilde + 1, bar - tilde - 1));
                std::string path = Trim(line.substr(bar + 1));
                const auto slash = path.find_last_of("\\/");
                if (slash != std::string::npos) path = path.substr(slash + 1);
                const auto dot = path.find_last_of('.');
                if (dot != std::string::npos) path = path.substr(0, dot);
                const RE::FormID id = dh->LookupFormID(local & 0x00FFFFFF, plugin);
                if (!id) {
                    ++unresolved;
                    continue;
                }
                bookAudio[id] = Lower(path);
                ++lines;
            }
        }
        spdlog::info("Voiced Books mappings: {} books from {} *_VBOS.ini files ({} for plugins not loaded)", lines, files, unresolved);
    }

    // never let a surprise in someone's Data folder or ini take the game down: log it and carry on without timing data
    void LoadVBoSMappings() {
        try {
            LoadVBoSMappingsImpl();
        } catch (const std::exception& e) {
            bookAudio.clear();
            spdlog::info("Voiced Books mappings: could not be read ({}); pages will be timed by word count", e.what());
        } catch (...) {
            bookAudio.clear();
            spdlog::info("Voiced Books mappings: could not be read (unknown error); pages will be timed by word count");
        }
    }

    void LoadTimingsForBookImpl(RE::TESObjectBOOK* a_book) {
        spoken.clear();
        spokenName.clear();
        spokenRecordingSec = 0.0;
        if (!a_book) {
            Log("Timing data: no book form");
            return;
        }
        auto it = bookAudio.find(a_book->GetFormID());
        if (it == bookAudio.end()) {
            Log(fmt::format("Timing data: book {:08X} has no Voiced Books audio", a_book->GetFormID()));
            return;
        }
        spokenName = it->second;
        const std::filesystem::path file = std::filesystem::path("Data/SKSE/Plugins/ReadItNow/VBoSTimings") / (spokenName + ".txt");
        std::ifstream in(file);
        if (!in) {
            Log(fmt::format("Timing data: none for {} (timing by word count instead)", spokenName));
            return;
        }
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("# duration ", 0) == 0) {
                try {
                    spokenRecordingSec = std::stod(line.substr(11));
                } catch (...) {
                }
                continue;
            }
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ls(line);
            SpokenWord w;
            if (ls >> w.start >> w.end >> w.w) spoken.push_back(std::move(w));
        }
        Log(fmt::format("Timing data: {} ({} spoken words, last ends at {:.1f}s)", spokenName, spoken.size(),
                        spoken.empty() ? 0.0 : spoken.back().end));
    }

    void LoadTimingsForBook(RE::TESObjectBOOK* a_book) {
        try {
            LoadTimingsForBookImpl(a_book);
        } catch (const std::exception& e) {
            spoken.clear();
            Log(fmt::format("Timing data: could not be read ({}); timing by word count", e.what()));
        } catch (...) {
            spoken.clear();
            Log("Timing data: could not be read (unknown error); timing by word count");
        }
    }

    int EditDistance(const std::string& a, const std::string& b) {
        if (a.size() > b.size() + 3 || b.size() > a.size() + 3) return 99;
        std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
        for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = static_cast<int>(j);
        for (std::size_t i = 1; i <= a.size(); ++i) {
            cur[0] = static_cast<int>(i);
            for (std::size_t j = 1; j <= b.size(); ++j)
                cur[j] = (std::min)({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] != b[j - 1] ? 1 : 0)});
            std::swap(prev, cur);
        }
        return prev[b.size()];
    }

    // 1 = same word, 0.7 = close (narrator/recognizer spelling, "Marobar"/"Marabar", "Sul"/"Sulan"), 0 = different
    double Similar(const std::string& a, const std::string& b) {
        if (a == b) return 1.0;
        if (a.size() >= 3 && b.size() >= 3 && (a.rfind(b, 0) == 0 || b.rfind(a, 0) == 0)) return 0.7;
        const std::size_t m = (std::max)(a.size(), b.size());
        if (m >= 4 && EditDistance(a, b) <= static_cast<int>(m / 3)) return 0.7;
        return 0.0;
    }

    // Find where the narrator says book word a_end-1: compare the last few book words of the spread with the spoken
    // words around every candidate position (allowing a word or two skipped/added) and take the best fit.
    // Returns the index into `spoken`, or -1.
    int MatchSpokenEnd(const std::vector<BookWord>& a_book, int a_end, double& a_quality) {
        a_quality = 0.0;
        const int nB = static_cast<int>(a_book.size());
        const int nT = static_cast<int>(spoken.size());
        if (a_end <= 0 || nT == 0 || nB == 0) return -1;
        const int K = (std::min)(8, a_end);
        const int expected = static_cast<int>(static_cast<long long>(a_end - 1) * nT / nB);
        const int radius = (std::max)(150, nT / 4);
        double bestScore = -1.0;
        int best = -1;
        for (int j = (std::max)(0, expected - radius); j <= (std::min)(nT - 1, expected + radius); ++j) {
            double score = 0.0;
            for (int k = 0; k < K; ++k) {
                const std::string& bw = a_book[a_end - 1 - k].w;
                double s = 0.0;
                static constexpr int offsets[] = {0, -1, 1, -2, 2};
                for (int d : offsets) {
                    const int jj = j - k + d;
                    if (jj < 0 || jj >= nT) continue;
                    double v = Similar(bw, spoken[jj].w);
                    if (d != 0) v *= 0.8;
                    if (v > s) s = v;
                    if (s >= 1.0) break;
                }
                score += s;
            }
            score -= std::abs(j - expected) / (10.0 * nT);  // tie-break toward the expected position
            if (score > bestScore) {
                bestScore = score;
                best = j;
            }
        }
        a_quality = bestScore / K;
        return best;
    }

    // character offset where the page starting at a_y begins (same calls as book.swf)
    double CharOffsetAtY(RE::GFxValue& a_field, double a_y) {
        RE::GFxValue args[2];
        args[0].SetNumber(0.0);
        args[1].SetNumber(a_y);
        RE::GFxValue line;
        if (!a_field.Invoke("getLineIndexAtPoint", &line, args, 2) || !line.IsNumber() || line.GetNumber() < 0) return -1;
        RE::GFxValue off;
        if (!a_field.Invoke("getLineOffset", &off, &line, 1) || !off.IsNumber()) return -1;
        return off.GetNumber();
    }

    struct SpreadInfo;
    bool WordFraction(RE::GFxValue& a_bm, double a_nextTop, SpreadInfo& a_info);

    bool GetBookMenuObj(RE::GFxMovieView* a_movie, RE::GFxValue& a_out) {
        // book.swf registers its BookMenu class instance as BookMenu.BookMenuInstance
        static const char* paths[] = {"BookMenu.BookMenuInstance", "_global.BookMenu.BookMenuInstance",
                                      "_root.BookMenu_mc", "_root.BookMenuInstance"};
        for (auto p : paths) {
            if (a_movie->GetVariable(&a_out, p) && a_out.IsObject()) {
                RE::GFxValue test;
                if (a_out.GetMember("PageInfoA", &test) && test.IsArray()) return true;
            }
        }
        return false;
    }

    double PageNumber(const RE::GFxValue& a_obj, const char* a_name, double a_default) {
        RE::GFxValue v;
        if (a_obj.GetMember(a_name, &v) && v.IsNumber()) return v.GetNumber();
        return a_default;
    }

    // ok = false while the page movie is still paginating, so a spread is never
    // judged from a half-built page list.
    std::string spreadFail;  // why the last ReadSpread failed (for the status line)

    SpreadInfo ReadSpread() {
        SpreadInfo info;
        spreadFail = "no UI";
        auto ui = RE::UI::GetSingleton();
        if (!ui) return info;
        spreadFail = "Book Menu not found";
        auto menu = ui->GetMenu<RE::BookMenu>();
        if (!menu) return info;
        auto& data = menu->GetRuntimeData();
        spreadFail = "it's a note";
        if (data.isNote) return info;  // single page notes never turn
        spreadFail = "no page movie";
        auto movie = data.book.get();
        if (!movie) return info;
        spreadFail = "BookMenuInstance/PageInfoA not found";

        RE::GFxValue bm;
        if (!GetBookMenuObj(movie, bm)) {
            if (!loggedPathFail) {
                spdlog::info("AutoTurn: could not find PageInfoA in the page movie; auto-turn disabled for this book");
                loggedPathFail = true;
            }
            return info;
        }

        // iPaginationIndex is the setInterval id while paginating, -1 once finished
        const double pagIdx = PageNumber(bm, "iPaginationIndex", 0);
        spreadFail = fmt::format("still paginating (iPaginationIndex={})", pagIdx);
        if (pagIdx != -1) return info;
        spreadFail = "PageInfoA not an array";

        RE::GFxValue pages;
        if (!bm.GetMember("PageInfoA", &pages) || !pages.IsArray()) return info;
        const int pageCount = static_cast<int>(pages.GetArraySize());
        const int left = static_cast<int>(PageNumber(bm, "iLeftPageNumber", -1));
        spreadFail = fmt::format("bad page numbers (iLeftPageNumber={}, pages={})", left, pageCount);
        if (left < 0 || pageCount == 0) return info;
        spreadFail = "last page entry unreadable";

        info.leftPage = left;
        info.isLastSpread = (left + 2 >= pageCount);

        // the spread ends where page left+2 starts (or at the end of the text)
        double nextTop = -1;
        if (left + 2 < pageCount) {
            RE::GFxValue nextPage;
            if (pages.GetElement(static_cast<std::uint32_t>(left + 2), &nextPage) && nextPage.IsObject()) {
                nextTop = PageNumber(nextPage, "pageTop", -1);
            }
        }

        // by pixel height of the text (the last PageInfoA entry starts at the end of the text)
        RE::GFxValue lastPage;
        if (!pages.GetElement(static_cast<std::uint32_t>(pageCount - 1), &lastPage) || !lastPage.IsObject())
            return info;
        const double totalHeight = PageNumber(lastPage, "pageTop", 0);
        spreadFail = fmt::format("text height {}", totalHeight);
        if (totalHeight <= 0) return info;
        spreadFail.clear();
        const double endY = nextTop >= 0 ? nextTop : totalHeight;
        info.pixelFraction = std::clamp(endY / totalHeight, 0.0, 1.0);

        // By WORDS: the narrator reads words, not pixels. Pictures, titles and blank space take height on the
        // page but no narration time. The page's text is cut out of ReferenceTextField exactly the way
        // book.swf's CreateDisplayPage does it: getLineOffset(getLineIndexAtPoint(0, pageTop)).
        info.endFraction = info.pixelFraction;
        info.byWords = false;
        if (!WordFraction(bm, nextTop, info)) {
            if (!loggedWordFail) {
                Log(fmt::format("Could not read the page text ({}); timing by text height instead", wordFail));
                loggedWordFail = true;
            }
        }
        info.ok = true;
        return info;
    }

    // per open book: the page text's words, and the result for each spread end already worked out
    std::vector<BookWord> bookWords;
    std::size_t bookTextLen = 0;
    struct EndCache {
        int wordsToEnd;
        double endTimeSec;
        std::string spokenTail;
        double quality;
    };
    std::map<double, EndCache> endCache;  // key: pageTop of the next spread (-1 = end of the book)

    bool WordFraction(RE::GFxValue& a_bm, double a_nextTop, SpreadInfo& a_info) {
        RE::GFxValue field;
        if (!a_bm.GetMember("ReferenceTextField", &field) || !field.IsDisplayObject() && !field.IsObject()) {
            wordFail = "ReferenceTextField not found";
            return false;
        }
        if (bookWords.empty()) {
            RE::GFxValue textV;
            if (!field.GetMember("text", &textV)) {
                wordFail = "ReferenceTextField.text not readable";
                return false;
            }
            const std::wstring text = GFxText(textV);
            if (text.empty()) {
                wordFail = "the text is empty";
                return false;
            }
            bookWords = Tokenize(text);
            bookTextLen = text.size();
            endCache.clear();
            if (bookWords.empty()) {
                wordFail = "no words in the text";
                return false;
            }
        }
        const int total = static_cast<int>(bookWords.size());
        auto cached = endCache.find(a_nextTop);
        if (cached == endCache.end()) {
            double endChar = static_cast<double>(bookTextLen);
            if (a_nextTop >= 0) {
                endChar = CharOffsetAtY(field, a_nextTop);
                if (endChar < 0) {
                    wordFail = fmt::format("getLineIndexAtPoint/getLineOffset failed at y={}", a_nextTop);
                    return false;
                }
            }
            EndCache c{WordsBefore(bookWords, static_cast<std::size_t>(endChar)), -1.0, {}, 0.0};
            if (!spoken.empty()) {
                if (a_nextTop < 0) {
                    c.endTimeSec = spoken.back().end;  // end of the book
                    c.quality = 1.0;
                    c.spokenTail = spoken.back().w;
                } else {
                    double q = 0.0;
                    const int j = MatchSpokenEnd(bookWords, c.wordsToEnd, q);
                    c.quality = q;
                    if (j >= 0) {
                        for (int k = (std::max)(0, j - 4); k <= j; ++k) c.spokenTail += (k > (std::max)(0, j - 4) ? " " : "") + spoken[k].w;
                        if (q >= 0.5) c.endTimeSec = spoken[j].end;
                    }
                }
            }
            cached = endCache.emplace(a_nextTop, std::move(c)).first;
        }
        const EndCache& c = cached->second;
        a_info.wordsToEnd = c.wordsToEnd;
        a_info.totalWords = total;
        a_info.endFraction = std::clamp(static_cast<double>(c.wordsToEnd) / total, 0.0, 1.0);
        a_info.byWords = true;
        a_info.endTimeSec = c.endTimeSec;
        a_info.spokenTail = c.spokenTail;
        a_info.matchQuality = c.quality;
        return true;
    }

    // ---------------- key press ----------------

    void SendKey(bool a_down) {
        if (iTurnMethod == 2) {
            // a "NextPage" button event handed straight to the input dispatcher, the way the game passes on a
            // real key press after mapping it (the Book Menu's input handler gets it like any other input)
            auto mgr = RE::BSInputDeviceManager::GetSingleton();
            if (!mgr) return;
            static RE::ButtonEvent* button = nullptr;  // kept for reuse, never freed
            if (!button) {
                button = RE::ButtonEvent::Create(RE::INPUT_DEVICE::kKeyboard, RE::UserEvents::GetSingleton()->nextPage, turnKey,
                                                 1.0f, 0.0f);
                if (!button) return;
            }
            button->idCode = turnKey;
            button->next = nullptr;
            button->value = a_down ? 1.0f : 0.0f;
            button->heldDownSecs = a_down ? 0.0f : 0.1f;
            RE::InputEvent* list = button;
            mgr->SendEvent(&list);
            return;
        }
        if (iTurnMethod == 1) {
            INPUT in{};
            in.type = INPUT_KEYBOARD;
            // DirectInput scan code -> Windows scan code (extended keys have 0x80 set)
            in.ki.wScan = static_cast<WORD>(iTurnKey & 0x7F);
            in.ki.dwFlags = KEYEVENTF_SCANCODE | ((iTurnKey & 0x80) ? KEYEVENTF_EXTENDEDKEY : 0) |
                            (a_down ? 0 : KEYEVENTF_KEYUP);
            SendInput(1, &in, sizeof(INPUT));
            return;
        }
        auto queue = RE::BSInputEventQueue::GetSingleton();
        if (!queue) return;
        if (a_down) {
            queue->AddButtonEvent(RE::INPUT_DEVICE::kKeyboard, static_cast<std::int32_t>(turnKey), 1.0f, 0.0f);
        } else {
            queue->AddButtonEvent(RE::INPUT_DEVICE::kKeyboard, static_cast<std::int32_t>(turnKey), 0.0f, 0.1f);
        }
    }

    // ---------------- per tick (UI thread) ----------------

    // ---------------- test mode: which route turns a page? ----------------
    // Each route is tried once, 3 s apart, while a book is open. Before each try the open spread
    // (iLeftPageNumber of the page movie) is read; 1.5 s later it is read again and the result is logged.
    struct Probe
    {
        const char* name;
        int         kind;  // 1 dispatcher event, 2 MenuControls, 3 Windows key, 4 page movie call
        std::uint32_t key;
    };
    const Probe kProbes[] = {
        { "NextPage button event to the input dispatcher (BSInputDeviceManager)", 1, 0 },
        { "NextPage button event straight to MenuControls", 2, 0 },
        { "Windows key press: arrow key (Right forward / Left back)", 3, 0xCD },
        { "Windows key press: your mapped Next/Prev Page key", 3, 0 },
        { "page movie TurnPage(2) only (text changes, not the 3D book)", 4, 0 },
    };
    constexpr int kProbeCount = 5;
    int    probeIndex = -1;
    double probeClock = 0.0;     // seconds since the book opened
    double probeSentAt = 0.0;
    int    probeLeftBefore = -1;
    bool   probeReleasePending = false;
    const Probe* probeCurrent = nullptr;
    bool   probeBack = false;      // this try turns back (the book was on its last spread)
    std::uint32_t prevKey = 0xCB;  // Left Arrow, or the mapped "PrevPage" key

    int CurrentPageCount() {
        auto ui = RE::UI::GetSingleton();
        if (!ui) return -1;
        auto menu = ui->GetMenu<RE::BookMenu>();
        if (!menu) return -1;
        auto movie = menu->GetRuntimeData().book.get();
        if (!movie) return -1;
        RE::GFxValue bm, pages;
        if (!GetBookMenuObj(movie, bm) || !bm.GetMember("PageInfoA", &pages) || !pages.IsArray()) return -1;
        return static_cast<int>(pages.GetArraySize());
    }

    int CurrentLeftPage() {
        auto ui = RE::UI::GetSingleton();
        if (!ui) return -1;
        auto menu = ui->GetMenu<RE::BookMenu>();
        if (!menu) return -1;
        auto movie = menu->GetRuntimeData().book.get();
        if (!movie) return -1;
        RE::GFxValue bm;
        if (!GetBookMenuObj(movie, bm)) return -1;
        return static_cast<int>(PageNumber(bm, "iLeftPageNumber", -1));
    }

    void ProbeButton(int a_kind, bool a_down) {
        static RE::ButtonEvent* button = nullptr;
        if (!button) {
            button = RE::ButtonEvent::Create(RE::INPUT_DEVICE::kKeyboard, RE::UserEvents::GetSingleton()->nextPage, turnKey, 1.0f, 0.0f);
            if (!button) return;
        }
        button->userEvent = probeBack ? RE::UserEvents::GetSingleton()->prevPage : RE::UserEvents::GetSingleton()->nextPage;
        button->idCode = probeBack ? prevKey : turnKey;
        button->next = nullptr;
        button->value = a_down ? 1.0f : 0.0f;
        button->heldDownSecs = a_down ? 0.0f : 0.2f;
        RE::InputEvent* list = button;
        if (a_kind == 1) {
            if (auto mgr = RE::BSInputDeviceManager::GetSingleton()) mgr->SendEvent(&list);
        } else {
            if (auto mc = RE::MenuControls::GetSingleton()) mc->ProcessEvent(&list, nullptr);
        }
    }

    void ProbeKey(std::uint32_t a_dik, bool a_down) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = static_cast<WORD>(a_dik & 0x7F);
        in.ki.dwFlags = KEYEVENTF_SCANCODE | ((a_dik & 0x80) ? KEYEVENTF_EXTENDEDKEY : 0) | (a_down ? 0 : KEYEVENTF_KEYUP);
        SendInput(1, &in, sizeof(INPUT));
    }

    void ProbeSend(const Probe& a_p, bool a_down) {
        switch (a_p.kind) {
        case 1:
        case 2:
            ProbeButton(a_p.kind, a_down);
            break;
        case 3:
            if (probeBack) {
                ProbeKey(a_p.key ? 0xCB : prevKey, a_down);  // Left Arrow, or the mapped PrevPage key
            } else {
                ProbeKey(a_p.key ? a_p.key : turnKey, a_down);
            }
            break;
        case 4:
            if (a_down) {
                auto menu = RE::UI::GetSingleton()->GetMenu<RE::BookMenu>();
                RE::GFxValue bm;
                if (menu && menu->GetRuntimeData().book && GetBookMenuObj(menu->GetRuntimeData().book.get(), bm)) {
                    RE::GFxValue arg(probeBack ? -2.0 : 2.0), result;
                    bm.Invoke("TurnPage", &result, &arg, 1);
                }
            }
            break;
        }
    }

    void ProbeTick(double a_dt) {
        probeClock += a_dt;
        if (probeCurrent) {
            if (probeReleasePending && probeClock - probeSentAt >= 0.2) {
                ProbeSend(*probeCurrent, false);
                probeReleasePending = false;
            }
            if (probeClock - probeSentAt >= 1.5) {
                const int after = CurrentLeftPage();
                spdlog::info("PROBE {}: {}  (spread was page {}, now page {})", probeIndex + 1,
                             after != probeLeftBefore ? "TURNED" : "no change", probeLeftBefore, after);
                probeCurrent = nullptr;
            }
            return;
        }
        if (probeIndex + 1 >= kProbeCount) return;
        if (probeClock < 3.0 * (probeIndex + 2)) return;  // first try 3 s after the book opened, then every 3 s
        const int left = CurrentLeftPage();
        const int count = CurrentPageCount();
        if (left < 0 || count <= 0) return;
        ++probeIndex;
        probeCurrent = &kProbes[probeIndex];
        probeBack = left + 2 >= count;  // on the last spread: this try turns back
        probeLeftBefore = left;
        probeSentAt = probeClock;
        probeReleasePending = true;
        std::uint32_t shownKey = probeBack ? prevKey : turnKey;
        if (probeCurrent->kind == 3 && probeCurrent->key) shownKey = probeBack ? 0xCB : probeCurrent->key;
        spdlog::info("PROBE {}: {} - {} (key 0x{:02X}), spread at page {} of {}", probeIndex + 1, probeCurrent->name,
                     probeBack ? "BACK" : "FORWARD", shownKey, left, count);
        ProbeSend(*probeCurrent, true);
    }

    // ---------------- read while walking ----------------
    // Forward key while a narrated book is open (and the narration is running): the book screen closes, the
    // narration plays on, and a mini page in the upper-right corner shows the page the narrator is on. In combat the
    // narration pauses and picks up again when combat ends. Standing still for fReopenAfterStill seconds out of
    // combat reopens the full book on the narrator's pages (handed over like a resume, see Resume above).
    struct Walk {
        bool active = false;
        RE::FormID book = 0;
        std::string title;
        RE::ObjectRefHandle ref;               // the book in the world, if it was read from there
        std::int32_t instance = -1;
        std::vector<std::wstring> pages;       // text of every page
        std::vector<double> pageEnd;           // when the narrator finishes each page (s)
        std::vector<int> pageWords;
        bool combatPaused = false;
        double stillSec = 0.0;
        RE::NiPoint3 lastPos;
        int shownPage = -1;
        int reopenTries = 0;  // failed attempts to reopen the book; after 3, wait until the player moves again
    };
    Walk walk;
    std::atomic<bool> walkRequested{false};
    double reopenWaitSec = 0.0;  // asked the game to reopen the walking book, waiting for its Book Menu
    std::uint32_t forwardKey = 0x11;  // W

    std::wstring CleanPageText(std::wstring a_t) {
        for (std::size_t p; (p = a_t.find(L"[pagebreak]")) != std::wstring::npos;) a_t.erase(p, 11);
        for (auto& c : a_t)
            if (c == L'\r') c = L'\n';
        std::wstring out;
        int newlines = 0;
        for (wchar_t c : a_t) {
            if (c == L'\n') {
                if (++newlines > 2) continue;
            } else {
                newlines = 0;
            }
            out.push_back(c);
        }
        const auto b = out.find_first_not_of(L" \t\n");
        if (b == std::wstring::npos) return {};
        const auto e = out.find_last_not_of(L" \t\n");
        return out.substr(b, e - b + 1);
    }

    // everything the mini page needs, taken from the page movie while the book is still open
    bool CaptureWalkPages() {
        auto ui = RE::UI::GetSingleton();
        auto menu = ui ? ui->GetMenu<RE::BookMenu>() : nullptr;
        if (!menu || !menu->GetRuntimeData().book) return false;
        RE::GFxValue bm, field, pages, textV;
        if (!GetBookMenuObj(menu->GetRuntimeData().book.get(), bm)) return false;
        if (!bm.GetMember("ReferenceTextField", &field) || !bm.GetMember("PageInfoA", &pages) || !pages.IsArray()) return false;
        if (!field.GetMember("text", &textV)) return false;
        const std::wstring text = GFxText(textV);
        const int count = static_cast<int>(pages.GetArraySize());
        if (text.empty() || count <= 0) return false;
        if (bookWords.empty()) bookWords = Tokenize(text);

        std::vector<std::size_t> offs(static_cast<std::size_t>(count) + 1, text.size());
        offs[0] = 0;
        for (int i = 1; i < count; ++i) {
            RE::GFxValue pg;
            if (!pages.GetElement(static_cast<std::uint32_t>(i), &pg) || !pg.IsObject()) return false;
            const double off = CharOffsetAtY(field, PageNumber(pg, "pageTop", -1));
            if (off < 0) return false;
            offs[i] = (std::min)(static_cast<std::size_t>(off), text.size());
        }
        walk.pages.clear();
        walk.pageEnd.clear();
        walk.pageWords.clear();
        const int total = static_cast<int>(bookWords.size());
        double prevEnd = 0.0;
        for (int i = 0; i < count; ++i) {
            const std::size_t a = offs[i], b = (std::max)(offs[i], offs[i + 1]);
            walk.pages.push_back(CleanPageText(text.substr(a, b - a)));
            const int wordsEnd = WordsBefore(bookWords, b);
            walk.pageWords.push_back(wordsEnd - WordsBefore(bookWords, a));
            double end = -1.0;
            if (i == count - 1) {
                end = spoken.empty() ? durationSec : spoken.back().end;
            } else if (!spoken.empty() && wordsEnd > 0) {
                double q = 0.0;
                const int j = MatchSpokenEnd(bookWords, wordsEnd, q);
                if (j >= 0 && q >= 0.5) end = spoken[j].end;
            }
            if (end < 0.0) end = total > 0 ? static_cast<double>(wordsEnd) / total * durationSec : durationSec;
            end = (std::max)(end, prevEnd);
            prevEnd = end;
            walk.pageEnd.push_back(end);
        }
        return true;
    }

    void BeginWalk() {
        auto* book = RE::BookMenu::GetTargetForm();
        if (!book || !CaptureWalkPages()) {
            Log("Read while walking: couldn't read the pages, staying in the book");
            return;
        }
        walk.active = true;
        walk.book = book->GetFormID();
        walk.title = book->GetName() ? book->GetName() : "";
        walk.ref = {};
        if (auto ref = RE::BookMenu::GetTargetReference()) walk.ref = ref->GetHandle();
        walk.instance = trackedInstance;
        walk.combatPaused = false;
        walk.stillSec = 0.0;
        walk.shownPage = -1;
        walk.reopenTries = 0;
        if (auto player = RE::PlayerCharacter::GetSingleton()) walk.lastPos = player->GetPosition();
        Log(fmt::format("Read while walking: {} pages, narration at {:.1f}s of {:.1f}s", walk.pages.size(), elapsedSec, durationSec));
        auto queue = RE::UIMessageQueue::GetSingleton();
        queue->AddMessage(RE::BookMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
        // a book read from the inventory or a container sits on top of that menu: close it too, or it comes back
        // into view when the book closes and the mini page waits behind it
        if (auto ui = RE::UI::GetSingleton()) {
            static constexpr const char* underMenus[] = {"InventoryMenu", "ContainerMenu", "FavoritesMenu", "TweenMenu"};
            for (auto m : underMenus) {
                if (ui->IsMenuOpen(m)) {
                    queue->AddMessage(RE::BSFixedString(m), RE::UI_MESSAGE_TYPE::kHide, nullptr);
                    Log(fmt::format("Read while walking: closed the {} the book was read from", m));
                }
            }
        }
    }

    void EndWalk(const char* a_why) {
        if (!walk.active) return;
        walk.active = false;
        MiniPage::Hide();
        Log(fmt::format("Read while walking ended: {}", a_why));
    }

    int CurrentWalkPage() {
        int lastWithWords = 0;
        for (int i = 0; i < static_cast<int>(walk.pages.size()); ++i) {
            if (walk.pageWords[i] <= 0) continue;
            lastWithWords = i;
            if (elapsedSec < walk.pageEnd[i] + fTurnDelay) return i;
        }
        return lastWithWords;
    }

    bool PlayerHasItem(RE::TESBoundObject* a_obj) {
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player || !a_obj) return false;
        auto counts = player->GetInventoryCounts([a_obj](RE::TESBoundObject& o) { return &o == a_obj; });
        auto it = counts.find(a_obj);
        return it != counts.end() && it->second > 0;
    }

    void ReopenWalkBook() {
        auto player = RE::PlayerCharacter::GetSingleton();
        auto book = RE::TESForm::LookupByID<RE::TESObjectBOOK>(walk.book);
        if (!player || !book) {
            walk.reopenTries++;
            walk.stillSec = 0.0;
            return;
        }
        const int page = CurrentWalkPage();
        pendingResume = {true, walk.book, walk.instance, elapsedSec, durationSec, (page / 2) * 2, true};
        // the copy in the inventory first; a book in the world only when it's close (Skyrim Souls RE closes book
        // menus of far-away books straight away)
        auto ref = walk.ref.get();
        bool opened = false;
        if (PlayerHasItem(book)) {
            // open the Book Menu directly, the way the Inventory menu does when you read a book there
            // (equipping the book with no menu open does not bring up the Book Menu)
            RE::BSString text;
            book->GetDescription(text, nullptr);
            const RE::NiPoint3 pos{};
            const RE::NiMatrix3 rot{};
            RE::BookMenu::OpenBookMenu(text, nullptr, nullptr, book, pos, rot, 1.0f, true);
            opened = true;
        } else if (ref && !ref->IsDisabled() && !ref->IsDeleted() && ref->GetBaseObject() == book &&
                   ref->GetPosition().GetDistance(player->GetPosition()) < 350.0f) {
            ref->ActivateRef(player, 0, book, 1, false);
            opened = true;
        }
        if (!opened) {
            pendingResume = {};
            walk.reopenTries++;
            walk.stillSec = 0.0;
            Log("Read while walking: the book isn't in reach any more, staying on the mini page");
            return;
        }
        Log(fmt::format("Read while walking: stood still {:.0f}s, reopening the book at page {}", fReopenAfterStill, (page / 2) * 2));
        reopenWaitSec = 0.0;
        walk.active = false;  // Start() takes over from here (the pending resume)
        MiniPage::Hide();
    }

    // menus the player is busy in (with Skyrim Souls RE the game keeps running under them)
    bool OtherMenuOpen() {
        auto ui = RE::UI::GetSingleton();
        if (!ui) return false;
        static constexpr std::string_view menus[] = {
            "InventoryMenu", "ContainerMenu", "MagicMenu", "MapMenu", "Journal Menu", "TweenMenu", "BarterMenu",
            "Dialogue Menu", "Console", "Lockpicking Menu", "FavoritesMenu", "StatsMenu", "Sleep/Wait Menu",
            "MessageBoxMenu", "GiftMenu", "Training Menu", "LevelUp Menu", "Crafting Menu", "Book Menu", "Loading Menu",
            "Main Menu", "Tutorial Menu", "CustomMenu", "Mod Manager Menu"};
        for (auto m : menus)
            if (ui->IsMenuOpen(m)) return true;
        return false;
    }

    void WalkTick(double dt) {
        auto ui = RE::UI::GetSingleton();
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        if (ui && ui->GameIsPaused()) return;  // a menu is open: the narration clock stops with the game

        const bool combat = player->IsInCombat();
        if (combat && !walk.combatPaused) {
            walk.combatPaused = true;
            CallSetPlaybackPaused(true);
            Log(fmt::format("Read while walking: combat, narration paused at {:.1f}s", elapsedSec));
        } else if (!combat && walk.combatPaused) {
            walk.combatPaused = false;
            CallSetPlaybackPaused(false);
            Log(fmt::format("Read while walking: combat over, narration continues at {:.1f}s", elapsedSec));
        }
        if (!walk.combatPaused) elapsedSec += dt;

        if (elapsedSec >= durationSec) {
            CallStopPlayback();  // VBoS's clean-up: the narration was detached from it, so it won't do this itself
            EndWalk("the narration finished");
            running = false;
            return;
        }

        const int page = CurrentWalkPage();
        const bool menuOpen = OtherMenuOpen();
        if (menuOpen) {
            if (walk.shownPage != -1) {
                MiniPage::Hide();
                walk.shownPage = -1;  // shown again when the menu closes
            }
        } else if (page != walk.shownPage) {
            walk.shownPage = page;
            const std::string header = fmt::format("{}  -  page {} of {}", walk.title, page + 1, walk.pages.size());
            MiniPage::Show(header, ToUtf8(walk.pages[page]), walk.combatPaused);
            Log(fmt::format("Mini page: page {} (narration at {:.1f}s, page ends at {:.1f}s)", page, elapsedSec, walk.pageEnd[page]));
        }
        MiniPage::SetPaused(walk.combatPaused);

        // standing still: under ~15 units a second counts as not moving (turning the camera is fine)
        const RE::NiPoint3 pos = player->GetPosition();
        const bool moved = pos.GetDistance(walk.lastPos) > 15.0 * dt;
        walk.lastPos = pos;
        if (moved) walk.reopenTries = 0;
        if (combat || moved || menuOpen || walk.reopenTries >= 3) {
            walk.stillSec = 0.0;
        } else {
            walk.stillSec += dt;
            if (fReopenAfterStill > 0.0f && walk.stillSec >= fReopenAfterStill) ReopenWalkBook();
        }
        hbReason = fmt::format("walking: page {} of {}, still {:.1f}s{}", page, walk.pages.size(), walk.stillSec,
                               walk.combatPaused ? " (combat pause)" : "");
    }

    class WalkInputSink : public RE::BSTEventSink<RE::InputEvent*> {
    public:
        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override {
            if (!a_event || !bReadWhileWalking || !running || walk.active) return RE::BSEventNotifyControl::kContinue;
            for (auto e = *a_event; e; e = e->next) {
                if (e->GetEventType() == RE::INPUT_EVENT_TYPE::kButton) {
                    auto b = e->AsButtonEvent();
                    if (b && b->GetDevice() == RE::INPUT_DEVICE::kKeyboard && b->GetIDCode() == forwardKey && b->IsDown())
                        walkRequested = true;
                } else if (e->GetEventType() == RE::INPUT_EVENT_TYPE::kThumbstick) {
                    auto t = static_cast<RE::ThumbstickEvent*>(e);
                    if (t->IsLeft() && t->yValue > 0.7f) walkRequested = true;
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    WalkInputSink g_walkInput;

    // ---- heartbeat (diagnostics): what Tick saw, logged every 3 s while a book is open ----
    double hbMaxDt = 0.0;
    int hbTicks = 0;
    double hbClock = 0.0;

    void TickInner(double a_dt);

    void Tick() {
        if (!running) return;
        auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - lastTick).count();
        lastTick = now;
        if (dt > 1.0) Log(fmt::format("Tick gap: {:.1f}s since the last tick (game wasn't running UI tasks)", dt));
        hbTicks++;
        hbMaxDt = (std::max)(hbMaxDt, dt);
        hbClock += dt;
        hbReason = "ok";
        TickInner(dt);
        if (hbClock >= 3.0 && !bProbe) {
            Log(fmt::format("  status: {} ticks, max gap {:.2f}s, narration {:.1f}/{:.1f}s, instance {}, spread page {}, {}",
                            hbTicks, hbMaxDt, elapsedSec, durationSec, trackedInstance, shownLeftPage, hbReason));
            hbClock = 0.0;
            hbTicks = 0;
            hbMaxDt = 0.0;
        }
    }

    void TickInner(double dt) {

        if (releasePending > 0) {
            if (--releasePending == 0) SendKey(false);
        }

        if (bProbe) {
            ProbeTick(dt);
            return;  // test mode: no automatic turning
        }

        if (!FindVBoSObject()) { hbReason = "VBoS script not found"; return; }

        if (walk.active) {
            WalkTick(dt);
            return;
        }
        if (pendingResume.valid && pendingResume.playing) {
            // reopening the walking book: if the Book Menu doesn't come up, go back to the mini page
            auto ui = RE::UI::GetSingleton();
            pendingResume.elapsed += dt;
            reopenWaitSec += dt;
            if (reopenWaitSec > 2.0 && !(ui && ui->IsMenuOpen(RE::BookMenu::MENU_NAME))) {
                elapsedSec = pendingResume.elapsed;
                pendingResume = {};
                walk.active = true;
                walk.reopenTries++;
                walk.stillSec = 0.0;
                walk.shownPage = -1;
                Log(fmt::format("Read while walking: the book didn't open again (try {} of 3), back to the mini page", walk.reopenTries));
            }
            hbReason = "reopening the book";
            return;
        }

        if (activeResume.valid) {
            // the reopened book: VBoS restarts the narration from the beginning when a book is opened (its OnBookRead).
            // Its restart also un-pauses VBoS's sound category, so the paused narration plays on. Stop the restart
            // and hand VBoS the paused narration back.
            const std::int32_t inst = ReadInt("iSoundPlayInstance");
            resumeWaitSec += dt;
            if (activeResume.playing) activeResume.elapsed += dt;
            if (inst != -1 && inst != activeResume.instance) {
                CallStopInstance(inst);
                CallStopSubtitles();
                WriteIntProp("iSoundPlayInstance", activeResume.instance);
                WriteBoolProp("bIsPlaying", true);
                Log(fmt::format("Resumed: stopped VBoS's restart (instance {}) and continued the paused narration at {:.1f}s",
                                inst, activeResume.elapsed));
            } else if (resumeWaitSec >= 1.5) {
                // VBoS didn't restart (automatic reading off?): un-pause the narration ourselves
                WriteIntProp("iSoundPlayInstance", activeResume.instance);
                WriteBoolProp("bIsPlaying", true);
                CallSetPlaybackPaused(false);
                Log(fmt::format("Resumed: VBoS didn't restart the narration; un-paused it at {:.1f}s", activeResume.elapsed));
            } else {
                hbReason = "waiting for VBoS to start the reopened book";
                return;
            }
            trackedInstance = activeResume.instance;
            elapsedSec = activeResume.elapsed;
            durationSec = activeResume.duration;
            navTarget = activeResume.leftPage;
            navClock = 0.0;
            navSentAt = -10.0;
            resumeGuardSec = 5.0;
            activeResume = {};
            return;
        }

        if (resumeGuardSec > 0.0) {
            resumeGuardSec -= dt;
            const std::int32_t inst = ReadInt("iSoundPlayInstance");
            if (inst != -1 && inst != trackedInstance) {
                // VBoS restarted the book late: stop the restart again, keep the resumed narration
                CallStopInstance(inst);
                CallStopSubtitles();
                WriteIntProp("iSoundPlayInstance", trackedInstance);
                WriteBoolProp("bIsPlaying", true);
                Log(fmt::format("Stopped a late VBoS restart (instance {}), kept the resumed narration", inst));
            }
        }

        const bool playing = ReadBool("bIsPlaying", true);
        const std::int32_t instance = ReadInt("iSoundPlayInstance");
        if (!playing || instance == -1) {
            hbReason = fmt::format("waiting: VBoS bIsPlaying={} iSoundPlayInstance={}", playing, instance);
            return;
        }

        if (instance != trackedInstance) {
            trackedInstance = instance;
            elapsedSec = 0.0;
            durationSec = GetSoundDurationSec(instance);
            shownLeftPage = -1;
            Log(fmt::format("Narration started: instance {} duration {:.1f}s", instance, durationSec));
            return;  // start counting from the next tick
        }
        if (durationSec <= 1.0) {
            durationSec = GetSoundDurationSec(instance);  // the sound may not have been ready yet
            if (durationSec <= 1.0) return;
            Log(fmt::format("Narration length: {:.1f}s", durationSec));
            shownLeftPage = -1;  // log the open spread again, now with its turn time
        }
        if (!spoken.empty() && spokenRecordingSec > 0.0 && std::abs(spokenRecordingSec - durationSec) > 0.25) {
            // made from a different recording of this book (another narrator): its times don't apply
            Log(fmt::format("Timing data for {} is from a {:.1f}s recording, this narration is {:.1f}s: not used (timing by word count)",
                            spokenName, spokenRecordingSec, durationSec));
            spoken.clear();
            endCache.clear();
            shownLeftPage = -1;
        }

        const bool paused = ReadBool("bIsPaused", false) || ReadBool("bIsKillcamPaused", false);
        if (!paused) elapsedSec += dt;

        const SpreadInfo spread = ReadSpread();
        if (!spread.ok) { hbReason = fmt::format("page info not readable: {} (paused={})", spreadFail, paused); return; }
        // when the narrator finishes the spread: from the timing data if the spread's last words were found in it,
        // otherwise estimated from the word count
        const bool timed = spread.endTimeSec >= 0.0;
        const double spreadEndSec = timed ? spread.endTimeSec : spread.endFraction * durationSec;
        hbReason = fmt::format("paused={} narration {:.1f}s / spread ends {:.1f}s ({}){}{}", paused, elapsedSec, spreadEndSec,
                               timed ? "timing data" : "word count", spread.isLastSpread ? " (last spread)" : "",
                               turnedThisSpread ? " (key already sent)" : "");

        if (spread.leftPage != shownLeftPage) {
            shownLeftPage = spread.leftPage;
            elapsedAtShow = elapsedSec;
            turnedThisSpread = false;
            turnRetries = 0;
            if (spread.isLastSpread) {
                Log(fmt::format("Spread at page {}: last spread, no turn (narration at {:.1f}s)", spread.leftPage, elapsedSec));
            } else if (elapsedAtShow >= spreadEndSec) {
                Log(fmt::format("Spread at page {}: already narrated, no turn (narration at {:.1f}s)", spread.leftPage, elapsedSec));
            } else {
                std::string how;
                if (timed) {
                    how = fmt::format("narrator says \"{}\" at {:.2f}s (match {:.0f}%)", spread.spokenTail, spread.endTimeSec,
                                      spread.matchQuality * 100.0);
                } else if (!spoken.empty()) {
                    how = fmt::format("no good match in the timing data (best \"{}\", {:.0f}%), estimated by word count",
                                      spread.spokenTail, spread.matchQuality * 100.0);
                } else {
                    how = "no timing data, estimated by word count";
                }
                Log(fmt::format("Spread at page {} ends at word {} of {}: {}. Turns at {:.2f}s (narration at {:.1f}s of {:.1f}s)",
                                spread.leftPage, spread.wordsToEnd, spread.totalWords, how, spreadEndSec + fTurnDelay, elapsedSec,
                                durationSec));
            }
        }

        if (bReadWhileWalking && trackedInstance != -1 && durationSec > 1.0 && elapsedSec < durationSec - 1.0 && navTarget < 0) {
            auto player = RE::PlayerCharacter::GetSingleton();
            if (player && player->IsInCombat()) {
                // the game keeps running under the book (Skyrim Souls RE): a fight shrinks it to the mini page;
                // the walking tick pauses the narration until the fight is over
                Log(fmt::format("Combat while reading: switching to the mini page (narration at {:.1f}s)", elapsedSec));
                walkRequested = false;
                BeginWalk();
                if (walk.active) return;
            }
        }

        if (walkRequested.exchange(false)) {
            if (trackedInstance != -1 && durationSec > 1.0 && elapsedSec < durationSec - 1.0 && navTarget < 0) {
                BeginWalk();
                if (walk.active) return;
            } else {
                Log("Read while walking: the narration isn't running, staying in the book");
            }
        }

        if (navTarget >= 0) {
            // reopened book: page forward to where it was closed, one spread at a time
            navClock += dt;
            if (spread.leftPage >= navTarget || navClock > 15.0) {
                Log(spread.leftPage >= navTarget ? fmt::format("Back at page {}", spread.leftPage)
                                                 : fmt::format("Gave up paging to page {} (at page {})", navTarget, spread.leftPage));
                navTarget = -1;
            } else {
                hbReason += fmt::format(" (paging back to page {})", navTarget);
                if (releasePending == 0 && navClock - navSentAt > 0.8) {
                    navSentAt = navClock;
                    SendKey(true);
                    releasePending = 2;
                }
                return;
            }
        }

        if (releasePending > 0 || spread.isLastSpread) return;

        if (turnedThisSpread) {
            // the key was sent but the spread didn't change: try once more
            if (turnRetries < 1 && elapsedSec - turnSentAt > 1.5) {
                turnRetries++;
                turnSentAt = elapsedSec;
                Log("Page didn't turn after 1.5s, pressing the key again");
                SendKey(true);
                releasePending = 2;
            }
            return;
        }
        // paged back to something already narrated: don't fight the player
        if (elapsedAtShow >= spreadEndSec) { hbReason += " (already narrated: no turn)"; return; }

        if (elapsedSec >= spreadEndSec + fTurnDelay) {
            turnedThisSpread = true;
            turnSentAt = elapsedSec;
            Log(fmt::format("Turning page at {:.2f}s / {:.1f}s (method {}, key 0x{:02X})", elapsedSec, durationSec, iTurnMethod,
                            iTurnMethod == 1 ? iTurnKey : turnKey));
            SendKey(true);
            releasePending = 2;
        }
    }

    // ---------------- start / stop with the Book Menu ----------------

    void Start() {
        if (!bEnabled) return;
        const bool workerRunning = running.load();
        walkRequested = false;
        trackedInstance = -1;
        elapsedSec = 0.0;
        durationSec = 0.0;
        shownLeftPage = -1;
        turnedThisSpread = false;
        releasePending = 0;
        loggedPathFail = false;
        loggedWordFail = false;
        probeIndex = -1;
        probeClock = 0.0;
        probeCurrent = nullptr;
        probeReleasePending = false;
        probeBack = false;
        lastTick = std::chrono::steady_clock::now();
        hbClock = 0.0;
        hbTicks = 0;
        hbMaxDt = 0.0;

        auto key = RE::ControlMap::GetSingleton()->GetMappedKey(RE::UserEvents::GetSingleton()->nextPage,
                                                               RE::INPUT_DEVICE::kKeyboard,
                                                               RE::UserEvents::INPUT_CONTEXT_ID::kBook);
        // 0xCD = Right Arrow (DirectInput code), used if "NextPage" has no keyboard mapping
        turnKey = (key == 0 || key >= 0xFF) ? 0xCD : key;
        auto pk = RE::ControlMap::GetSingleton()->GetMappedKey(RE::UserEvents::GetSingleton()->prevPage, RE::INPUT_DEVICE::kKeyboard,
                                                              RE::UserEvents::INPUT_CONTEXT_ID::kBook);
        prevKey = (pk == 0 || pk >= 0xFF) ? 0xCB : pk;  // 0xCB = Left Arrow
        Log(fmt::format("Book opened: next-page key 0x{:02X}, prev-page key 0x{:02X}, method {}", turnKey, prevKey, iTurnMethod));
        bookWords.clear();
        bookTextLen = 0;
        endCache.clear();
        auto* openedBook = RE::BookMenu::GetTargetForm();
        openedBookID = openedBook ? openedBook->GetFormID() : 0;
        LoadTimingsForBook(openedBook);
        activeResume = {};
        resumeWaitSec = 0.0;
        resumeGuardSec = 0.0;
        navTarget = -1;
        if (walk.active) {
            FindVBoSObject();
            if (openedBook && openedBook->GetFormID() == walk.book) {
                // opened the walking book yourself: same hand-over as standing still
                const int page = CurrentWalkPage();
                pendingResume = {true, walk.book, walk.instance, elapsedSec, durationSec, (page / 2) * 2, true};
                EndWalk("the book was opened again");
            } else {
                CallStopInstance(walk.instance);  // another book: VBoS starts that one, ours stops
                EndWalk("another book was opened");
            }
        }
        if (pendingResume.valid) {
            if (openedBook && openedBook->GetFormID() == pendingResume.book) {
                activeResume = pendingResume;
                Log(fmt::format("Reopened the book closed at page {}: continuing its narration from {:.1f}s", pendingResume.leftPage,
                                pendingResume.elapsed));
            } else {
                // another book: the paused narration of the old one is dropped (VBoS's restart would un-pause it)
                FindVBoSObject();
                CallStopInstance(pendingResume.instance);
                Log(fmt::format("Another book opened: dropped the paused narration of the previous book (instance {})",
                                pendingResume.instance));
            }
            pendingResume = {};
        }

        running = true;
        if (workerRunning) return;  // still ticking from walking
        if (worker.joinable()) worker.join();
        worker = std::thread([]() {
            while (running) {
                SKSE::GetTaskInterface()->AddUITask([]() { Tick(); });
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }

    void Stop(bool a_allowResume = true) {
        if (!running) return;
        if (walk.active && a_allowResume) {
            // closed by read-while-walking: the narration plays on. Detach it from VBoS so its "stop on close"
            // leaves it alone; the worker keeps ticking for the mini page.
            WriteBoolProp("bIsPlaying", false);
            WriteIntProp("iSoundPlayInstance", -1);
            if (releasePending > 0) {
                releasePending = 0;
                SendKey(false);
            }
            Log(fmt::format("Book closed for walking (narration at {:.1f}s)", elapsedSec));
            return;
        }
        running = false;
        Log(fmt::format("Book closed (narration at {:.1f}s)", elapsedSec));
        if (a_allowResume && bResumeOnReopen && !bProbe && openedBookID != 0 && trackedInstance != -1 && durationSec > 1.0 && elapsedSec < durationSec - 2.0 &&
            FindVBoSObject() && ReadBool("bIsPlaying", true) && ReadInt("iSoundPlayInstance") == trackedInstance) {
            {
                // Detach the narration from VBoS so its close handling ("stop on close") and its next restart leave
                // it alone, and pause it. Reopening this book continues it; anything else drops it.
                pendingResume = {true, openedBookID, trackedInstance, elapsedSec, durationSec, shownLeftPage};
                WriteBoolProp("bIsPlaying", false);
                WriteIntProp("iSoundPlayInstance", -1);
                CallSetPlaybackPaused(true);
                Log(fmt::format("Narration paused at {:.1f}s on page {}: reopen this book to continue", elapsedSec, shownLeftPage));
            }
        }
        if (releasePending > 0) {
            releasePending = 0;
            SendKey(false);
        }
        if (worker.joinable()) worker.join();
    }

    void OnGameLoaded() {
        EndWalk("a save was loaded");
        Stop(false);
        pendingResume = {};  // VBoS stops all playback on load; a reopened book starts from the beginning
        activeResume = {};
        vbosObject.reset();  // script objects change with each save
    }

    class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                              RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
            if (a_event && a_event->menuName == RE::BookMenu::MENU_NAME) {
                if (a_event->opening) {
                    Start();
                } else {
                    Stop();
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    MenuSink g_menuSink;

    void Install() {
        if (!bEnabled) return;
        RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuSink);
        LoadVBoSMappings();
        if (auto input = RE::BSInputDeviceManager::GetSingleton()) input->AddEventSink<RE::InputEvent*>(&g_walkInput);
        if (auto cm = RE::ControlMap::GetSingleton()) {
            const auto k = cm->GetMappedKey(RE::UserEvents::GetSingleton()->forward, RE::INPUT_DEVICE::kKeyboard,
                                            RE::UserEvents::INPUT_CONTEXT_ID::kGameplay);
            if (k != 0 && k < 0xFF) forwardKey = k;
        }
        spdlog::info("AutoTurn installed (delay {:.2f}s, method {})", fTurnDelay, iTurnMethod);
    }
}
