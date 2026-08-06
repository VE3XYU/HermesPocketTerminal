import pytest

from htp_bridge.salutation import detect

PREFIXES = ["hey hermes", "hermes"]


@pytest.mark.parametrize(
    "transcript,expected",
    [
        ("Hey Hermes, what's on my calendar today?", "what's on my calendar today?"),
        ("hey hermes what's on my calendar", "what's on my calendar"),
        ("Hey, Hermes! What's on my calendar?", "What's on my calendar?"),
        ("HEY HERMES tell me a joke", "tell me a joke"),
        ("Hermes, remind me about dinner", "remind me about dinner"),
        ("  Hey Hermes   what time is it  ", "what time is it"),
    ],
)
def test_detects_salutation_and_returns_remainder(transcript, expected):
    assert detect(transcript, PREFIXES) == expected


@pytest.mark.parametrize(
    "transcript",
    [
        "Add milk to the shopping list",
        "Remember that Hermes is the messenger god",
        "Hermetic seals are underrated",
        "",
        "   ",
    ],
)
def test_returns_none_without_leading_salutation(transcript):
    assert detect(transcript, PREFIXES) is None


def test_salutation_alone_returns_empty_remainder():
    assert detect("Hey Hermes.", PREFIXES) == ""


@pytest.mark.parametrize("transcript", ["Hey Hermes .", "Hey Hermes ...", "Hey Hermes , !"])
def test_detached_punctuation_after_salutation_is_an_empty_remainder(transcript):
    """STT sometimes emits punctuation as its own token; a remainder with no
    word characters is the salutation-alone case, not a prompt of '.'."""
    assert detect(transcript, PREFIXES) == ""


def test_longest_matching_prefix_wins():
    """With both 'hermes' and 'hey hermes' configured, the longer must match first
    so the remainder is not left containing the word 'hermes'."""
    assert detect("Hey Hermes what's up", ["hermes", "hey hermes"]) == "what's up"


def test_custom_assistant_name():
    assert detect("Okay Athena, lights on", ["okay athena"]) == "lights on"


def test_empty_prefix_list_never_matches():
    assert detect("Hey Hermes what's up", []) is None


def test_blank_prefixes_are_ignored():
    assert detect("Hey Hermes what's up", ["", "   "]) is None
