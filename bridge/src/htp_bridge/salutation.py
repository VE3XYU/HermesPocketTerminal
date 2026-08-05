from __future__ import annotations

import re

_WORD = re.compile(r"\S+")
_PUNCTUATION = re.compile(r"[^\w\s]", re.UNICODE)


def _normalize_word(word: str) -> str:
    return _PUNCTUATION.sub("", word).lower()


def detect(transcript: str, prefixes: list[str]) -> str | None:
    """Return the text following a leading salutation, or None if absent.

    Speech-to-text output varies in casing and punctuation ("Hey Hermes," /
    "hey hermes." / "Hey, Hermes!"), so matching is done on normalized words
    while the remainder is sliced from the original text to preserve it exactly.
    An empty return means the user said only the salutation.
    """
    if not transcript or not prefixes:
        return None

    spans = [(m.group(0), m.start()) for m in _WORD.finditer(transcript)]
    if not spans:
        return None
    words = [_normalize_word(word) for word, _ in spans]

    longest_match = 0
    for prefix in prefixes:
        prefix_words = [w for w in (_normalize_word(p) for p in prefix.split()) if w]
        if not prefix_words or len(prefix_words) > len(words):
            continue
        if words[: len(prefix_words)] == prefix_words:
            longest_match = max(longest_match, len(prefix_words))

    if longest_match == 0:
        return None
    # A remainder of detached punctuation only ("Hey Hermes .") is the
    # salutation-alone case: every trailing word normalizes to nothing.
    if longest_match >= len(spans) or not any(words[longest_match:]):
        return ""
    return transcript[spans[longest_match][1] :].strip()
