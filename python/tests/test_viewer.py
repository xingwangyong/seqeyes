"""Tests for seqeyes.seqeyes() viewer launcher."""

import os
import pytest
from unittest.mock import patch, MagicMock


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

FAKE_EXE = "/usr/local/bin/seqeyes"


def _popen_mock():
    """Return a mock that replaces subprocess.Popen."""
    return MagicMock()


# ---------------------------------------------------------------------------
# Executable discovery
# ---------------------------------------------------------------------------

def test_seqeyes_raises_when_exe_not_found():
    """FileNotFoundError when neither bundled binary nor PATH entry exists."""
    from seqeyes.viewer import seqeyes
    with patch("shutil.which", return_value=None), \
         patch("pathlib.Path.is_file", return_value=False):
        with pytest.raises(FileNotFoundError, match="executable not found"):
            seqeyes("dummy.seq")


def test_find_executable_prefers_bundled(tmp_path):
    """_find_executable returns the bundled binary when it exists."""
    import sys
    from seqeyes import viewer
    from seqeyes.viewer import _find_executable

    _exe_name = "seqeyes.exe" if sys.platform == "win32" else "seqeyes"
    fake_bin = tmp_path / _exe_name
    fake_bin.touch()

    with patch.object(viewer, "_BUNDLED_EXE", fake_bin):
        result = _find_executable()

    assert result == str(fake_bin)


# ---------------------------------------------------------------------------
# No-argument call
# ---------------------------------------------------------------------------

def test_seqeyes_no_args_opens_gui(tmp_path):
    """Calling seqeyes() with no args should Popen the bare executable."""
    from seqeyes.viewer import seqeyes
    popen = _popen_mock()
    with patch("shutil.which", return_value=FAKE_EXE), \
         patch("subprocess.Popen", popen):
        seqeyes()
    popen.assert_called_once_with([FAKE_EXE])


# ---------------------------------------------------------------------------
# File path argument
# ---------------------------------------------------------------------------

def test_seqeyes_filepath(tmp_path):
    """seqeyes('file.seq') should Popen [exe, filepath]."""
    from seqeyes.viewer import seqeyes
    seq_file = tmp_path / "test.seq"
    seq_file.write_text("[VERSION]\nmajor 1\nminor 4\n")

    popen = _popen_mock()
    with patch("shutil.which", return_value=FAKE_EXE), \
         patch("subprocess.Popen", popen):
        seqeyes(str(seq_file))
    popen.assert_called_once_with([FAKE_EXE, str(seq_file)])


def test_seqeyes_filepath_not_found(tmp_path):
    """seqeyes() should raise FileNotFoundError for a missing .seq file."""
    from seqeyes.viewer import seqeyes
    with patch("shutil.which", return_value=FAKE_EXE):
        with pytest.raises(FileNotFoundError, match="not found"):
            seqeyes(str(tmp_path / "missing.seq"))


def test_seqeyes_pathlike(tmp_path):
    """seqeyes() should accept a pathlib.Path."""
    from seqeyes.viewer import seqeyes
    seq_file = tmp_path / "test.seq"
    seq_file.write_text("[VERSION]\nmajor 1\nminor 4\n")

    popen = _popen_mock()
    with patch("shutil.which", return_value=FAKE_EXE), \
         patch("subprocess.Popen", popen):
        seqeyes(seq_file)
    popen.assert_called_once_with([FAKE_EXE, str(seq_file)])


# ---------------------------------------------------------------------------
# Sequence object argument
# ---------------------------------------------------------------------------

def test_seqeyes_sequence_object(tmp_path):
    """seqeyes(seq) should write to a temp file and Popen [exe, tempfile]."""
    from seqeyes.viewer import seqeyes

    written_paths = []

    class FakeSeq:
        def write(self, path):
            written_paths.append(path)

    popen = _popen_mock()
    with patch("shutil.which", return_value=FAKE_EXE), \
         patch("subprocess.Popen", popen):
        seqeyes(FakeSeq())

    assert len(written_paths) == 1
    assert written_paths[0].endswith(".seq")
    popen.assert_called_once()
    call_args = popen.call_args[0][0]
    assert call_args[0] == FAKE_EXE
    assert call_args[-1] == written_paths[0]


# ---------------------------------------------------------------------------
# Options-only call
# ---------------------------------------------------------------------------

def test_seqeyes_help_option():
    """seqeyes('--help') should pass --help to the executable."""
    from seqeyes.viewer import seqeyes
    popen = _popen_mock()
    with patch("shutil.which", return_value=FAKE_EXE), \
         patch("subprocess.Popen", popen):
        seqeyes("--help")
    popen.assert_called_once_with([FAKE_EXE, "--help"])


def test_seqeyes_options_before_file(tmp_path):
    """seqeyes('--layout', '212', 'file.seq') passes options before filepath."""
    from seqeyes.viewer import seqeyes
    seq_file = tmp_path / "test.seq"
    seq_file.write_text("[VERSION]\nmajor 1\nminor 4\n")

    popen = _popen_mock()
    with patch("shutil.which", return_value=FAKE_EXE), \
         patch("subprocess.Popen", popen):
        seqeyes("--layout", "212", str(seq_file))
    popen.assert_called_once_with([FAKE_EXE, "--layout", "212", str(seq_file)])


# ---------------------------------------------------------------------------
# Bad argument
# ---------------------------------------------------------------------------

def test_seqeyes_bad_last_arg():
    """seqeyes() should raise TypeError for an unrecognised last argument."""
    from seqeyes.viewer import seqeyes
    with patch("shutil.which", return_value=FAKE_EXE):
        with pytest.raises(TypeError):
            seqeyes(42)
