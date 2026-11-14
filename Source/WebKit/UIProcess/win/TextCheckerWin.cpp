/*
 * Copyright (C) 2026 Lyra contributors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// Lyra: spell checking on Windows through the Windows Spell Checking API (spellcheck.h, the
// dictionaries Windows itself uses). Every language Windows has a dictionary for among the user's
// preferred languages, English and Russian is loaded; a word is misspelled when no loaded
// dictionary accepts it. WebCore asks word by word (no unified text checking on Windows):
// checkSpellingOfString, getGuessesForWord, learnWord, ignoreWord.

#include "config.h"
#include "TextChecker.h"

#if PLATFORM(WIN)

#include "TextCheckerState.h"
#include "WebProcessPool.h"
#include <objbase.h>
#include <spellcheck.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

namespace WebKit {
using namespace WebCore;

namespace {

template<typename T> class ComRef {
public:
    ComRef() = default;
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    ComRef(ComRef&& o) : m_ptr(std::exchange(o.m_ptr, nullptr)) { }
    ~ComRef() { if (m_ptr) m_ptr->Release(); }
    T* get() const { return m_ptr; }
    T* operator->() const { return m_ptr; }
    T** out() { if (m_ptr) { m_ptr->Release(); m_ptr = nullptr; } return &m_ptr; }
    explicit operator bool() const { return m_ptr; }
private:
    T* m_ptr { nullptr };
};

struct Dictionary {
    String language;
    ComRef<ISpellChecker> checker;
    bool cyrillic { false };            // a dictionary for a language written in Cyrillic
};

class SpellCheckers {
public:
    static SpellCheckers& singleton()
    {
        static NeverDestroyed<SpellCheckers> instance;
        return instance;
    }

    bool available() { ensure(); return !m_dictionaries.isEmpty(); }

    // Every misspelled word of `text`, in text order. A word is judged by the dictionaries of its
    // script only (Cyrillic words by Russian, Latin ones by English...): it is misspelled when each
    // of them flags it. A dictionary of another script marks words it cannot read in its own way
    // (whole runs, or not at all), so it has no say.
    Vector<std::pair<unsigned, unsigned>> misspellings(StringView text)
    {
        ensure();
        if (m_dictionaries.isEmpty() || text.isEmpty())
            return { };
        String copy = text.toString();
        Vector<wchar_t> buffer = toWide(copy);
        Vector<Vector<std::pair<unsigned, unsigned>>> errors;
        for (auto& dictionary : m_dictionaries)
            errors.append(errorsOf(dictionary, buffer));
        auto overlaps = [](const std::pair<unsigned, unsigned>& a, const std::pair<unsigned, unsigned>& b) {
            return a.first < b.first + b.second && b.first < a.first + a.second;
        };
        Vector<std::pair<unsigned, unsigned>> result;
        for (size_t d = 0; d < m_dictionaries.size(); ++d) {
            for (auto& range : errors[d]) {
                if (range.first + range.second > copy.length())
                    continue;
                if (hasCyrillic(copy.substring(range.first, range.second)) != m_dictionaries[d].cyrillic)
                    continue;               // not this dictionary's script
                bool everyone = true;
                for (size_t other = 0; other < m_dictionaries.size() && everyone; ++other) {
                    if (other == d || m_dictionaries[other].cyrillic != m_dictionaries[d].cyrillic)
                        continue;
                    everyone = errors[other].containsIf([&](const auto& e) { return overlaps(e, range); });
                }
                if (everyone && !result.containsIf([&](const auto& r) { return overlaps(r, range); }))
                    result.append(range);
            }
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    // The first misspelled word of `text`: location / length, or -1 / 0.
    void check(StringView text, int32_t& location, int32_t& length)
    {
        location = -1;
        length = 0;
        auto all = misspellings(text);
        if (all.isEmpty())
            return;
        location = all[0].first;
        length = all[0].second;
    }

    Vector<String> suggestions(const String& word)
    {
        Vector<String> result;
        ensure();
        Vector<wchar_t> buffer = toWide(word);
        for (auto* dictionary : orderFor(word)) {
            ComRef<IEnumString> list;
            if (FAILED(dictionary->checker->Suggest(buffer.span().data(), list.out())) || !list)
                continue;
            LPOLESTR item = nullptr;
            while (list->Next(1, &item, nullptr) == S_OK) {
                String suggestion = fromWide(item);
                CoTaskMemFree(item);
                if (!result.contains(suggestion))
                    result.append(WTF::move(suggestion));
                if (result.size() >= 8)
                    return result;
            }
            if (!result.isEmpty())
                break;                  // the dictionary of the word's script had ideas
        }
        return result;
    }

    void learn(const String& word)
    {
        ensure();
        Vector<wchar_t> buffer = toWide(word);
        if (auto order = orderFor(word); !order.isEmpty())
            order[0]->checker->Add(buffer.span().data());       // the user's dictionary, kept by Windows
    }

    void ignore(const String& word)
    {
        ensure();
        Vector<wchar_t> buffer = toWide(word);
        for (auto& dictionary : m_dictionaries)
            dictionary.checker->Ignore(buffer.span().data());   // until Lyra closes
    }

private:
    friend class NeverDestroyed<SpellCheckers>;
    SpellCheckers() = default;

    static Vector<wchar_t> toWide(const String& s)
    {
        Vector<wchar_t> buffer;
        buffer.reserveInitialCapacity(s.length() + 1);
        for (unsigned i = 0; i < s.length(); ++i)
            buffer.append(static_cast<wchar_t>(s[i]));
        buffer.append(0);
        return buffer;
    }

    static String fromWide(const wchar_t* p)
    {
        return String(std::span<const UChar>(reinterpret_cast<const UChar*>(p), wcslen(p)));
    }

    static bool hasCyrillic(const String& s)
    {
        for (unsigned i = 0; i < s.length(); ++i) {
            UChar c = s[i];
            if (c >= 0x0400 && c <= 0x052F)
                return true;
        }
        return false;
    }

    // The dictionaries in the order to ask for a word: those of its script first.
    Vector<Dictionary*> orderFor(const String& word)
    {
        const bool cyrillic = hasCyrillic(word);
        Vector<Dictionary*> order;
        for (auto& d : m_dictionaries)
            if (d.cyrillic == cyrillic)
                order.append(&d);
        for (auto& d : m_dictionaries)
            if (d.cyrillic != cyrillic)
                order.append(&d);
        return order;
    }

    static Vector<std::pair<unsigned, unsigned>> errorsOf(Dictionary& dictionary, const Vector<wchar_t>& text)
    {
        Vector<std::pair<unsigned, unsigned>> result;
        ComRef<IEnumSpellingError> errors;
        if (FAILED(dictionary.checker->Check(text.span().data(), errors.out())) || !errors)
            return result;
        while (true) {
            ComRef<ISpellingError> error;
            if (errors->Next(error.out()) != S_OK || !error)
                break;
            ULONG start = 0, length = 0;
            error->get_StartIndex(&start);
            error->get_Length(&length);
            if (length)
                result.append({ start, length });
        }
        return result;
    }

    void ensure()
    {
        if (m_initialized)
            return;
        m_initialized = true;
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);      // (already done by the browser normally)
        if (FAILED(CoCreateInstance(__uuidof(SpellCheckerFactory), nullptr, CLSCTX_INPROC_SERVER, __uuidof(ISpellCheckerFactory), reinterpret_cast<void**>(m_factory.out()))) || !m_factory)
            return;

        Vector<String> wanted;
        ULONG count = 0, size = 0;
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) && size) {
            Vector<wchar_t> names(size);
            if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, names.mutableSpan().data(), &size)) {
                for (const wchar_t* p = names.span().data(); *p; p += wcslen(p) + 1)
                    wanted.append(fromWide(p));
            }
        }
        wanted.append("en-US"_s);
        wanted.append("ru-RU"_s);

        for (auto& language : wanted) {
            if (m_dictionaries.containsIf([&](auto& d) { return equalIgnoringASCIICase(d.language, language); }))
                continue;
            Vector<wchar_t> name = toWide(language);
            BOOL supported = FALSE;
            if (FAILED(m_factory->IsSupported(name.span().data(), &supported)) || !supported)
                continue;
            Dictionary dictionary;
            dictionary.language = language;
            if (FAILED(m_factory->CreateSpellChecker(name.span().data(), dictionary.checker.out())) || !dictionary.checker)
                continue;
            const String lower = language.convertToASCIILowercase();
            dictionary.cyrillic = lower.startsWith("ru"_s) || lower.startsWith("uk"_s) || lower.startsWith("be"_s)
                || lower.startsWith("bg"_s) || lower.startsWith("sr"_s) || lower.startsWith("kk"_s) || lower.startsWith("mk"_s);
            m_dictionaries.append(WTF::move(dictionary));
        }
    }

    bool m_initialized { false };
    ComRef<ISpellCheckerFactory> m_factory;
    Vector<Dictionary> m_dictionaries;
};

OptionSet<TextCheckerState>& checkerState()
{
    static OptionSet<TextCheckerState> textCheckerState;
    return textCheckerState;
}

bool s_testingMode = false;

} // namespace

OptionSet<TextCheckerState> TextChecker::state()
{
    return checkerState();
}

void TextChecker::setTestingMode(bool enabled)
{
    s_testingMode = enabled;
}

bool TextChecker::isTestingMode()
{
    return s_testingMode;
}

bool TextChecker::isContinuousSpellCheckingAllowed()
{
    return SpellCheckers::singleton().available();
}

bool TextChecker::setContinuousSpellCheckingEnabled(bool enabled)
{
    if (checkerState().contains(TextCheckerState::ContinuousSpellCheckingEnabled) == enabled)
        return false;
    checkerState().set(TextCheckerState::ContinuousSpellCheckingEnabled, enabled);
    for (const auto& processPool : WebProcessPool::allProcessPools())
        processPool->textCheckerStateChanged();
    return true;
}

void TextChecker::setGrammarCheckingEnabled(bool)
{
}

void TextChecker::continuousSpellCheckingEnabledStateChanged(bool enabled)
{
    checkerState().set(TextCheckerState::ContinuousSpellCheckingEnabled, enabled);
}

void TextChecker::grammarCheckingEnabledStateChanged(bool)
{
}

SpellDocumentTag TextChecker::uniqueSpellDocumentTag(WebPageProxy*)
{
    return { };
}

void TextChecker::closeSpellDocumentWithTag(SpellDocumentTag)
{
}

void TextChecker::checkSpellingOfString(SpellDocumentTag, StringView text, int32_t& misspellingLocation, int32_t& misspellingLength)
{
    SpellCheckers::singleton().check(text, misspellingLocation, misspellingLength);
}

void TextChecker::checkGrammarOfString(SpellDocumentTag, StringView, Vector<WebCore::GrammarDetail>&, int32_t& badGrammarLocation, int32_t& badGrammarLength)
{
    badGrammarLocation = -1;
    badGrammarLength = 0;
}

bool TextChecker::spellingUIIsShowing()
{
    return false;
}

void TextChecker::toggleSpellingUIIsShowing()
{
}

void TextChecker::updateSpellingUIWithMisspelledWord(SpellDocumentTag, const String&)
{
}

void TextChecker::updateSpellingUIWithGrammarString(SpellDocumentTag, const String&, const GrammarDetail&)
{
}

void TextChecker::getGuessesForWord(SpellDocumentTag, const String& word, const String&, int32_t, Vector<String>& guesses, bool)
{
    guesses = SpellCheckers::singleton().suggestions(word);
}

void TextChecker::learnWord(SpellDocumentTag, const String& word)
{
    SpellCheckers::singleton().learn(word);
}

void TextChecker::ignoreWord(SpellDocumentTag, const String& word)
{
    SpellCheckers::singleton().ignore(word);
}

// The whole paragraph at once (pasted text, text already in a field): WebCore marks every
// misspelled word the Windows checker finds in it.
Vector<TextCheckingResult> TextChecker::checkTextOfParagraph(SpellDocumentTag, StringView text, int32_t, OptionSet<TextCheckingType> checkingTypes, bool)
{
    Vector<TextCheckingResult> results;
    if (!checkingTypes.contains(TextCheckingType::Spelling))
        return results;
    for (auto& [start, length] : SpellCheckers::singleton().misspellings(text)) {
        TextCheckingResult result;
        result.type = TextCheckingType::Spelling;
        result.range = CharacterRange(start, length);
        results.append(WTF::move(result));
    }
    return results;
}

void TextChecker::requestCheckingOfString(Ref<TextCheckerCompletion>&& completion, int32_t insertionPoint)
{
    TextCheckingRequestData request = completion->textCheckingRequestData();
    completion->didFinishCheckingText(checkTextOfParagraph(completion->spellDocumentTag(), request.text(), insertionPoint, request.checkingTypes(), false));
}

void TextChecker::requestExtendedCheckingOfString(Ref<TextCheckerCompletion>&& completion, int32_t)
{
    completion->didFinishCheckingText({ });
}

} // namespace WebKit

#endif // PLATFORM(WIN)
