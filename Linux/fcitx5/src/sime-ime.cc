// Sime Fcitx5 Engine

#include "sime-ime.h"
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fcitx-utils/key.h>
#include <fcitx-utils/standardpaths.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/statusarea.h>
#include <fcitx/text.h>
#include <fcitx/userinterface.h>
#include <fcitx/userinterfacemanager.h>


namespace fcitx {

namespace {
constexpr int kPageSize = 9;
constexpr const char *kUserSentencePath = "sime/sentences.txt";

// True when the word is ASCII letters/digits (an English command/word such
// as "ps"/"git"), so prediction should stay in English and surface options
// like "aux" instead of Chinese punctuation.
bool isEnglishWord(const std::string &word) {
    bool hasLetter = false;
    for (unsigned char c : word) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            hasLetter = true;
        } else if (!((c >= '0' && c <= '9') || c == '-' || c == '_' ||
                     c == '.')) {
            return false;
        }
    }
    return hasLetter;
}

const KeyList &selectionKeys() {
    static const KeyList keys = Key::keyListFromString("1 2 3 4 5 6 7 8 9");
    return keys;
}

std::filesystem::path userSentencePath() {
    // PkgData = $XDG_DATA_HOME/fcitx5 (e.g. ~/.local/share/fcitx5).
    // `Data` (without Pkg) is the bare XDG_DATA_HOME, which would put
    // our state outside fcitx5's own dir.
    auto dir = StandardPaths::global().userDirectory(StandardPathsType::PkgData);
    if (!dir.empty()) {
        return dir / kUserSentencePath;
    }
    if (const char *home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".local/share/fcitx5" /
               kUserSentencePath;
    }
    return kUserSentencePath;
}

std::vector<sime::TokenID> decodeContext(const SimeState *st) {
    std::vector<sime::TokenID> context = st->context_ids;
    for (const auto &sel : st->selections) {
        context.insert(context.end(), sel.tokens.begin(), sel.tokens.end());
    }
    return context;
}

std::string punctuationOutput(uint32_t chr, SimeState *st, bool english) {
    if (english) {
        // English-punctuation mode: emit the ASCII char unchanged for every
        // symbol we would otherwise convert to a full-width Chinese form.
        switch (chr) {
        case ',': case '.': case '?': case '!': case ';': case ':':
        case '\\': case '(': case ')': case '<': case '>': case '[':
        case ']': case '{': case '}': case '$': case '^': case '_':
        case '@': case '#': case '&': case '~':
            return std::string(1, static_cast<char>(chr));
        case '"': return "\"";
        case '\'': return "'";
        default: return {};
        }
    }
    switch (chr) {
    case ',': return "，";
    case '.': return "。";
    case '?': return "？";
    case '!': return "！";
    case ';': return "；";
    case ':': return "：";
    case '\\': return "、";
    case '(': return "（";
    case ')': return "）";
    case '<': return "《";
    case '>': return "》";
    case '[': return "【";
    case ']': return "】";
    case '{': return "「";
    case '}': return "」";
    case '$': return "￥";
    case '^': return "……";
    case '_': return "——";
    case '@': return "＠";
    case '#': return "＃";
    case '&': return "＆";
    case '~': return "～";
    case '"': {
        const auto result = st->doubleQuoteOpen ? "“" : "”";
        st->doubleQuoteOpen = !st->doubleQuoteOpen;
        return result;
    }
    case '\'': {
        const auto result = st->singleQuoteOpen ? "‘" : "’";
        st->singleQuoteOpen = !st->singleQuoteOpen;
        return result;
    }
    default: return {};
    }
}

// Printable ASCII for the key in English mode, with Shift applied. Letters
// are uppercased by Key::normalize() (isUAZ). For digits and punctuation the
// common US-layout Shift combinations are mapped explicitly, since
// keySymToUnicode ignores the Shift state. Returns 0 when the key produces
// no printable ASCII (modifier combos, cursor keys, ...).
uint32_t englishModeChar(const Key &key) {
    // Ctrl/Alt/Super combos are commands (e.g. Ctrl+A), not characters —
    // let them fall through to normal handling.
    if (key.states().test(KeyState::Ctrl) ||
        key.states().test(KeyState::Alt) ||
        key.states().test(KeyState::Super)) {
        return 0;
    }
    if (key.isLAZ() || key.isUAZ()) {
        return static_cast<uint32_t>(key.sym());
    }
    if (key.hasModifier()) {
        // Only a bare Shift remaps digits/punctuation (Ctrl/Alt/Super
        // already excluded above).
        if (key.states().test(KeyState::Shift)) {
            switch (key.sym()) {
            case FcitxKey_1: return '!';
            case FcitxKey_2: return '@';
            case FcitxKey_3: return '#';
            case FcitxKey_4: return '$';
            case FcitxKey_5: return '%';
            case FcitxKey_6: return '^';
            case FcitxKey_7: return '&';
            case FcitxKey_8: return '*';
            case FcitxKey_9: return '(';
            case FcitxKey_0: return ')';
            case FcitxKey_minus: return '_';
            case FcitxKey_equal: return '+';
            case FcitxKey_bracketleft: return '{';
            case FcitxKey_bracketright: return '}';
            case FcitxKey_semicolon: return ':';
            case FcitxKey_apostrophe: return '"';
            case FcitxKey_comma: return '<';
            case FcitxKey_period: return '>';
            case FcitxKey_slash: return '?';
            case FcitxKey_backslash: return '|';
            case FcitxKey_grave: return '~';
            default: return 0;
            }
        }
        return 0;
    }
    auto chr = Key::keySymToUnicode(key.sym());
    if (chr >= 0x20 && chr <= 0x7E) {
        return chr;
    }
    return 0;
}
}  // namespace

// ===== Candidate word =====

class SimeCandidateWord : public CandidateWord {
public:
    SimeCandidateWord(Sime *engine, std::string text,
                      std::string pinyin,
                      std::vector<sime::TokenID> tokens,
                      std::size_t matchedLen)
        : CandidateWord(Text(text)), engine_(engine),
          text_(std::move(text)), pinyin_(std::move(pinyin)),
          tokens_(std::move(tokens)), matchedLen_(matchedLen) {}

    void select(InputContext *ic) const override {
        engine_->selectCandidate(ic, text_, pinyin_, tokens_, matchedLen_);
    }

private:
    Sime *engine_;
    std::string text_;
    std::string pinyin_;
    std::vector<sime::TokenID> tokens_;
    std::size_t matchedLen_;
};

// ===== Prediction candidate word =====

class SimeNextCandidateWord : public CandidateWord {
public:
    SimeNextCandidateWord(Sime *engine, std::string text,
                          std::vector<sime::TokenID> tokens)
        : CandidateWord(Text(text)), engine_(engine),
          text_(std::move(text)), tokens_(std::move(tokens)) {}

    void select(InputContext *ic) const override {
        auto *st = ic->propertyFor(engine_->stateFactory());
        int maxCtx = engine_->contextSize();
        // English command/word predictions need a separating space so
        // "ps" + "aux" commits as "ps aux", not "psaux". A dash flag also
        // gets a leading space ("ls" + "-" -> "ls -"), while the flag word
        // after the dash attaches tightly ("-" + "la" -> "-la"). Chinese
        // word predictions and punctuation attach without a space.
        const bool needSpace =
            !text_.empty() && text_.front() != ' ' &&
            !st->context.empty() && isEnglishWord(st->context.back()) &&
            (isEnglishWord(text_) || text_.front() == '-');
        engine_->learnUserSentence(ic, tokens_);
        st->pushContext(text_, tokens_, maxCtx);
        ic->commitString(needSpace ? " " + text_ : text_);
        engine_->showPredictions(ic);
    }

private:
    Sime *engine_;
    std::string text_;
    std::vector<sime::TokenID> tokens_;
};

// ===== Engine =====

Sime::Sime(Instance *instance)
    : instance_(instance),
      factory_([](InputContext &) { return new SimeState(); }) {
    instance_->inputContextManager().registerProperty("simeState", &factory_);
    reloadConfig();
    initSime();
}

Sime::~Sime() { flushUserSentence(); }

void Sime::reloadConfig() {
    readAsIni(config_, "conf/sime.conf");
    applyConfig();
}

void Sime::applyConfig() {
    english_punc_ = *config_.englishPunctuation;
    if (sime_) {
        sime_->SetUserSentenceEnabled(*config_.userSentence);
        // Integer-percent options → engine doubles (IntConstrain bounds range).
        sime_->SetGruScales(*config_.gruPinyinScale / 100.0,
                            *config_.gruT9Scale / 100.0);
        sime_->SetAdaptiveGruEnabled(*config_.adaptiveGru);
        sime_->SetCorrectionEnabled(*config_.enableFuzzy, *config_.enableTypo);
        sime_->SetT9CorrectionEnabled(*config_.t9Correction);
        sime_->SetEnglishWhitelistEnabled(*config_.englishWhitelist);
        // SetWeight clamps to [0,1]; SetDecayHalfLife rebuilds decay (0 disables).
        sime_->MutableUserSentence().SetWeight(*config_.userWeight / 100.0);
        sime_->MutableUserSentence().SetDecayHalfLife(*config_.decayHalfLifeDays);
    }
}

void Sime::initSime() {
    static constexpr const char *kDictPath = "/usr/share/sime/sime.dict";
    static constexpr const char *kLmPath = "/usr/share/sime/sime.cnt";
    sime_ = std::make_unique<sime::Sime>(kDictPath, kLmPath);
    if (!sime_->Ready()) {
        FCITX_ERROR() << "Sime: failed to load resources"
                      << " dict=" << kDictPath
                      << " lm=" << kLmPath;
        sime_.reset();
    } else {
        sime_->LoadUserSentence(userSentencePath());
        applyConfig();
        FCITX_INFO() << "Sime: resources loaded, GRU="
                     << (sime_->GruReady() ? "ready" : "disabled");
    }
}

SimeState *Sime::state(InputContext *ic) {
    return ic->propertyFor(&factory_);
}

void Sime::activate(const InputMethodEntry &, InputContextEvent &event) {
    if (!sime_) initSime();

    // Load optional addon modules
    fullwidth();
    chttrans();

    // Add status area actions supplied by optional addons.
    auto *ic = event.inputContext();
    for (const auto *actionName : {"chttrans", "fullwidth"}) {
        if (auto *action =
                instance_->userInterfaceManager().lookupAction(actionName)) {
            ic->statusArea().addAction(StatusGroup::InputMethod, action);
        }
    }
}

void Sime::deactivate(const InputMethodEntry &entry, InputContextEvent &event) {
    auto *ic = event.inputContext();
    auto *st = state(ic);

    if (event.type() == EventType::InputContextSwitchInputMethod &&
        !st->empty()) {
        switch (*config_.switchInputMethodBehavior) {
        case SwitchInputMethodBehavior::CommitPreedit:
            ic->commitString(st->committedText() + st->remaining());
            break;
        case SwitchInputMethodBehavior::CommitDefault:
            ic->commitString(commitText(ic));
            break;
        case SwitchInputMethodBehavior::Clear:
            break;
        }
    }

    reset(entry, event);
    st->clearContext();
    st->resetPuncState();
    flushUserSentence();
}

void Sime::reset(const InputMethodEntry &, InputContextEvent &event) {
    resetState(event.inputContext());
}

void Sime::resetState(InputContext *ic) {
    state(ic)->reset();
    ic->inputPanel().reset();
    ic->updatePreedit();
    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
}

// Build the default commit string: selected text + first candidate for remaining
std::string Sime::commitText(InputContext *ic) const {
    auto *st = ic->propertyFor(const_cast<FactoryFor<SimeState>*>(&factory_));
    std::string result = st->committedText();
    if (sime_) {
        std::string rem = st->remaining();
        if (!rem.empty()) {
            auto results = *config_.userSentence
                               ? sime_->DecodeSentence(rem, decodeContext(st), 0)
                               : sime_->DecodeSentence(rem, 0);
            if (!results.empty())
                result += results[0].text;
            else
                result += rem;
        }
    } else {
        result += st->remaining();
    }
    return result;
}

// Called when user selects a candidate
void Sime::selectCandidate(InputContext *ic, const std::string& text,
                                  const std::string& pinyin,
                                  const std::vector<sime::TokenID>& tokens,
                                  std::size_t matchedLen) {
    auto *st = state(ic);

    // Record selection
    st->select(text, pinyin, tokens, matchedLen);

    // If all input consumed, commit everything
    if (st->fullySelected()) {
        // Learn each selection in turn so the engine sees the right
        // context at each step (and the file format gets one row per
        // selection for easy inspection). pushContext between calls so
        // each successive learn sees the prior selections in context.
        for (const auto& sel : st->selections) {
            learnUserSentence(ic, sel.tokens);
            st->pushContext(sel.text, sel.tokens, contextSize());
        }
        ic->commitString(st->committedText());
        st->reset();
        showPredictions(ic);
    } else {
        // Move cursor to end of remaining
        st->cursor = st->buffer.size();
        updateUI(ic);
    }
}

void Sime::learnUserSentence(InputContext *ic,
                             const std::vector<sime::TokenID> &tokens) {
    if (!sime_ || !*config_.userSentence || tokens.empty()) {
        return;
    }
    auto *st = state(ic);
    std::vector<sime::TokenID> context = st->context_ids;
    auto maxContext = static_cast<std::size_t>(contextSize());
    if (context.size() > maxContext) {
        context.erase(context.begin(),
                      context.begin() +
                          static_cast<std::ptrdiff_t>(context.size() -
                                                      maxContext));
    }
    sime_->LearnUserSentence(context, tokens);
    ++pending_user_sentence_saves_;
    if (pending_user_sentence_saves_ >= kUserSentenceFlushThreshold) {
        flushUserSentence();
    }
}

void Sime::flushUserSentence() {
    if (pending_user_sentence_saves_ == 0 || !sime_) {
        return;
    }
    if (!sime_->SaveUserSentence(userSentencePath())) {
        FCITX_WARN() << "Sime: failed to save user sentence history";
    }
    pending_user_sentence_saves_ = 0;
}

void Sime::toggleEnglishMode(InputContext *ic, SimeState *st) {
    // Commit any composing buffer (selected + top decode of remaining),
    // matching the macOS frontend's commitOrClear, so no half-composed
    // pinyin is stranded when the mode flips.
    if (!st->empty()) {
        ic->commitString(commitText(ic));
        st->reset();
    }
    st->predicting = false;
    englishMode_ = !englishMode_;
    auto &panel = ic->inputPanel();
    panel.reset();
    ic->updatePreedit();
    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
    // Refresh the tray / im-switcher icon + label for the new mode.
    ic->updateUserInterface(UserInterfaceComponent::StatusArea);
}

std::string Sime::subModeIconImpl(const InputMethodEntry &entry,
                                  InputContext &ic) {
    FCITX_UNUSED(entry);
    FCITX_UNUSED(ic);
    // English mode shows the "A" icon; Chinese mode falls back to the
    // default "sime" icon from the input method config.
    return englishMode_ ? "sime-en" : std::string();
}

std::string Sime::subModeLabelImpl(const InputMethodEntry &entry,
                                   InputContext &ic) {
    FCITX_UNUSED(entry);
    FCITX_UNUSED(ic);
    return englishMode_ ? "A" : std::string();
}

void Sime::showPredictions(InputContext *ic) {
    auto *st = state(ic);
    auto &panel = ic->inputPanel();
    panel.reset();

    if (!sime_ || st->context.empty() || !*config_.prediction) {
        st->predicting = false;
        ic->updatePreedit();
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
        return;
    }

    std::vector<sime::DecodeResult> results;
    if (isEnglishWord(st->context.back())) {
        // English command: surface bare options/subcommands (aux, clone, ...)
        // plus dash flags combined into "-la"/"-ef" so they show directly
        // after the command instead of a bare "-" needing a second pick.
        results = sime_->NextTokens(st->context_ids,
            static_cast<std::size_t>(kPageSize), true);
        auto mixed = sime_->NextTokens(st->context_ids,
            static_cast<std::size_t>(kPageSize) * 2, false);
        sime::TokenID dash = 0;
        for (const auto &r : mixed) {
            if (r.text == "-" && !r.tokens.empty()) {
                dash = r.tokens.front();
                break;
            }
        }
        if (dash != 0) {
            auto flagCtx = st->context_ids;
            flagCtx.push_back(dash);
            auto flags = sime_->NextTokens(flagCtx,
                static_cast<std::size_t>(kPageSize), false);
            for (const auto &r : flags) {
                if (!isEnglishWord(r.text)) continue;
                sime::DecodeResult c;
                c.text = "-" + r.text;
                c.tokens = r.tokens;
                c.tokens.insert(c.tokens.begin(), dash);
                results.push_back(std::move(c));
            }
        }
        if (results.size() > static_cast<std::size_t>(kPageSize)) {
            results.resize(static_cast<std::size_t>(kPageSize));
        }
    } else {
        results = sime_->NextTokens(st->context_ids,
            static_cast<std::size_t>(kPageSize), false);
    }

    if (results.empty()) {
        st->predicting = false;
        ic->updatePreedit();
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
        return;
    }

    st->predicting = true;

    auto cl = std::make_unique<CommonCandidateList>();
    cl->setPageSize(kPageSize);
    cl->setLayoutHint(CandidateLayoutHint::Horizontal);
    cl->setSelectionKey(selectionKeys());
    cl->setCursorPositionAfterPaging(CursorPositionAfterPaging::ResetToFirst);

    for (const auto& r : results) {
        cl->append<SimeNextCandidateWord>(this, r.text, r.tokens);
    }
    cl->setCursorIndex(0);
    panel.setCandidateList(std::move(cl));

    panel.setClientPreedit(Text{});
    panel.setPreedit(Text{});
    ic->updatePreedit();
    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
}

void Sime::updateUI(InputContext *ic) {
    auto *st = state(ic);
    auto &panel = ic->inputPanel();
    panel.reset();

    if (st->empty() || !sime_) {
        ic->updatePreedit();
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
        return;
    }

    // Decode only the remaining (unselected) input
    std::string rem = st->remaining();
    std::vector<sime::DecodeResult> results;
    if (!rem.empty()) {
        if (*config_.userSentence) {
            auto context = decodeContext(st);
            results = sime_->DecodeSentence(
                rem, context, static_cast<std::size_t>(*config_.nbest));
        } else {
            results = sime_->DecodeSentence(
                rem, static_cast<std::size_t>(*config_.nbest));
        }
    }

    // Build preedit based on mode
    std::string committed = st->committedText();

    // Build remaining_display from rem with spaces at syllable boundaries.
    // Use results[0].units to identify boundaries, then map back to rem.
    std::string remaining_display;
    if (!results.empty() && !results[0].units.empty()) {
        // units is e.g. "bu'zhi'dao", matching first results[0].cnt bytes of rem
        // Insert spaces at apostrophe positions in the original rem
        const auto &units = results[0].units;
        std::size_t ri = 0;  // index into rem
        std::size_t ui = 0;  // index into units
        while (ri < results[0].cnt && ui < units.size()) {
            if (units[ui] == '\'') {
                remaining_display += ' ';
                ++ui;
            } else {
                remaining_display += rem[ri];
                ++ri;
                ++ui;
            }
        }
        // Append unmatched tail (e.g. incomplete syllable 'd' in 'buzhid')
        if (ri < rem.size()) {
            remaining_display += ' ';
            remaining_display += rem.substr(ri);
        }
    } else {
        remaining_display = rem;
    }

    bool hasPreeditCap = ic->capabilityFlags().test(CapabilityFlag::Preedit);
    PreeditMode mode = hasPreeditCap ? *config_.preeditMode : PreeditMode::No;

    // Compute display cursor position (maps raw buffer cursor to display)
    std::size_t sel_len = st->selectedLength();
    int displayCursor;
    if (st->cursor < sel_len) {
        displayCursor = static_cast<int>(committed.size());
    } else if (st->cursor >= st->buffer.size()) {
        displayCursor = static_cast<int>(committed.size() + remaining_display.size());
    } else {
        // Map raw cursor offset to display offset, skipping inserted spaces
        std::size_t raw_offset = st->cursor - sel_len;
        std::size_t disp = 0, raw = 0;
        while (raw < raw_offset && disp < remaining_display.size()) {
            if (remaining_display[disp] == ' ' && (raw >= rem.size() || rem[raw] != ' ')) {
                ++disp;
            } else {
                ++raw;
                ++disp;
            }
        }
        // Skip any trailing spaces at the cursor position
        while (disp < remaining_display.size() && remaining_display[disp] == ' ' &&
               (raw >= rem.size() || rem[raw] != ' '))
            ++disp;
        displayCursor = static_cast<int>(committed.size() + disp);
    }

    // Build composing preedit: [committed](highlight) + [remaining pinyin](underline)
    auto buildComposingPreedit = [&]() {
        Text t;
        if (!committed.empty())
            t.append(committed, TextFormatFlags{TextFormatFlag::HighLight});
        if (!remaining_display.empty())
            t.append(remaining_display, TextFormatFlags{TextFormatFlag::Underline});
        t.setCursor(displayCursor);
        return t;
    };

    Text clientPreedit;
    switch (mode) {
    case PreeditMode::ComposingPinyin:
        clientPreedit = buildComposingPreedit();
        panel.setClientPreedit(clientPreedit);
        panel.setPreedit(Text{});
        break;
    case PreeditMode::CommitPreview: {
        std::string preview = committed;
        if (!results.empty()) preview += results[0].text;
        clientPreedit.append(preview, TextFormatFlags{TextFormatFlag::Underline});
        clientPreedit.setCursor(static_cast<int>(committed.size()));
        panel.setClientPreedit(clientPreedit);
        panel.setPreedit(buildComposingPreedit());
        break;
    }
    case PreeditMode::No:
        panel.setClientPreedit(Text{});
        panel.setPreedit(buildComposingPreedit());
        break;
    }

    // Build candidate list from remaining input
    auto cl = std::make_unique<CommonCandidateList>();
    cl->setPageSize(kPageSize);
    cl->setLayoutHint(CandidateLayoutHint::Horizontal);
    cl->setSelectionKey(selectionKeys());
    cl->setCursorPositionAfterPaging(CursorPositionAfterPaging::ResetToFirst);

    for (const auto &r : results) {
        cl->append<SimeCandidateWord>(this, r.text, r.units, r.tokens, r.cnt);
    }

    if (!results.empty()) {
        cl->setCursorIndex(0);
    }
    panel.setCandidateList(std::move(cl));
    ic->updatePreedit();
    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
}

void Sime::keyEvent(const InputMethodEntry &, KeyEvent &event) {
    auto *ic = event.inputContext();
    auto *st = state(ic);
    auto key = event.key();

    // Shift-alone tap (press + release with no other key in between)
    // toggles English mode, matching the macOS frontend. When the input
    // method group has another IM (e.g. a keyboard layout), the fcitx5
    // core's AltTriggerKeys (default Shift_L) intercepts bare Shift and
    // switches input methods instead — it filters the key, so we never
    // see it and there is no conflict. Shift_R is always free for the
    // in-engine toggle.
    if (key.isModifier() &&
        (key.check(FcitxKey_Shift_L) || key.check(FcitxKey_Shift_R))) {
        if (event.isRelease()) {
            if (st->shiftDown && !st->keyAfterShift) {
                toggleEnglishMode(ic, st);
            }
            st->shiftDown = false;
            st->keyAfterShift = false;
        } else {
            st->shiftDown = true;
            st->keyAfterShift = false;
        }
        event.filterAndAccept();
        return;
    }

    if (event.isRelease()) return;

    // A real key was pressed during the shift press — disqualify the tap.
    if (st->shiftDown) {
        st->keyAfterShift = true;
    }

    // English mode: printable ASCII commits directly, no pinyin decode
    // (letters keep case; digits/punctuation use the US Shift mapping).
    // Non-ASCII keys fall through to normal handling.
    if (englishMode_) {
        auto chr = englishModeChar(key);
        if (chr) {
            // Flush any composing buffer first (defensive; the buffer is
            // already committed when entering English mode).
            if (!st->empty()) {
                ic->commitString(commitText(ic));
                st->reset();
            }
            ic->commitString(std::string(1, static_cast<char>(chr)));
            event.filterAndAccept();
            return;
        }
    }

    // Chinese/English punctuation toggle hotkey (Ctrl+. by default).
    if (key.checkKeyList(*config_.togglePunctuation)) {
        english_punc_ = !english_punc_;
        event.filterAndAccept();
        return;
    }

    // Save and clear punctuation undo state
    bool lastIsPunc = st->lastIsPunc;
    std::string lastPuncStr = st->lastPuncStr;
    st->lastIsPunc = false;
    st->lastPuncStr.clear();

    auto &panel = ic->inputPanel();
    auto *cl = panel.candidateList().get();

    // Prediction mode: handle selection or exit
    if (st->predicting && cl) {
        // Selection keys: select prediction candidate
        auto idx = key.keyListIndex(selectionKeys());
        if (idx >= 0 && idx < cl->size()) {
            cl->candidate(idx).select(ic);
            event.filterAndAccept();
            return;
        }
        // Space: select current prediction
        if (key.checkKeyList(*config_.currentCandidate) && cl->size() > 0) {
            int cidx = cl->cursorIndex();
            if (cidx < 0 || cidx >= cl->size()) cidx = 0;
            cl->candidate(cidx).select(ic);
            event.filterAndAccept();
            return;
        }
        // Page navigation in prediction mode
        auto *pageable = cl->toPageable();
        if (pageable) {
            if (key.checkKeyList(*config_.nextPage) && pageable->hasNext()) {
                pageable->next();
                ic->updateUserInterface(UserInterfaceComponent::InputPanel);
                event.filterAndAccept();
                return;
            }
            if (key.checkKeyList(*config_.prevPage) && pageable->hasPrev()) {
                pageable->prev();
                ic->updateUserInterface(UserInterfaceComponent::InputPanel);
                event.filterAndAccept();
                return;
            }
        }
        // Escape: dismiss predictions
        if (key.check(FcitxKey_Escape)) {
            st->predicting = false;
            panel.reset();
            ic->updatePreedit();
            ic->updateUserInterface(UserInterfaceComponent::InputPanel);
            event.filterAndAccept();
            return;
        }
        // Any other key: exit prediction, fall through to normal handling
        st->predicting = false;
        panel.reset();
        ic->updatePreedit();
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
        cl = nullptr;
    }

    // Selection keys (1-9 by default): select candidate
    if (!st->empty() && cl) {
        auto idx = key.keyListIndex(selectionKeys());
        if (idx >= 0 && idx < cl->size()) {
            cl->candidate(idx).select(ic);
            event.filterAndAccept();
            return;
        }
    }

    // Current candidate (Space by default): select highlighted
    if (!st->empty() && key.checkKeyList(*config_.currentCandidate)) {
        if (cl && cl->size() > 0) {
            int idx = cl->cursorIndex();
            if (idx < 0 || idx >= cl->size()) idx = 0;
            cl->candidate(idx).select(ic);
        }
        event.filterAndAccept();
        return;
    }

    // Commit raw input (Enter by default)
    // Like fcitx5-pinyin: commit selected hanzi + remaining raw pinyin
    if (!st->empty() && key.checkKeyList(*config_.commitRawInput)) {
        ic->commitString(st->committedText() + st->remaining());
        st->clearContext();
        resetState(ic);
        event.filterAndAccept();
        return;
    }

    // Escape: cancel
    if (key.check(FcitxKey_Escape) && !st->empty()) {
        resetState(ic);
        event.filterAndAccept();
        return;
    }

    // Backspace
    if (key.check(FcitxKey_BackSpace)) {
        // Punctuation undo: buffer empty, last action was punctuation
        if (st->empty() && lastIsPunc && !lastPuncStr.empty()) {
            auto charCount = utf8::lengthValidated(lastPuncStr);
            if (charCount != utf8::INVALID_LENGTH && charCount > 1) {
                for (size_t i = 1; i < charCount; i++) {
                    ic->forwardKey(Key(FcitxKey_BackSpace));
                }
            }
            // Let the original backspace pass through to delete the last char
            return;
        }

        if (!st->empty()) {
            // Like fcitx5-pinyin: if cursor at selection boundary, cancel selection
            if (!st->selections.empty() && st->cursor == st->selectedLength()) {
                st->cancel();
                st->cursor = st->buffer.size();
            } else if (st->cursor > st->selectedLength()) {
                st->buffer.erase(st->cursor - 1, 1);
                --st->cursor;
            }
            if (st->empty())
                resetState(ic);
            else
                updateUI(ic);
            event.filterAndAccept();
        }
        return;
    }

    // Delete: delete character after cursor (only in remaining part)
    if (key.check(FcitxKey_Delete)) {
        if (!st->empty() && st->cursor >= st->selectedLength() &&
            st->cursor < st->buffer.size()) {
            st->buffer.erase(st->cursor, 1);
            if (st->empty())
                resetState(ic);
            else
                updateUI(ic);
            event.filterAndAccept();
        }
        return;
    }

    // Next/prev candidate (Tab/Shift+Tab by default)
    if (!st->empty() && cl) {
        int delta = 0;
        if (key.checkKeyList(*config_.nextCandidate)) delta = 1;
        else if (key.checkKeyList(*config_.prevCandidate)) delta = -1;
        if (delta) {
            auto *ccl = dynamic_cast<CommonCandidateList *>(cl);
            if (ccl) {
                int next = ccl->cursorIndex() + delta;
                if (next >= 0 && next < ccl->size())
                    ccl->setCursorIndex(next);
            }
            ic->updateUserInterface(UserInterfaceComponent::InputPanel);
            event.filterAndAccept();
            return;
        }
    }

    // Page navigation
    if (!st->empty() && cl) {
        auto *pageable = cl->toPageable();
        if (pageable) {
            bool handled = false;
            if (key.checkKeyList(*config_.nextPage)) {
                if (pageable->hasNext()) pageable->next();
                handled = true;
            } else if (key.checkKeyList(*config_.prevPage)) {
                if (pageable->hasPrev()) pageable->prev();
                handled = true;
            }
            if (handled) {
                ic->updateUserInterface(UserInterfaceComponent::InputPanel);
                event.filterAndAccept();
                return;
            }
        }
    }

    // Left/Right: move cursor (like fcitx5-pinyin)
    if (!st->empty() && (key.check(FcitxKey_Left) || key.check(FcitxKey_Right))) {
        if (key.check(FcitxKey_Left)) {
            // If cursor is at selection boundary, cancel last selection
            // and stay in place (don't move further left).
            if (st->cursor == st->selectedLength() && !st->selections.empty()) {
                st->cancel();
            } else if (st->cursor > st->selectedLength()) {
                --st->cursor;
            }
        } else {
            if (st->cursor < st->buffer.size())
                ++st->cursor;
        }
        updateUI(ic);
        event.filterAndAccept();
        return;
    }

    // Home/End (like fcitx5-pinyin: Home goes to selection boundary, not 0)
    if (!st->empty() && (key.check(FcitxKey_Home) || key.check(FcitxKey_End))) {
        st->cursor = key.check(FcitxKey_Home) ? st->selectedLength() : st->buffer.size();
        updateUI(ic);
        event.filterAndAccept();
        return;
    }

    // a-z / Shift+A-Z: append to buffer (preserving case)
    // Apostrophe: separator (only when composing)
    {
        char ch = 0;
        if (key.isLAZ())
            ch = static_cast<char>(key.sym());
        else if (key.isUAZ())
            ch = static_cast<char>(key.sym());
        else if (key.check(FcitxKey_apostrophe) && !st->empty())
            ch = '\'';
        if (ch) {
            st->buffer.insert(st->cursor, 1, ch);
            ++st->cursor;
            updateUI(ic);
            event.filterAndAccept();
            return;
        }
    }

    // Chinese punctuation.
    if (!key.hasModifier() && !key.isKeyPad()) {
        auto chr = Key::keySymToUnicode(key.sym());
        if (auto punc = punctuationOutput(chr, st, english_punc_); !punc.empty()) {
            // Auto-select first candidate if composing
            if (!st->empty() && cl && cl->size() > 0) {
                cl->candidate(0).select(ic);
            }

            ic->commitString(punc);

            st->lastIsPunc = true;
            st->lastPuncStr = punc;

            // Sentence-ending punctuation clears context
            if (punc == "。" || punc == "？" || punc == "！") {
                st->clearContext();
            }
            event.filterAndAccept();
            return;
        }
    }
}

} // namespace fcitx

FCITX_ADDON_FACTORY(fcitx::SimeAddonFactory)
